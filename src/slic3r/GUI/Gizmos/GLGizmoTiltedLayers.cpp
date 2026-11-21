// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code, which overrides our localization "L" macro.
#include "GLGizmoTiltedLayers.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Slicing.hpp"

#include <imgui/imgui_internal.h>
#include <GL/glew.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Slic3r {
namespace GUI {

static const std::array<float, 4> TRANSITION_COLOR    { 0.94f, 0.60f, 0.48f, 0.55f };
static const std::array<float, 4> FIXED_COLOR         { 0.30f, 0.78f, 0.36f, 0.85f };
static const std::array<float, 4> UNSUPPORTED_COLOR   { 0.90f, 0.25f, 0.20f, 0.85f };
static const std::array<float, 4> HOVER_COLOR         { 1.00f, 0.85f, 0.20f, 0.90f };
static const std::array<float, 4> PLANE_COLOR         { 0.11f, 0.62f, 0.46f, 0.30f };
// The print head does not clear on the side the layers rise towards: they will not be tilted.
static const std::array<float, 4> PLANE_WARNING_COLOR { 0.90f, 0.30f, 0.30f, 0.30f };
// The overlays are lifted off the surface of the object along its normal, not to fight with it.
static constexpr double OVERLAY_OFFSET = 0.03;

static void add_triangle(indexed_triangle_set &its, const Vec3d p[3], const Vec3d &offset)
{
    const int base = int(its.vertices.size());
    for (int i = 0; i < 3; ++ i)
        its.vertices.emplace_back((p[i] + offset).cast<float>());
    its.indices.emplace_back(base, base + 1, base + 2);
}

// Part of the triangle p where the value s, linear over the triangle, is between lo and hi.
static void add_clipped_triangle(indexed_triangle_set &its, const Vec3d p[3], const double s[3], double lo, double hi, const Vec3d &offset)
{
    if (std::max(s[0], std::max(s[1], s[2])) < lo || std::min(s[0], std::min(s[1], s[2])) > hi)
        return;
    using Vertex = std::pair<Vec3d, double>;
    auto clip = [](const std::vector<Vertex> &in, double bound, bool keep_above) {
        std::vector<Vertex> out;
        for (size_t i = 0; i < in.size(); ++ i) {
            const Vertex &a  = in[i];
            const Vertex &b  = in[i + 1 < in.size() ? i + 1 : 0];
            const bool    ia = keep_above ? a.second >= bound : a.second <= bound;
            const bool    ib = keep_above ? b.second >= bound : b.second <= bound;
            if (ia)
                out.push_back(a);
            if (ia != ib) {
                const double t = (bound - a.second) / (b.second - a.second);
                out.emplace_back(a.first + (b.first - a.first) * t, bound);
            }
        }
        return out;
    };
    std::vector<Vertex> polygon { { p[0], s[0] }, { p[1], s[1] }, { p[2], s[2] } };
    polygon = clip(polygon, lo, true);
    if (polygon.size() < 3)
        return;
    polygon = clip(polygon, hi, false);
    if (polygon.size() < 3)
        return;
    const int base = int(its.vertices.size());
    for (const Vertex &v : polygon)
        its.vertices.emplace_back((v.first + offset).cast<float>());
    for (int i = 1; i + 1 < int(polygon.size()); ++ i)
        its.indices.emplace_back(base, base + i, base + i + 1);
}

// Rectangle origin +- u +- v.
static indexed_triangle_set make_quad(const Vec3d &origin, const Vec3d &u, const Vec3d &v)
{
    indexed_triangle_set its;
    its.vertices.emplace_back((origin - u - v).cast<float>());
    its.vertices.emplace_back((origin + u - v).cast<float>());
    its.vertices.emplace_back((origin + u + v).cast<float>());
    its.vertices.emplace_back((origin - u + v).cast<float>());
    its.indices.emplace_back(0, 1, 2);
    its.indices.emplace_back(0, 2, 3);
    return its;
}

// Side of the bed the overhangs point to, dir on the bed.
static wxString overhangs_side(const Vec2d &dir)
{
    const double deg    = std::atan2(dir.y(), dir.x()) * 180. / PI;
    const int    sector = (int(std::floor((deg + 22.5) / 45.)) % 8 + 8) % 8;
    switch (sector) {
    case 0:  return _L("Overhangs towards the right");
    case 1:  return _L("Overhangs towards the back right");
    case 2:  return _L("Overhangs towards the back");
    case 3:  return _L("Overhangs towards the back left");
    case 4:  return _L("Overhangs towards the left");
    case 5:  return _L("Overhangs towards the front left");
    case 6:  return _L("Overhangs towards the front");
    default: return _L("Overhangs towards the front right");
    }
}

GLGizmoTiltedLayers::GLGizmoTiltedLayers(GLCanvas3D &parent, unsigned int sprite_id)
    : GLGizmoBase(parent, sprite_id)
{
}

bool GLGizmoTiltedLayers::on_init()
{
    m_shortcut_key = WXK_CONTROL_I;
    // The planes are moved along Z with their grabbers.
    m_grabbers.push_back(Grabber());
    m_grabbers.push_back(Grabber());
    return true;
}

std::string GLGizmoTiltedLayers::get_icon_filename(bool is_dark_mode) const
{
    return is_dark_mode ? "toolbar_tilted_layers_dark.svg" : "toolbar_tilted_layers.svg";
}

std::string GLGizmoTiltedLayers::get_gizmo_entering_text() const
{
    return _u8L("Entering tilted layers");
}

std::string GLGizmoTiltedLayers::get_gizmo_leaving_text() const
{
    return _u8L("Leaving tilted layers");
}

std::string GLGizmoTiltedLayers::on_get_name() const
{
    // Shown when the mouse is over the tool in the toolbar: the tool is still in beta.
    if (! on_is_activable() && m_state == EState::Off)
        return _u8L("Tilted layers (Beta)") + ":\n" + _u8L("Please select single object.");
    return _u8L("Tilted layers (Beta)");
}

bool GLGizmoTiltedLayers::on_is_activable() const
{
    return wxGetApp().plater()->printer_technology() == ptFFF && m_parent.get_selection().is_single_full_instance();
}

CommonGizmosDataID GLGizmoTiltedLayers::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::Raycaster));
}

void GLGizmoTiltedLayers::on_set_state()
{
    // Reload the object, its settings and the printer settings when the tool is opened, forget them when it is closed.
    m_object         = nullptr;
    m_hover_zw       = -1.;
    m_edit_snapshot  = false;
    m_no_overhangs   = false;
    m_overlays_dirty = true;
    m_transition_model.reset();
    m_fixed_model.reset();
    m_unsupported_model.reset();
    m_hover_model.reset();
    m_start_plane.reset();
    m_end_plane.reset();
    if (m_state == On)
        this->update_object();
    else
        m_mesh.clear();
    GLGizmoBase::on_set_state();
}

void GLGizmoTiltedLayers::data_changed(bool is_serializing)
{
    this->update_object();
    m_parent.set_as_dirty();
}

void GLGizmoTiltedLayers::read_printer_settings()
{
    const DynamicPrintConfig &printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const DynamicPrintConfig &print   = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    m_clearance = head_clearance(printer);
    auto print_value = [this, &print](const char *key, double default_value) {
        // The object may override the print settings.
        const ConfigOption *opt = m_object != nullptr ? m_object->config.option(key) : nullptr;
        if (opt == nullptr)
            opt = print.option(key);
        if (opt == nullptr)
            return default_value;
        switch (opt->type()) {
        case coFloat: return opt->getFloat();
        case coInt:   return double(opt->getInt());
        default:      return default_value;
        }
    };
    m_first_layer_height = print_value("initial_layer_print_height", 0.2);
    m_layer_height       = print_value("layer_height", 0.2);
    m_support_angle      = print_value("support_threshold_angle", 30.);
    // As the slicing parameters: the thickest layer of the nozzle, 3/4 of its diameter if not set, never below the
    // layer height.
    m_max_layer_height = std::max(Slicing::max_layer_height_from_nozzle(printer, 1), m_layer_height);
}

void GLGizmoTiltedLayers::default_settings()
{
    m_settings = TiltedLayersSettings();
    // Overhangs towards the side where the print head clears the most, until the overhangs of the object are found.
    const Vec2d best = this->best_direction(Vec2d(1., 0.));
    this->set_bed_direction(best.squaredNorm() > 0.5 ? best : Vec2d(1., 0.));
    const double max_angle = this->max_angle();
    m_settings.angle        = max_angle > 0. ? std::min(max_angle, 10.) : 10.;
    m_settings.start_height = m_mesh_box.defined ? 0.25 * m_mesh_box.max.z() : 0.;
    m_settings.end_height   = 0.;
    this->fit_overhangs();
}

bool GLGizmoTiltedLayers::fit_overhangs()
{
    // Overhangs needing support with flat layers, but the bottom on the bed.
    const double sin_critical = std::sin((90. - std::clamp(m_support_angle, 1., 89.)) * PI / 180.);
    std::vector<std::pair<size_t, Vec3d>> overhangs; // face, unit normal
    Vec2d  sum   = Vec2d::Zero();
    double total = 0.;
    for (size_t i = 0; i < m_mesh.indices.size(); ++ i) {
        const stl_triangle_vertex_indices &f = m_mesh.indices[i];
        const Vec3d p[3] = { m_mesh.vertices[f[0]].cast<double>(), m_mesh.vertices[f[1]].cast<double>(), m_mesh.vertices[f[2]].cast<double>() };
        if (std::max(p[0].z(), std::max(p[1].z(), p[2].z())) <= m_first_layer_height + EPSILON)
            continue;
        Vec3d        n    = (p[1] - p[0]).cross(p[2] - p[0]);
        const double norm = n.norm();
        if (norm < EPSILON)
            continue;
        n /= norm;
        if (- n.z() <= sin_critical)
            continue;
        overhangs.emplace_back(i, n);
        sum   += 0.5 * norm * Vec2d(n.x(), n.y());
        total += 0.5 * norm;
    }
    if (overhangs.empty())
        return false;
    // Towards the overhangs if they mostly lean the same way, which flat ceilings do not show. Onto the nearest side
    // where the print head clears when a few degrees away: the clearance of the other sides is often unknown.
    if (sum.norm() > 0.05 * total) {
        const Vec2d mean     = sum.normalized();
        Vec2d       dir      = mean;
        double      best_dot = std::cos(15. * PI / 180.);
        for (int deg = -180; deg < 180; ++ deg) {
            const double a = double(deg) * PI / 180.;
            const Vec2d  side(std::cos(a), std::sin(a));
            if (side.dot(mean) > best_dot && m_clearance.max_tilt(- side) > 0.) {
                best_dot = side.dot(mean);
                dir      = side;
            }
        }
        this->set_bed_direction(dir);
    }

    const Vec2d  dir = this->bed_direction();
    const double t   = std::tan(this->preview_angle() * PI / 180.);
    // Middle of the object along the direction, where the heights of the zone are measured (see TiltedLayersParams).
    double w_min = std::numeric_limits<double>::max();
    double w_max = std::numeric_limits<double>::lowest();
    for (const stl_vertex &v : m_mesh.vertices) {
        const double w = double(v.x()) * dir.x() + double(v.y()) * dir.y();
        w_min = std::min(w_min, w);
        w_max = std::max(w_max, w);
    }
    const double w_c = 0.5 * (w_min + w_max);
    // The zone goes from the tilted layer under the lowest overhang made printable by the tilt to the tilted layer
    // over the highest one, or else from the overhangs leaning towards the direction.
    const Vec3d layer_normal = Vec3d(t * dir.x(), t * dir.y(), 1.).normalized();
    double      start        = std::numeric_limits<double>::max();
    double      end          = std::numeric_limits<double>::lowest();
    for (const bool fixed_only : { true, false }) {
        for (const auto &[i, n] : overhangs) {
            if (fixed_only ? - n.dot(layer_normal) > sin_critical : n.x() * dir.x() + n.y() * dir.y() <= 0.)
                continue;
            for (int j = 0; j < 3; ++ j) {
                const stl_vertex &v      = m_mesh.vertices[m_mesh.indices[i][j]];
                const double      height = double(v.z()) + t * (double(v.x()) * dir.x() + double(v.y()) * dir.y() - w_c);
                start = std::min(start, height);
                end   = std::max(end, height);
            }
        }
        if (start <= end)
            break;
    }
    if (start > end)
        return false;
    m_settings.start_height = std::max(0., start);
    // 0 is kept for an end at the top of the object.
    m_settings.end_height   = std::max(end, 0.01);
    return true;
}

void GLGizmoTiltedLayers::update_object()
{
    const Selection &selection = m_parent.get_selection();
    ModelObject     *mo        = selection.is_single_full_instance() ? get_selected_model_object(m_parent) : nullptr;
    const int instance_idx     = selection.get_instance_idx();
    if (mo == nullptr || instance_idx < 0 || instance_idx >= int(mo->instances.size())) {
        m_object = nullptr;
        return;
    }
    const Transform3d matrix     = mo->instances[instance_idx]->get_transformation().get_matrix();
    const bool        new_object = mo != m_object || mo->id() != m_object_id || instance_idx != m_instance_idx;
    const bool        new_mesh   = new_object || ! matrix.isApprox(m_instance_matrix) || mo->volumes.size() != m_volumes_count;
    // Undo / redo keeps the same object but changes its configuration, as the settings of the object list do.
    const bool        new_config = new_object || mo->config.ModelConfig::timestamp() != m_config_timestamp;
    if (! new_mesh && ! new_config)
        return;

    m_object          = mo;
    m_object_id       = mo->id();
    m_instance_idx    = instance_idx;
    m_instance_matrix = matrix;
    m_volumes_count   = mo->volumes.size();
    if (new_mesh) {
        m_mesh     = tilted_layers_object_mesh(*mo, matrix);
        m_mesh_box = BoundingBoxf3();
        for (const stl_vertex &v : m_mesh.vertices)
            m_mesh_box.merge(Vec3d(v.cast<double>()));
    }
    if (new_object) {
        m_no_overhangs = false;
        this->read_printer_settings();
        m_settings = tilted_layers_settings(mo->config.get());
        if (! m_settings.enabled)
            this->default_settings();
    } else if (new_config) {
        // The applied settings follow the object; while they are only previewed, the tool keeps its own.
        const TiltedLayersSettings stored = tilted_layers_settings(mo->config.get());
        if (stored.enabled)
            m_settings = stored;
        else
            m_settings.enabled = false;
    }
    m_config_timestamp = mo->config.ModelConfig::timestamp();
    this->update_params();
}

void GLGizmoTiltedLayers::update_params()
{
    // Computed as if applied, to preview the tilted layers before applying them.
    TiltedLayersSettings settings = m_settings;
    settings.enabled = true;
    settings.angle   = this->preview_angle();
    m_params = tilted_layers_params(settings, m_mesh, m_instance_matrix, m_first_layer_height, m_layer_height, m_max_layer_height, nullptr);
    m_overlays_dirty = true;
    m_start_plane.reset();
    m_end_plane.reset();
    m_hover_model.reset();
    m_hover_model_zw = -1.;
}

void GLGizmoTiltedLayers::update_overlays()
{
    m_overlays_dirty = false;
    m_transition_model.reset();
    m_fixed_model.reset();
    m_unsupported_model.reset();
    m_fixed_area       = 0.;
    m_unsupported_area = 0.;
    m_vertex_zw.clear();
    if (m_mesh.indices.empty() || ! m_params.enabled)
        return;

    m_vertex_zw.reserve(m_mesh.vertices.size());
    for (const stl_vertex &v : m_mesh.vertices)
        m_vertex_zw.emplace_back(float(m_params.warped_z(Vec2d(v.x(), v.y()), double(v.z()))));

    // Overhang angle from the vertical above which supports are generated.
    const double sin_critical = std::sin((90. - std::clamp(m_support_angle, 1., 89.)) * PI / 180.);
    indexed_triangle_set transition, fixed, unsupported;
    for (const stl_triangle_vertex_indices &f : m_mesh.indices) {
        const Vec3d p[3] = { m_mesh.vertices[f[0]].cast<double>(), m_mesh.vertices[f[1]].cast<double>(), m_mesh.vertices[f[2]].cast<double>() };
        Vec3d        n    = (p[1] - p[0]).cross(p[2] - p[0]);
        const double norm = n.norm();
        if (norm < EPSILON)
            continue;
        n /= norm;
        const double s[3] = { double(m_vertex_zw[f[0]]), double(m_vertex_zw[f[1]]), double(m_vertex_zw[f[2]]) };
        // The transitions, drawn along the layers.
        add_clipped_triangle(transition, p, s, m_params.z0, m_params.z1, n * OVERLAY_OFFSET);
        add_clipped_triangle(transition, p, s, m_params.z2, m_params.z3, n * OVERLAY_OFFSET);
        // The overhangs, but the bottom on the bed.
        if (std::max(p[0].z(), std::max(p[1].z(), p[2].z())) <= m_first_layer_height + EPSILON || - n.z() <= sin_critical)
            continue;
        const double slope        = m_params.slope((s[0] + s[1] + s[2]) / 3.);
        const Vec3d  layer_normal = Vec3d(slope * m_params.dir.x(), slope * m_params.dir.y(), 1.).normalized();
        const double area         = 0.5 * norm;
        if (- n.dot(layer_normal) <= sin_critical) {
            add_triangle(fixed, p, n * (2. * OVERLAY_OFFSET));
            m_fixed_area += area;
        } else {
            add_triangle(unsupported, p, n * (2. * OVERLAY_OFFSET));
            m_unsupported_area += area;
        }
    }
    if (! transition.indices.empty())
        m_transition_model.init_from(transition);
    if (! fixed.indices.empty())
        m_fixed_model.init_from(fixed);
    if (! unsupported.indices.empty())
        m_unsupported_model.init_from(unsupported);
}

void GLGizmoTiltedLayers::update_hover_model()
{
    if (m_hover_zw < 0. || m_vertex_zw.size() != m_mesh.vertices.size()) {
        m_hover_model.reset();
        m_hover_model_zw = -1.;
        return;
    }
    if (m_hover_model.is_initialized() && std::abs(m_hover_zw - m_hover_model_zw) < 0.05)
        return;
    m_hover_model.reset();
    m_hover_model_zw = m_hover_zw;
    // The layer under the mouse, which shows how the layers lean.
    const double         half = 0.5 * std::max(m_layer_height, 0.05);
    indexed_triangle_set its;
    for (const stl_triangle_vertex_indices &f : m_mesh.indices) {
        const double s[3] = { double(m_vertex_zw[f[0]]), double(m_vertex_zw[f[1]]), double(m_vertex_zw[f[2]]) };
        if (std::max(s[0], std::max(s[1], s[2])) < m_hover_zw - half || std::min(s[0], std::min(s[1], s[2])) > m_hover_zw + half)
            continue;
        const Vec3d p[3] = { m_mesh.vertices[f[0]].cast<double>(), m_mesh.vertices[f[1]].cast<double>(), m_mesh.vertices[f[2]].cast<double>() };
        Vec3d        n    = (p[1] - p[0]).cross(p[2] - p[0]);
        const double norm = n.norm();
        if (norm < EPSILON)
            continue;
        add_clipped_triangle(its, p, s, m_hover_zw - half, m_hover_zw + half, n * (3. * OVERLAY_OFFSET / norm));
    }
    if (! its.indices.empty())
        m_hover_model.init_from(its);
}

Vec2d GLGizmoTiltedLayers::bed_direction() const
{
    return tilted_layers_direction(m_settings.direction, m_instance_matrix);
}

void GLGizmoTiltedLayers::set_bed_direction(const Vec2d &dir)
{
    // The angle set is kept: the preview reduces it to the clearance of the print head on the new side.
    m_settings.direction = tilted_layers_direction_deg(dir, m_instance_matrix);
}

double GLGizmoTiltedLayers::max_angle() const
{
    // The side of the print head facing uphill passes over the part printed so far.
    const double clearance = m_clearance.max_tilt(- this->bed_direction());
    return clearance <= 0. ? 0. : std::clamp(clearance - TILTED_LAYERS_CLEARANCE_MARGIN, 0., TILTED_LAYERS_MAX_ANGLE);
}

double GLGizmoTiltedLayers::preview_angle() const
{
    const double max_angle = this->max_angle();
    return max_angle > 0. ? std::min(m_settings.angle, max_angle) : m_settings.angle;
}

Vec2d GLGizmoTiltedLayers::best_direction(const Vec2d &current) const
{
    double best_tilt = 0.;
    double best_dot  = -2.;
    Vec2d  best      = Vec2d::Zero();
    for (int deg = -180; deg < 180; ++ deg) {
        const double a = double(deg) * PI / 180.;
        const Vec2d  dir(std::cos(a), std::sin(a));
        const double tilt = m_clearance.max_tilt(- dir);
        if (tilt <= 0.)
            continue;
        const double dot = dir.dot(current);
        if (tilt > best_tilt + 1e-6 || (tilt > best_tilt - 1e-6 && dot > best_dot)) {
            best_tilt = tilt;
            best_dot  = dot;
            best      = dir;
        }
    }
    return best;
}

double GLGizmoTiltedLayers::zone_start() const
{
    return m_params.enabled ? m_params.start_height : std::max(0., m_settings.start_height);
}

double GLGizmoTiltedLayers::zone_end() const
{
    if (m_params.enabled)
        return m_params.end_height;
    const double top = m_mesh_box.defined ? m_mesh_box.max.z() : 0.;
    return m_settings.end_height > 0. ? std::min(m_settings.end_height, top) : top;
}

Vec3d GLGizmoTiltedLayers::plane_point(double height) const
{
    const Vec2d  dir    = this->bed_direction();
    const Vec2d  center = m_mesh_box.center().head<2>();
    const double w_c    = m_params.enabled ? m_params.w_center : center.dot(dir);
    const Vec2d  pt     = center + dir * (w_c - center.dot(dir));
    return Vec3d(pt.x(), pt.y(), height);
}

void GLGizmoTiltedLayers::update_grabbers() const
{
    if (m_grabbers.size() != 2 || ! m_mesh_box.defined)
        return;
    const Vec2d  dir  = this->bed_direction();
    const Vec2d  perp(- dir.y(), dir.x());
    const double half = 0.6 * std::max(m_mesh_box.size().x(), m_mesh_box.size().y()) + 5.;
    const Vec3d  side(perp.x() * half, perp.y() * half, 0.);
    // On a side edge of each plane, in the middle of the object where the height of the plane is measured.
    const Vec3d positions[2] = { this->plane_point(this->zone_start()) + side, this->plane_point(this->zone_end()) + side };
    for (int i = 0; i < 2; ++ i) {
        Grabber &grabber    = m_grabbers[i];
        grabber.center      = positions[i];
        grabber.m_matrix    = Geometry::translation_transform(positions[i]) * Geometry::scale_transform(double(get_grabber_size()));
        grabber.color       = GRABBER_NORMAL_COL;
        grabber.hover_color = GRABBER_HOVER_COL;
        grabber.enabled     = true;
    }
}

BoundingBoxf3 GLGizmoTiltedLayers::get_bounding_box() const
{
    BoundingBoxf3 box = m_mesh_box;
    if (box.defined) {
        // The planes are larger than the object.
        const Vec3d  center = box.center();
        const double half   = 0.6 * std::max(box.size().x(), box.size().y()) + 5. + get_grabber_size();
        box.merge(Vec3d(center + Vec3d(half, half, 0.)));
        box.merge(Vec3d(center - Vec3d(half, half, 0.)));
    }
    return box;
}

void GLGizmoTiltedLayers::render_overlays()
{
    const Camera &camera = wxGetApp().plater()->get_camera();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    if (m_transition_model.is_initialized())
        render_glmodel(m_transition_model, TRANSITION_COLOR, camera.get_view_matrix(), camera.get_projection_matrix());
    if (m_fixed_model.is_initialized())
        render_glmodel(m_fixed_model, FIXED_COLOR, camera.get_view_matrix(), camera.get_projection_matrix());
    if (m_unsupported_model.is_initialized())
        render_glmodel(m_unsupported_model, UNSUPPORTED_COLOR, camera.get_view_matrix(), camera.get_projection_matrix());
    if (m_hover_model.is_initialized())
        render_glmodel(m_hover_model, HOVER_COLOR, camera.get_view_matrix(), camera.get_projection_matrix());
    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoTiltedLayers::render_planes()
{
    if (! m_mesh_box.defined)
        return;
    const Vec2d  dir  = this->bed_direction();
    const Vec2d  perp(- dir.y(), dir.x());
    const double half = 0.6 * std::max(m_mesh_box.size().x(), m_mesh_box.size().y()) + 5.;
    const Vec3d  side(perp.x() * half, perp.y() * half, 0.);
    // The first and the last tilted layers of the zone.
    const double angle = m_params.enabled ? m_params.angle : this->preview_angle() * PI / 180.;
    const Vec3d  down(dir.x() * std::cos(angle) * half, dir.y() * std::cos(angle) * half, - std::sin(angle) * half);
    if (! m_start_plane.is_initialized())
        m_start_plane.init_from(make_quad(this->plane_point(this->zone_start()), down, side));
    if (! m_end_plane.is_initialized())
        m_end_plane.init_from(make_quad(this->plane_point(this->zone_end()), down, side));

    const Camera &camera = wxGetApp().plater()->get_camera();
    const std::array<float, 4> &color = this->max_angle() > 0. ? PLANE_COLOR : PLANE_WARNING_COLOR;
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    render_glmodel(m_start_plane, color, camera.get_view_matrix(), camera.get_projection_matrix());
    render_glmodel(m_end_plane, color, camera.get_view_matrix(), camera.get_projection_matrix());
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
}

void GLGizmoTiltedLayers::on_render()
{
    this->update_object();
    if (m_object == nullptr)
        return;
    if (m_overlays_dirty)
        this->update_overlays();
    this->update_hover_model();
    this->render_overlays();
    this->render_planes();
    this->update_grabbers();
    // The grabbers over everything.
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    render_grabbers();
}

void GLGizmoTiltedLayers::on_render_for_picking()
{
    if (m_object == nullptr)
        return;
    glsafe(::glDisable(GL_DEPTH_TEST));
    this->update_grabbers();
    render_grabbers_for_picking(this->get_bounding_box());
}

void GLGizmoTiltedLayers::on_start_dragging()
{
    if (m_hover_id != StartGrabber && m_hover_id != EndGrabber)
        return;
    this->update_grabbers();
    m_drag_start_point  = m_grabbers[m_hover_id].center;
    m_drag_start_height = m_drag_start_point.z();
}

void GLGizmoTiltedLayers::on_update(const UpdateData &data)
{
    if ((m_hover_id != StartGrabber && m_hover_id != EndGrabber) || ! m_mesh_box.defined)
        return;
    // Point of the vertical line through the grabber closest to the mouse ray.
    const Vec3d  d   = data.mouse_ray.unit_vector();
    const Vec3d  w0  = data.mouse_ray.a - m_drag_start_point;
    const double den = 1. - d.z() * d.z();
    if (den < 1e-6)
        // Looking along the line.
        return;
    const double height = m_drag_start_height + (w0.z() - w0.dot(d) * d.z()) / den;
    if (m_hover_id == StartGrabber) {
        // The plane stops where the zone cannot start lower (see TiltedLayersParams::start_raised).
        m_settings.start_height = std::max(0., height);
        // The end is pushed, unless the zone goes up to the top of the object.
        if (m_settings.end_height > 0. && m_settings.end_height < m_settings.start_height)
            m_settings.end_height = m_settings.start_height;
    } else {
        // Above the object: up to its top. Below the start of the zone, the plane stops there.
        const double top = m_params.enabled ? m_params.top_height : m_mesh_box.max.z();
        m_settings.end_height = height >= top ? 0. : std::max(height, 0.01);
    }
    this->update_params();
}

void GLGizmoTiltedLayers::on_stop_dragging()
{
    if (m_hover_id != StartGrabber && m_hover_id != EndGrabber)
        return;
    const double height = m_hover_id == StartGrabber ? this->zone_start() : this->zone_end();
    if (std::abs(height - m_drag_start_height) < EPSILON)
        // Only clicked, or held where the plane stops.
        return;
    // Moving a plane applies the zone to the object. The snapshot keeps the configuration before the move.
    wxGetApp().plater()->take_snapshot(_u8L("Tilted layers: move a plane"));
    m_settings.enabled = true;
    this->commit();
}

bool GLGizmoTiltedLayers::pick_layer(const Vec2d &mouse_position, double &zw) const
{
    if (m_object == nullptr || ! m_params.enabled || m_c == nullptr || m_c->raycaster() == nullptr)
        return false;
    const Camera                            &camera     = wxGetApp().plater()->get_camera();
    const std::vector<const MeshRaycaster*>  raycasters = m_c->raycaster()->raycasters();
    const Transform3d                        instance   = m_object->instances[m_instance_idx]->get_transformation().get_matrix();
    double best     = std::numeric_limits<double>::max();
    bool   found    = false;
    Vec3d  best_hit = Vec3d::Zero();
    size_t mesh_id  = 0;
    for (const ModelVolume *volume : m_object->volumes) {
        if (! volume->is_model_part())
            continue;
        if (mesh_id >= raycasters.size())
            break;
        const Transform3d trafo = instance * volume->get_matrix();
        Vec3f hit, normal;
        if (raycasters[mesh_id]->unproject_on_mesh(mouse_position, trafo, camera, hit, normal)) {
            const Vec3d  pt   = trafo * hit.cast<double>();
            const double dist = (camera.get_position() - pt).squaredNorm();
            if (dist < best) {
                best     = dist;
                best_hit = pt;
                found    = true;
            }
        }
        ++ mesh_id;
    }
    if (found)
        zw = m_params.warped_z(Vec2d(best_hit.x(), best_hit.y()), best_hit.z());
    return found;
}

bool GLGizmoTiltedLayers::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (m_object == nullptr || action != SLAGizmoEventType::Moving)
        return false;
    // The layer under the mouse is highlighted.
    double zw = -1.;
    if (m_hover_id != -1 || ! this->pick_layer(mouse_position, zw))
        zw = -1.;
    if (std::abs(zw - m_hover_zw) > 1e-6) {
        m_hover_zw = zw;
        m_parent.set_as_dirty();
    }
    return false;
}

void GLGizmoTiltedLayers::begin_edit(const std::string &snapshot)
{
    if (! m_edit_snapshot)
        wxGetApp().plater()->take_snapshot(snapshot);
    m_edit_snapshot = true;
}

void GLGizmoTiltedLayers::commit()
{
    if (m_object == nullptr)
        return;
    ModelConfigObject &config = m_object->config;
    const bool         had    = config.has("tilted_layers");
    // Not applied and nothing to remove.
    if (! m_settings.enabled && ! had)
        return;
    if (m_settings.enabled) {
        // As shown: the angle reduced to the clearance of the print head when it is known, the start of the zone
        // raised to make room for the lower transition.
        const double angle = this->max_angle() > 0. ? this->preview_angle() : m_settings.angle;
        double       start = m_settings.start_height;
        double       end   = m_settings.end_height;
        if (m_params.enabled) {
            start = m_params.start_height;
            end   = m_params.up_to_top() ? 0. : std::max(m_params.end_height, 0.01);
        }
        config.set("tilted_layers", true);
        config.set("tilted_layers_angle", angle);
        config.set("tilted_layers_direction", m_settings.direction);
        config.set("tilted_layers_start_height", start);
        config.set("tilted_layers_end_height", end);
    } else {
        for (const char *key : { "tilted_layers", "tilted_layers_angle", "tilted_layers_direction", "tilted_layers_start_height",
                                 "tilted_layers_end_height" })
            config.erase(key);
    }
    // Not a change made outside of the tool (see update_object()).
    m_config_timestamp = config.ModelConfig::timestamp();
    if (had != config.has("tilted_layers"))
        // The object gets or loses its settings item in the object list.
        wxGetApp().obj_list()->object_config_options_changed({ m_object, nullptr });
    wxGetApp().plater()->set_plater_dirty(true);
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLGizmoTiltedLayers::turn_to_best_side()
{
    if (m_object == nullptr || m_state != On)
        return;
    const Vec2d current = this->bed_direction();
    const Vec2d best    = this->best_direction(current);
    if (best.squaredNorm() < 0.5)
        return;
    double rotation = std::atan2(best.y(), best.x()) - std::atan2(current.y(), current.x());
    while (rotation > PI)
        rotation -= 2. * PI;
    while (rotation < - PI)
        rotation += 2. * PI;
    if (std::abs(rotation) < 1e-4)
        return;
    // The direction of the overhangs is attached to the object: it turns with it.
    Selection &selection = m_parent.get_selection();
    selection.setup_cache();
    selection.rotate(Vec3d(0., 0., rotation), TransformationType(TransformationType::World_Relative_Joint));
    m_parent.do_rotate(_u8L("Tilted layers: turn the object"));
    // The angle allowed by the print head changes with the side: stored as it will be printed.
    this->update_object();
    if (m_settings.enabled)
        this->commit();
    m_parent.set_as_dirty();
}

void GLGizmoTiltedLayers::on_render_input_window(float x, float y, float bottom_limit)
{
    this->update_object();

    const float win_h = ImGui::GetWindowHeight();
    y = std::min(y, bottom_limit - win_h);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    if (m_last_h != win_h || m_last_y != y) {
        // Ask the canvas for another frame to render the window in the correct position.
        m_imgui->set_requires_extra_frame();
        m_last_h = win_h;
        m_last_y = y;
    }
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float scale      = m_imgui->get_style_scaling();
    const float wrap_width = 360.f * scale;
    if (m_object == nullptr) {
        m_imgui->text(_L("Please select single object."));
    } else {
        bool changed = false;

        m_imgui->text_wrapped(_L("Place the zone to print with tilted layers between the two planes. The layers progressively tilt "
                                 "below it and become flat again above it."), wrap_width);
        // Applied as soon as edited: the tool only previews the zone of an object which has none yet.
        if (m_settings.enabled) {
            m_imgui->text_colored(ImGuiWrapper::COL_BAMBU, _L("Applied: this object is sliced with tilted layers."));
            if (m_imgui->button(_L("Remove"))) {
                wxGetApp().plater()->take_snapshot(_u8L("Tilted layers: remove"));
                m_settings.enabled = false;
                this->commit();
            }
        } else {
            m_imgui->text_wrapped(_L("Preview: move a plane or change a setting to apply the tilted layers to this object."), wrap_width);
            m_imgui->push_confirm_button_style();
            if (m_imgui->bbl_button(_L("Apply"))) {
                wxGetApp().plater()->take_snapshot(_u8L("Tilted layers: apply"));
                m_settings.enabled = true;
                this->commit();
            }
            m_imgui->pop_confirm_button_style();
        }

        // Layout of the sliders.
        const std::string labels[] = { _u8L("Direction"), _u8L("Angle"), _u8L("Zone start"), _u8L("Zone end") };
        float caption_width = 0.f;
        for (const std::string &label : labels)
            caption_width = std::max(caption_width, ImGuiWrapper::calc_text_size(wxString::FromUTF8(label.c_str())).x);
        const float       space  = 8.f * scale;
        SliderInputLayout layout = { caption_width + space, 150.f * scale, caption_width + 2.f * space + 150.f * scale, 70.f * scale };

        ImGui::Separator();
        const Vec2d bed_dir = this->bed_direction();
        m_imgui->text(overhangs_side(bed_dir));
        float azimuth = float(std::atan2(bed_dir.y(), bed_dir.x()) * 180. / PI);
        if (render_slider_double_input_by_format(layout, labels[0], azimuth, -180.f, 180.f, 0)) {
            this->begin_edit(_u8L("Tilted layers: direction"));
            const double a = double(azimuth) * PI / 180.;
            this->set_bed_direction(Vec2d(std::cos(a), std::sin(a)));
            changed = true;
        }
        const double   max_angle = this->max_angle();
        // The side of the print head facing uphill passes over the part printed so far.
        const wxString head_side = wxString::FromUTF8(head_side_name(- bed_dir));
        if (max_angle <= 0.) {
            m_imgui->warning_text_wrapped(wxString::Format(_L("The %s of the print head, which the layers rise towards, does not "
                                                              "clear (clearance unknown or too small): these layers will not be "
                                                              "tilted. Turn the object towards the best side, or set the clearance "
                                                              "in the printer settings (Extruder clearance)."), head_side), wrap_width);
        } else {
            float angle = float(this->preview_angle());
            if (render_slider_double_input_by_format(layout, labels[1], angle, 0.f, float(max_angle), 1)) {
                this->begin_edit(_u8L("Tilted layers: angle"));
                m_settings.angle = std::clamp(double(angle), 0., max_angle);
                changed = true;
            }
            m_imgui->text(wxString::Format(_L("The %s of the print head clears %.1f°."), head_side, max_angle + TILTED_LAYERS_CLEARANCE_MARGIN));
        }
        const Vec2d best = this->best_direction(bed_dir);
        if (best.squaredNorm() > 0.5) {
            // Compared by clearance: a direction a fraction of a degree off an axis may involve an unknown side.
            if (m_clearance.max_tilt(- bed_dir) >= m_clearance.max_tilt(- best) - 1e-6)
                m_imgui->text_wrapped(_L("The object is turned towards the side where the print head clears the most."), wrap_width);
            else if (m_imgui->button(_L("Turn the object towards the best side")))
                // Not while rendering.
                wxGetApp().CallAfter([this]() { this->turn_to_best_side(); });
        }

        ImGui::Separator();
        const double lowest = m_params.enabled ? std::max(0., m_params.bottom_height) : 0.;
        const double top    = std::max(lowest, m_params.enabled ? m_params.top_height : (m_mesh_box.defined ? m_mesh_box.max.z() : 0.));
        float        start  = float(this->zone_start());
        if (render_slider_double_input_by_format(layout, labels[2], start, float(lowest), float(top), 1)) {
            this->begin_edit(_u8L("Tilted layers: zone"));
            m_settings.start_height = std::max(0., double(start));
            if (m_settings.end_height > 0. && m_settings.end_height < m_settings.start_height)
                m_settings.end_height = m_settings.start_height;
            changed = true;
        }
        float end = float(this->zone_end());
        if (render_slider_double_input_by_format(layout, labels[3], end, std::min(start, float(top)), float(top), 1)) {
            this->begin_edit(_u8L("Tilted layers: zone"));
            // 0: up to the top of the object.
            m_settings.end_height = double(end) >= top - 1e-3 ? 0. : std::max(double(end), 0.01);
            changed = true;
        }
        if (m_imgui->button(_L("Fit to the overhangs"))) {
            this->begin_edit(_u8L("Tilted layers: fit to the overhangs"));
            m_no_overhangs = ! this->fit_overhangs();
            changed        = ! m_no_overhangs;
        }
        if (m_no_overhangs)
            m_imgui->text_wrapped(_L("This object has no overhang needing support."), wrap_width);
        if (m_params.enabled) {
            if (m_params.up_to_top())
                m_imgui->text_wrapped(wxString::Format(_L("Automatic transition of %.1f mm below the zone, which goes up to the top of "
                                                          "the object."), m_params.low_transition), wrap_width);
            else
                m_imgui->text_wrapped(wxString::Format(_L("Automatic transitions: %.1f mm below the zone, %.1f mm above it."),
                                                       m_params.low_transition, m_params.high_transition), wrap_width);
            if (m_params.start_raised)
                m_imgui->warning_text_wrapped(_L("The zone cannot start lower: the transition below it needs room above the first "
                                                 "layer."), wrap_width);
        }

        ImGui::Separator();
        m_imgui->text(wxString::Format(_L("Overhangs printed without support thanks to the tilt: %.1f cm²"), m_fixed_area / 100.));
        m_imgui->text(wxString::Format(_L("Overhangs still needing support: %.1f cm²"), m_unsupported_area / 100.));

        if (changed) {
            // Editing applies the zone to the object.
            m_settings.enabled = true;
            this->update_params();
            this->commit();
        }
    }

    // One undo snapshot per edit: the next edit starts when the mouse releases the slider.
    if (! ImGui::IsAnyItemActive())
        m_edit_snapshot = false;

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace GUI
} // namespace Slic3r
