#include "TiltedLayers.hpp"

#include "Config.hpp"
#include "I18N.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

double HeadClearance::max_tilt(const Vec2d &uphill) const
{
    // The print head is seen as a box whose lower edges are under the clearance angles of its sides: along a
    // diagonal, the corner of the box is further from the nozzle and the head clears less than along its sides.
    const double cx  = uphill.x() < 0. ? this->left : this->right;
    const double cy  = uphill.y() < 0. ? this->front : this->back;
    const double ux  = std::abs(uphill.x());
    const double uy  = std::abs(uphill.y());
    double       inv = 0.; // 1 / tan(clearance)
    if (ux > 1e-3) {
        if (cx <= 0.)
            return 0.;
        inv += ux / std::tan(cx * PI / 180.);
    }
    if (uy > 1e-3) {
        if (cy <= 0.)
            return 0.;
        inv += uy / std::tan(cy * PI / 180.);
    }
    return inv > 0. ? std::atan(1. / inv) * 180. / PI : 0.;
}

std::string head_side_name(const Vec2d &uphill)
{
    // As HeadClearance::max_tilt().
    const bool x = std::abs(uphill.x()) > 1e-3;
    const bool y = std::abs(uphill.y()) > 1e-3;
    const bool left  = uphill.x() < 0.;
    const bool front = uphill.y() < 0.;
    if (x && y)
        return front ? (left ? L("front left corner") : L("front right corner")) : (left ? L("back left corner") : L("back right corner"));
    if (y)
        return front ? L("front side") : L("back side");
    return left ? L("left side") : L("right side");
}

HeadClearance head_clearance(const std::string &printer_model, double left, double right, double front, double back)
{
    // Measured on photos of the print heads seen from the front: the front and back sides are not known yet.
    // The P1S has the print head of the X1 Carbon without its Micro LiDAR: its right side keeps the value of the
    // X1 Carbon until it is measured.
    struct Measured { const char *model; double left, right, front, back; };
    static const Measured measured[] = {
        { "Bambu Lab X1 Carbon", 15., 5.,  0., 0. },
        { "Bambu Lab P1S",       15., 5.,  0., 0. },
        { "Bambu Lab H2S",       12., 12., 0., 0. },
    };
    HeadClearance out;
    out.left  = left;
    out.right = right;
    out.front = front;
    out.back  = back;
    for (const Measured &m : measured)
        if (printer_model == m.model) {
            if (out.left <= 0.)
                out.left = m.left;
            if (out.right <= 0.)
                out.right = m.right;
            if (out.front <= 0.)
                out.front = m.front;
            if (out.back <= 0.)
                out.back = m.back;
        }
    return out;
}

HeadClearance head_clearance(const ConfigBase &printer_config)
{
    auto value = [&printer_config](const char *key) {
        const ConfigOptionFloat *opt = printer_config.option<ConfigOptionFloat>(key);
        return opt == nullptr ? 0. : opt->value;
    };
    const ConfigOptionString *model = printer_config.option<ConfigOptionString>("printer_model");
    return head_clearance(model == nullptr ? std::string() : model->value,
        value("nozzle_clearance_angle_left"), value("nozzle_clearance_angle_right"),
        value("nozzle_clearance_angle_front"), value("nozzle_clearance_angle_back"));
}

TiltedLayersSettings tilted_layers_settings(const ConfigBase &config)
{
    TiltedLayersSettings settings;
    auto value = [&config](const char *key) {
        const ConfigOptionFloat *opt = config.option<ConfigOptionFloat>(key);
        return opt == nullptr ? 0. : opt->value;
    };
    const ConfigOptionBool *enabled = config.option<ConfigOptionBool>("tilted_layers");
    settings.enabled      = enabled != nullptr && enabled->value;
    settings.angle        = value("tilted_layers_angle");
    settings.direction    = value("tilted_layers_direction");
    settings.start_height = value("tilted_layers_start_height");
    settings.end_height   = value("tilted_layers_end_height");
    return settings;
}

Vec2d tilted_layers_direction(double direction_deg, const Transform3d &object_trafo)
{
    const double a  = direction_deg * PI / 180.;
    const Vec3d  d3 = object_trafo.linear() * Vec3d(std::cos(a), std::sin(a), 0.);
    const Vec2d  d  = d3.head<2>();
    const double n  = d.norm();
    // An object turned on its side has its X Y plane vertical: the direction is then taken on the bed.
    return n < 1e-6 ? Vec2d(std::cos(a), std::sin(a)) : Vec2d(d / n);
}

double tilted_layers_direction_deg(const Vec2d &dir, const Transform3d &object_trafo)
{
    const Matrix3d linear = object_trafo.linear();
    const Vec3d    d3     = linear.inverse() * Vec3d(dir.x(), dir.y(), 0.);
    return (std::abs(d3.x()) < 1e-9 && std::abs(d3.y()) < 1e-9) ? std::atan2(dir.y(), dir.x()) * 180. / PI :
                                                                  std::atan2(d3.y(), d3.x()) * 180. / PI;
}

double TiltedLayersParams::slope(double zw) const
{
    if (! this->enabled || zw <= this->z0 || zw >= this->z3)
        return 0.;
    if (zw < this->z1)
        return this->t * (zw - this->z0) / (this->z1 - this->z0);
    if (zw <= this->z2)
        return this->t;
    return this->t * (this->z3 - zw) / (this->z3 - this->z2);
}

double TiltedLayersParams::offset(double zw) const
{
    if (! this->enabled || zw <= this->z0)
        return 0.;
    if (zw < this->z1)
        return this->slope(zw) * this->w_low;
    if (zw <= this->z2)
        return this->t * this->w_low;
    if (zw < this->z3)
        return this->t * this->w_low + (this->slope(zw) - this->t) * this->w_high;
    return this->t * (this->w_low - this->w_high);
}

double TiltedLayersParams::thickness_ratio(const Vec2d &pt, double zw) const
{
    if (! this->enabled)
        return 1.;
    if (zw > this->z0 && zw < this->z1)
        return 1. - this->t / (this->z1 - this->z0) * (this->w(pt) - this->w_low);
    if (zw > this->z2 && zw < this->z3)
        return 1. + this->t / (this->z3 - this->z2) * (this->w(pt) - this->w_high);
    return 1.;
}

double TiltedLayersParams::warped_z(const Vec2d &pt, double z) const
{
    if (! this->enabled)
        return z;
    const double w = this->w(pt);
    // G is linear in z' between the breakpoints: real heights of the breakpoints at w.
    const double g0 = this->real_z_w(w, this->z0);
    const double g1 = this->real_z_w(w, this->z1);
    const double g2 = this->real_z_w(w, this->z2);
    const double g3 = this->real_z_w(w, this->z3);
    // Searched from the top: far from the part crossed by a transition, its layers may cross each other (there is
    // no material there), the highest layer passing through the point is then taken.
    if (z >= g3)
        return this->z3 + (z - g3);
    if (g2 < g3 && z >= g2)
        return this->z2 + (z - g2) * (this->z3 - this->z2) / (g3 - g2);
    if (z >= g1 && z <= g2)
        return this->z1 + (z - g1);
    if (g0 != g1 && z >= std::min(g0, g1) && z <= std::max(g0, g1))
        return this->z0 + (z - g0) * (this->z1 - this->z0) / (g1 - g0);
    if (z <= g0)
        return z;
    // Between the flat layers and a crossed transition, not reached by any layer: take the top of the transition.
    return this->z1 + (z - g1);
}

indexed_triangle_set tilted_layers_object_mesh(const ModelObject &model_object, const Transform3d &object_trafo)
{
    indexed_triangle_set out;
    for (const ModelVolume *volume : model_object.volumes)
        if (volume->is_model_part()) {
            indexed_triangle_set its = volume->mesh().its;
            its_transform(its, object_trafo * volume->get_matrix(), true);
            its_merge(out, its);
        }
    return out;
}

// Range of w of the part of the surface of the mesh where z + k_lo w >= c_lo and z + k_hi w <= c_hi: the part
// between two planes leaning along w.
static bool surface_w_range(const indexed_triangle_set &mesh, const Vec2d &dir, double k_lo, double c_lo, double k_hi, double c_hi,
    double &w_lo, double &w_hi)
{
    w_lo = std::numeric_limits<double>::max();
    w_hi = std::numeric_limits<double>::lowest();
    auto w_of = [&dir](const Vec3d &p) { return p.x() * dir.x() + p.y() * dir.y(); };
    // Signed distances to the bounds, positive inside.
    auto d_lo = [&w_of, k_lo, c_lo](const Vec3d &p) { return p.z() + k_lo * w_of(p) - c_lo; };
    auto d_hi = [&w_of, k_hi, c_hi](const Vec3d &p) { return c_hi - p.z() - k_hi * w_of(p); };
    std::vector<Vec3d> polygon, clipped;
    auto clip = [&polygon, &clipped](const auto &distance) {
        clipped.clear();
        for (size_t i = 0; i < polygon.size(); ++ i) {
            const Vec3d &a  = polygon[i];
            const Vec3d &b  = polygon[i + 1 < polygon.size() ? i + 1 : 0];
            const double da = distance(a);
            const double db = distance(b);
            if (da >= 0.)
                clipped.push_back(a);
            if ((da >= 0.) != (db >= 0.))
                clipped.push_back(a + (b - a) * (da / (da - db)));
        }
        polygon.swap(clipped);
    };
    for (const stl_triangle_vertex_indices &f : mesh.indices) {
        const Vec3d p[3] = { mesh.vertices[f[0]].cast<double>(), mesh.vertices[f[1]].cast<double>(), mesh.vertices[f[2]].cast<double>() };
        if ((d_lo(p[0]) < 0. && d_lo(p[1]) < 0. && d_lo(p[2]) < 0.) || (d_hi(p[0]) < 0. && d_hi(p[1]) < 0. && d_hi(p[2]) < 0.))
            continue;
        polygon.assign(p, p + 3);
        clip(d_lo);
        clip(d_hi);
        for (const Vec3d &q : polygon) {
            const double w = w_of(q);
            w_lo = std::min(w_lo, w);
            w_hi = std::max(w_hi, w);
        }
    }
    return w_lo <= w_hi;
}

TiltedLayersParams tilted_layers_params(const TiltedLayersSettings &settings, const indexed_triangle_set &mesh,
    const Transform3d &object_trafo, double first_layer_height, double layer_height, double max_layer_height,
    const HeadClearance *clearance)
{
    TiltedLayersParams params;
    if (! settings.enabled || mesh.vertices.empty())
        return params;
    params.dir   = tilted_layers_direction(settings.direction, object_trafo);
    double angle = std::clamp(settings.angle, 0., TILTED_LAYERS_MAX_ANGLE);
    // Flat on purpose: nothing to check.
    if (angle < 0.1)
        return params;
    if (clearance != nullptr) {
        // The side of the print head facing uphill passes over the part printed so far.
        const double max_tilt = clearance->max_tilt(- params.dir);
        if (max_tilt <= 0.) {
            params.clearance_unknown = true;
            return params;
        }
        const double max_angle = std::max(0., max_tilt - TILTED_LAYERS_CLEARANCE_MARGIN);
        if (angle > max_angle + EPSILON) {
            angle                = max_angle;
            params.angle_limited = true;
        }
    }
    if (angle < 0.1)
        return params;
    params.angle   = angle * PI / 180.;
    params.t       = std::tan(params.angle);
    const double t = params.t;

    double z_max = std::numeric_limits<double>::lowest();
    params.w_min = std::numeric_limits<double>::max();
    params.w_max = std::numeric_limits<double>::lowest();
    for (const stl_vertex &v : mesh.vertices) {
        const Vec2d  pt = v.head<2>().cast<double>();
        const double w  = params.w(pt);
        params.w_min = std::min(params.w_min, w);
        params.w_max = std::max(params.w_max, w);
        params.bbox_xy.merge(pt);
        z_max = std::max(z_max, double(v.z()));
    }
    params.real_max_z = z_max;
    params.w_center   = 0.5 * (params.w_min + params.w_max);
    const double w_c  = params.w_center;
    // Heights of the tilted layers touching the bottom and the top of the object.
    params.bottom_height = std::numeric_limits<double>::max();
    params.top_height    = std::numeric_limits<double>::lowest();
    for (const stl_vertex &v : mesh.vertices) {
        const double height = double(v.z()) + t * (params.w(v.head<2>().cast<double>()) - w_c);
        params.bottom_height = std::min(params.bottom_height, height);
        params.top_height    = std::max(params.top_height, height);
    }

    // A transition of length L pivoting at w_p changes the thickness of its layers by t |w - w_p| / L at w: thinner
    // on one side of the pivot, thicker on the other side. The layers may get at most half thinner, and thicker up
    // to the max layer height of the nozzle, at most by half: the pivot shares the part crossed by the transition
    // between both sides in these proportions, for the shortest transition. The transitions are made for a total
    // change of 50 % when there is room, shorter down to the largest change allowed.
    const double h           = std::max(layer_height, 0.01);
    const double thinner     = std::clamp(1. - 0.02 / h, 0.2, 0.5);
    const double thicker     = max_layer_height > 0. ? std::clamp(max_layer_height / h - 1., 0., 0.5) : 0.5;
    const double max_change  = thinner + thicker;
    const double auto_change = std::min(max_change, 0.5);
    const double shortest    = 2. * h;

    // Range of w of the part crossed by a transition.
    struct Range
    {
        double lo { std::numeric_limits<double>::max() };
        double hi { std::numeric_limits<double>::lowest() };
        bool   empty() const { return lo > hi; }
    };
    auto merge = [&mesh, &params](Range &range, double k_lo, double c_lo, double k_hi, double c_hi) {
        double lo, hi;
        if (surface_w_range(mesh, params.dir, k_lo, c_lo, k_hi, c_hi, lo, hi)) {
            range.lo = std::min(range.lo, lo);
            range.hi = std::max(range.hi, hi);
        }
    };
    // Has the range stopped growing?
    auto contains = [](const Range &range, const Range &other) {
        return other.empty() || (! range.empty() && other.lo > range.lo - 1e-3 && other.hi < range.hi + 1e-3);
    };

    // Lower transition, from the flat layer z0 to the first tilted layer z = start - t (w - w_c), which passes through
    // its pivot at z1. It crosses the part between both layers, on both sides of the line where they cross. As this
    // part depends on the pivot and on the length, it is grown from the part just under the first tilted layer. The
    // start of the zone is raised if the transition does not fit above the first layer. The heights of the zone are
    // not negative: 0 is kept for an end of the zone at the top of the object.
    const double lowest_start = std::max(0., params.bottom_height);
    double       start        = std::clamp(settings.start_height, lowest_start, std::max(lowest_start, params.top_height));
    const double start_asked  = start;
    {
        Range  range;
        double w_low  = w_c;
        double length = shortest;
        merge(range, t, start + t * w_c - shortest, t, start + t * w_c);
        for (int iter = 0; iter < 10; ++ iter) {
            double min_length  = shortest;
            double auto_length = shortest;
            w_low = w_c;
            if (! range.empty()) {
                w_low       = (thicker * range.hi + thinner * range.lo) / max_change;
                min_length  = std::max(shortest, t * (range.hi - range.lo) / max_change);
                auto_length = std::max(shortest, t * (range.hi - range.lo) / auto_change);
            }
            double       z1   = start + t * (w_c - w_low);
            const double room = z1 - first_layer_height;
            if (room >= auto_length) {
                length = auto_length;
            } else if (room >= min_length) {
                length = room;
            } else {
                length = min_length;
                start += min_length - room;
                z1     = start + t * (w_c - w_low);
            }
            const double z0   = z1 - length;
            Range        next = range;
            // z >= z0 and z <= z1 - t (w - w_low)
            merge(next, 0., z0, t, z1 + t * w_low);
            // z >= z1 - t (w - w_low) and z <= z0: where the transition would cross the flat layers.
            merge(next, t, z1 + t * w_low, 0., z0);
            if (contains(range, next))
                break;
            range = next;
        }
        params.w_low          = w_low;
        params.low_transition = length;
    }
    params.start_height = start;
    params.start_raised = start > start_asked + 1e-3;
    params.z1           = start + t * (w_c - params.w_low);
    params.z0           = params.z1 - params.low_transition;

    // Upper transition, from the last tilted layer z = end - t (w - w_c) to the flat layer z3, pivoting at w_high:
    // grown in the same way from the part just above the last tilted layer. Nothing to cross when the zone reaches
    // the top of the object.
    const double end = std::max(start, settings.end_height > 0. ? std::min(settings.end_height, params.top_height) : params.top_height);
    {
        Range  range;
        double w_high = w_c;
        double length = shortest;
        merge(range, t, end + t * w_c, t, end + t * w_c + shortest);
        for (int iter = 0; iter < 10 && ! range.empty(); ++ iter) {
            w_high = (thinner * range.hi + thicker * range.lo) / max_change;
            length = std::max(shortest, t * (range.hi - range.lo) / auto_change);
            // Height of the flat layer ending the transition.
            const double z_flat = end - t * (w_high - w_c) + length;
            Range        next   = range;
            // z >= end - t (w - w_c) and z <= z_flat
            merge(next, t, end + t * w_c, 0., z_flat);
            // z >= z_flat and z <= end - t (w - w_c): where the transition would cross the flat layers.
            merge(next, 0., z_flat, t, end + t * w_c);
            if (contains(range, next))
                break;
            range = next;
        }
        params.w_high          = w_high;
        params.high_transition = length;
    }
    params.end_height = end;
    params.z2         = end + t * (w_c - params.w_low);
    params.z3         = params.z2 + params.high_transition;

    params.enabled   = true;
    params.tolerance = std::clamp(0.05 * h, 0.002, 0.02);
    // The warp is linear in each triangle outside the transitions and nearly linear inside them (where the meshes
    // are refined): the highest vertex, plus a layer, bounds the top of the warped part.
    double warped_max = std::numeric_limits<double>::lowest();
    for (const stl_vertex &v : mesh.vertices)
        warped_max = std::max(warped_max, params.warped_z(v.head<2>().cast<double>(), double(v.z())));
    params.warped_max_z = warped_max + h;
    return params;
}

indexed_triangle_set tilted_layers_warp_mesh(const indexed_triangle_set &its_in, const TiltedLayersParams &params)
{
    indexed_triangle_set its = its_in;
    if (! params.enabled)
        return its;
    // The face properties would not match the split triangles, and the slicer does not use them.
    its.properties.clear();

    // Zones of the warped heights: the warp is linear in the zones 0 (flat), 2 (tilted) and 4 (flat again), which
    // are convex, and not in the transitions 1 and 3.
    auto zone = [&params](double zw) -> int8_t {
        return zw <= params.z0 ? 0 : zw < params.z1 ? 1 : zw <= params.z2 ? 2 : zw < params.z3 ? 3 : 4;
    };
    const double tolerance = params.tolerance;
    // Static: read by the lambda below without being captured.
    static constexpr double min_edge2 = 0.05 * 0.05;
    std::vector<double> zws;
    std::vector<int8_t> zones;
    // Each pass splits at their middle the edges that the warp bends too much, the edge to midpoint map keeps the
    // neighbouring triangles connected.
    for (int pass = 0; pass < 20; ++ pass) {
        zws.resize(its.vertices.size());
        zones.resize(its.vertices.size());
        for (size_t i = 0; i < its.vertices.size(); ++ i) {
            const stl_vertex &v = its.vertices[i];
            zws[i]   = params.warped_z(v.head<2>().cast<double>(), double(v.z()));
            zones[i] = zone(zws[i]);
        }
        // a < b, the same edge is always tested in the same order.
        auto bent = [&its, &params, &zws, &zones, tolerance](int a, int b) {
            if (zones[a] == zones[b] && zones[a] % 2 == 0)
                return false;
            const Vec3d pa = its.vertices[a].cast<double>();
            const Vec3d pb = its.vertices[b].cast<double>();
            if ((pb - pa).squaredNorm() < min_edge2)
                return false;
            for (const double f : { 0.25, 0.5, 0.75 }) {
                const Vec3d p = pa + (pb - pa) * f;
                if (std::abs(params.warped_z(p.head<2>(), p.z()) - (zws[a] + (zws[b] - zws[a]) * f)) > tolerance)
                    return true;
            }
            return false;
        };
        std::unordered_map<uint64_t, int> midpoints;
        auto midpoint = [&its, &midpoints, &bent](int a, int b) -> int {
            const int      lo  = std::min(a, b);
            const int      hi  = std::max(a, b);
            const uint64_t key = (uint64_t(uint32_t(lo)) << 32) | uint64_t(uint32_t(hi));
            auto it = midpoints.find(key);
            if (it != midpoints.end())
                return it->second;
            if (! bent(lo, hi))
                return -1;
            const int        idx = int(its.vertices.size());
            const stl_vertex mid = (0.5f * (its.vertices[lo] + its.vertices[hi])).eval();
            its.vertices.emplace_back(mid);
            midpoints.emplace(key, idx);
            return idx;
        };

        std::vector<stl_triangle_vertex_indices> out;
        out.reserve(its.indices.size() + its.indices.size() / 4);
        bool split = false;
        for (const stl_triangle_vertex_indices &f : its.indices) {
            const int v0 = f[0], v1 = f[1], v2 = f[2];
            const int m0 = midpoint(v0, v1);
            const int m1 = midpoint(v1, v2);
            const int m2 = midpoint(v2, v0);
            const int mask = (m0 >= 0 ? 1 : 0) | (m1 >= 0 ? 2 : 0) | (m2 >= 0 ? 4 : 0);
            // The triangles below keep the orientation of the original triangle.
            switch (mask) {
            case 0:
                out.emplace_back(v0, v1, v2);
                break;
            case 1:
                out.emplace_back(v0, m0, v2);
                out.emplace_back(m0, v1, v2);
                break;
            case 2:
                out.emplace_back(v0, v1, m1);
                out.emplace_back(v0, m1, v2);
                break;
            case 4:
                out.emplace_back(v0, v1, m2);
                out.emplace_back(m2, v1, v2);
                break;
            case 3:
                out.emplace_back(m0, v1, m1);
                out.emplace_back(v0, m0, m1);
                out.emplace_back(v0, m1, v2);
                break;
            case 6:
                out.emplace_back(m1, v2, m2);
                out.emplace_back(v0, v1, m1);
                out.emplace_back(v0, m1, m2);
                break;
            case 5:
                out.emplace_back(v0, m0, m2);
                out.emplace_back(m0, v1, v2);
                out.emplace_back(m0, v2, m2);
                break;
            default: // 7
                out.emplace_back(v0, m0, m2);
                out.emplace_back(m0, v1, m1);
                out.emplace_back(m2, m1, v2);
                out.emplace_back(m0, m1, m2);
                break;
            }
            split |= mask != 0;
        }
        its.indices = std::move(out);
        if (! split)
            break;
    }

    for (stl_vertex &v : its.vertices)
        v.z() = float(params.warped_z(v.head<2>().cast<double>(), double(v.z())));
    return its;
}

} // namespace Slic3r
