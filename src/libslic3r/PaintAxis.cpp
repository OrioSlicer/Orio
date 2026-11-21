// Orio: painting a quantity along an axis of an object, see PaintAxis.hpp.

#include "PaintAxis.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <limits>

#include "Model.hpp"
#include "Print.hpp"

namespace Slic3r {

const char* paint_axis_name(PaintAxis axis)
{
    switch (axis) {
    case paX: return "x";
    case paY: return "y";
    default:  return "z";
    }
}

bool paint_axis_from_name(const std::string &name, PaintAxis &axis)
{
    if (name.size() != 1)
        return false;
    switch (std::tolower((unsigned char)name.front())) {
    case 'x': axis = paX; return true;
    case 'y': axis = paY; return true;
    case 'z': axis = paZ; return true;
    default:  return false;
    }
}

std::pair<double, double> paint_axis_object_extent(const ModelObject &object, PaintAxis axis)
{
    const int idx = (axis == paX) ? 0 : (axis == paY) ? 1 : 2;
    BoundingBoxf3 bbox;
    for (const ModelVolume *volume : object.volumes)
        if (volume->is_model_part())
            bbox.merge(volume->get_convex_hull().transformed_bounding_box(volume->get_matrix()));
    if (! bbox.defined)
        return { 0., 0. };
    return { bbox.min(idx), bbox.max(idx) };
}

// ------------------------------------------------------------------------------------------------
// PaintAxisFrame
// ------------------------------------------------------------------------------------------------

PaintAxisFrame::PaintAxisFrame(const PrintObject &object, PaintAxis axis) : PaintAxisFrame(object.trafo_centered(), axis) {}

PaintAxisFrame::PaintAxisFrame(const Transform3d &trafo_centered, PaintAxis axis)
{
    // Slices are the volumes transformed by trafo_centered() * volume matrix: the inverse brings a point of a layer back
    // into the frame of the object.
    const Transform3d inverse = trafo_centered.inverse();
    const int         row     = (axis == paX) ? 0 : (axis == paY) ? 1 : 2;
    m_row    = inverse.linear().row(row).transpose();
    m_offset = inverse.translation()(row);
}

PaintAxisFrame::Plane PaintAxisFrame::plane(double slice_z) const
{
    Plane plane;
    plane.gx = m_row.x() * SCALING_FACTOR;
    plane.gy = m_row.y() * SCALING_FACTOR;
    plane.c  = m_row.z() * slice_z + m_offset;
    return plane;
}

std::pair<double, double> PaintAxisFrame::Plane::range(const BoundingBox &bbox) const
{
    const double c[4] = {
        this->at(double(bbox.min.x()), double(bbox.min.y())),
        this->at(double(bbox.max.x()), double(bbox.min.y())),
        this->at(double(bbox.max.x()), double(bbox.max.y())),
        this->at(double(bbox.min.x()), double(bbox.max.y())) };
    return { *std::min_element(c, c + 4), *std::max_element(c, c + 4) };
}

Polygon PaintAxisFrame::Plane::band(const BoundingBox &bbox, double from, double to) const
{
    if (! bbox.defined || to <= from)
        return {};
    const std::pair<double, double> span = this->range(bbox);
    if (span.second - span.first < 1e-4) {
        // The axis is perpendicular to the layer: the coordinate is the same everywhere on it.
        const double c = 0.5 * (span.first + span.second);
        return (from <= c && c < to) ? bbox.polygon() : Polygon();
    }
    // Unit vector u along which the coordinate grows on the plane, and the unit vector w at 90 degrees: (u, w) is a
    // rotation of (x, y), a rectangle walked counter-clockwise in (u, w) is counter-clockwise in (x, y) as well.
    const double norm = std::sqrt(this->gx * this->gx + this->gy * this->gy);
    const double ux   = this->gx / norm;
    const double uy   = this->gy / norm;
    const double wx   = - uy;
    const double wy   = ux;
    // Extents of the bounding box along u and w.
    double s_min = std::numeric_limits<double>::max(), s_max = std::numeric_limits<double>::lowest();
    double t_min = s_min, t_max = s_max;
    for (const Point &corner : { bbox.min, Point(bbox.max.x(), bbox.min.y()), bbox.max, Point(bbox.min.x(), bbox.max.y()) }) {
        const double s = ux * double(corner.x()) + uy * double(corner.y());
        const double t = wx * double(corner.x()) + wy * double(corner.y());
        s_min = std::min(s_min, s);
        s_max = std::max(s_max, s);
        t_min = std::min(t_min, t);
        t_max = std::max(t_max, t);
    }
    // The coordinate at s along u is norm * s + c.
    const double s0 = std::max(s_min, (from - this->c) / norm);
    const double s1 = std::min(s_max, (to - this->c) / norm);
    if (s1 - s0 < 1.)
        return {};
    auto to_point = [ux, uy, wx, wy](double s, double t) {
        return Point(coord_t(std::round(s * ux + t * wx)), coord_t(std::round(s * uy + t * wy)));
    };
    Polygon out;
    out.points = { to_point(s0, t_min), to_point(s1, t_min), to_point(s1, t_max), to_point(s0, t_max) };
    return out;
}

// ------------------------------------------------------------------------------------------------
// Bands
// ------------------------------------------------------------------------------------------------

PaintAxisBands paint_axis_bands(const std::vector<double> &curve, bool integer_values, double tolerance, double min_width)
{
    PaintAxisBands bands;
    if (curve.size() < 4 || (curve.size() & 1) != 0)
        return bands;
    // Points of the curve, the coordinates strictly increasing (a malformed project may hold anything).
    std::vector<std::pair<double, double>> points;
    points.reserve(curve.size() / 2);
    for (size_t i = 0; i + 1 < curve.size(); i += 2) {
        if (! std::isfinite(curve[i]) || ! std::isfinite(curve[i + 1]))
            continue;
        if (points.empty() || curve[i] > points.back().first)
            points.emplace_back(curve[i], curve[i + 1]);
        else
            points.back().second = curve[i + 1];
    }
    if (points.empty())
        return bands;
    const double start = points.front().first;
    const double end   = points.back().first;
    if (points.size() == 1 || end - start < EPSILON) {
        const double v = integer_values ? std::round(points.front().second) : points.front().second;
        bands.push_back({ - PAINT_AXIS_INFINITY, PAINT_AXIS_INFINITY, v });
        return bands;
    }

    // Sample the curve finely: the band boundaries land within half a step of the exact ones.
    const double step    = std::max(0.05, (end - start) / 8000.);
    const size_t samples = size_t(std::ceil((end - start) / step)) + 1;
    size_t       segment = 0;
    auto value_at = [&points, &segment](double x) {
        while (segment + 2 < points.size() && points[segment + 1].first < x)
            ++ segment;
        const std::pair<double, double> &a = points[segment];
        const std::pair<double, double> &b = points[segment + 1];
        const double t = std::clamp((x - a.first) / (b.first - a.first), 0., 1.);
        return a.second + t * (b.second - a.second);
    };

    double x_prev   = start;
    double band_lo  = value_at(start);
    double band_hi  = band_lo;
    double band_from = start;
    double band_key = integer_values ? std::round(band_lo) : 0.;
    for (size_t i = 1; i < samples; ++ i) {
        const double x = std::min(end, start + double(i) * step);
        const double v = value_at(x);
        bool         new_band;
        if (integer_values)
            new_band = std::round(v) != band_key;
        else
            new_band = std::max(band_hi, v) - std::min(band_lo, v) > 2. * tolerance;
        if (new_band) {
            const double boundary = 0.5 * (x_prev + x);
            bands.push_back({ band_from, boundary, integer_values ? band_key : 0.5 * (band_lo + band_hi) });
            band_from = boundary;
            band_lo = band_hi = v;
            band_key = integer_values ? std::round(v) : 0.;
        } else {
            band_lo = std::min(band_lo, v);
            band_hi = std::max(band_hi, v);
        }
        x_prev = x;
    }
    bands.push_back({ band_from, end, integer_values ? band_key : 0.5 * (band_lo + band_hi) });

    // Merge the bands too narrow to print properly into their neighbour of closest value, narrowest first.
    while (bands.size() > 1) {
        size_t narrowest = size_t(-1);
        double width_min = min_width;
        for (size_t i = 0; i < bands.size(); ++ i)
            if (const double width = bands[i].to - bands[i].from; width < width_min) {
                width_min = width;
                narrowest = i;
            }
        if (narrowest == size_t(-1))
            break;
        size_t into;
        if (narrowest == 0)
            into = 1;
        else if (narrowest + 1 == bands.size())
            into = narrowest - 1;
        else {
            const double v       = bands[narrowest].value;
            const double d_left  = std::abs(bands[narrowest - 1].value - v);
            const double d_right = std::abs(bands[narrowest + 1].value - v);
            into = (d_left < d_right || (d_left == d_right && bands[narrowest - 1].value > bands[narrowest + 1].value)) ? narrowest - 1 : narrowest + 1;
        }
        bands[into].from = std::min(bands[into].from, bands[narrowest].from);
        bands[into].to   = std::max(bands[into].to,   bands[narrowest].to);
        bands.erase(bands.begin() + narrowest);
        // Neighbours of the same value become one band.
        for (size_t i = 1; i < bands.size();)
            if (bands[i].value == bands[i - 1].value) {
                bands[i - 1].to = bands[i].to;
                bands.erase(bands.begin() + i);
            } else
                ++ i;
    }

    bands.front().from = - PAINT_AXIS_INFINITY;
    bands.back().to    = PAINT_AXIS_INFINITY;
    return bands;
}

size_t paint_axis_band_index(const PaintAxisBands &bands, double coordinate)
{
    assert(! bands.empty());
    // First band ending after the coordinate.
    auto it = std::upper_bound(bands.begin(), bands.end(), coordinate, [](double c, const PaintAxisBand &band) { return c < band.to; });
    return (it == bands.end()) ? bands.size() - 1 : size_t(it - bands.begin());
}

std::vector<double> paint_axis_band_boundaries(const PaintAxisBands &bands)
{
    std::vector<double> out;
    if (! bands.empty()) {
        out.reserve(bands.size() - 1);
        for (size_t i = 0; i + 1 < bands.size(); ++ i)
            out.push_back(bands[i].to);
    }
    return out;
}

Points paint_axis_cut_points(const Points &points, const std::vector<PaintAxisCut> &cuts)
{
    Points out;
    if (points.empty())
        return out;
    out.reserve(points.size() + points.size() / 2);
    out.push_back(points.front());
    std::vector<double> ts;
    for (size_t i = 1; i < points.size(); ++ i) {
        const Point &a = points[i - 1];
        const Point &b = points[i];
        ts.clear();
        for (const PaintAxisCut &cut : cuts) {
            const double ca = cut.plane(a);
            const double cb = cut.plane(b);
            if (ca == cb)
                continue;
            const double lo = std::min(ca, cb);
            const double hi = std::max(ca, cb);
            for (auto it = std::upper_bound(cut.boundaries.begin(), cut.boundaries.end(), lo); it != cut.boundaries.end() && *it < hi; ++ it)
                ts.push_back((*it - ca) / (cb - ca));
        }
        if (! ts.empty()) {
            std::sort(ts.begin(), ts.end());
            const Vec2d ad = a.cast<double>();
            const Vec2d ab = (b - a).cast<double>();
            for (double t : ts) {
                const Vec2d  pd = ad + t * ab;
                const Point  p(coord_t(std::round(pd.x())), coord_t(std::round(pd.y())));
                if (p != out.back() && p != b)
                    out.push_back(p);
            }
        }
        if (b != out.back())
            out.push_back(b);
    }
    return out;
}

} // namespace Slic3r
