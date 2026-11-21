#include "GCodeSketchView.hpp"
#include "GCodeSketchPanel.hpp"

#include "CameraUtils.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "ImGuiWrapper.hpp"
#include "OpenGLManager.hpp"
#include "3DScene.hpp"
#include "I18N.hpp"

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Tesselate.hpp"

#include <GL/glew.h>
#include <imgui/imgui.h>
#include <wx/dcclient.h>
#include <wx/glcanvas.h>
#include <wx/menu.h>
#include <wx/utils.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

namespace Slic3r {
namespace GUI {

namespace {

// The bed is drawn a bit below Z = 0, not to hide what is drawn on it.
constexpr float  BED_Z           = -0.05f;
constexpr double SHADOW_Z        = 0.01;
constexpr float  EXCLUDE_Z       = 0.02f;
// mm between the lines of the bed and of the working plane.
constexpr double GRID_STEP       = 10.;
// px
constexpr double PICK_RADIUS     = 8.;
constexpr double AXIS_RADIUS     = 8.;
constexpr double ALIGN_RADIUS    = 6.;
constexpr double DRAG_THRESHOLD  = 4.;
constexpr double POINT_RADIUS    = 4.;
constexpr double CURSOR_RADIUS   = 5.;
// Below this length on the screen, the direction of a line isn't constrained.
constexpr double MIN_DIRECTION   = 20.;
// Rotation speed of the 3D view of the plater.
constexpr double TRACKBALL       = 0.8;
// Chordal error of the arcs on screen, and of the arcs searched under the mouse.
constexpr double ARC_DEVIATION   = 0.02;
constexpr double PICK_DEVIATION  = 0.1;
// Beyond these, the points aren't drawn as spheres and the extrusions only as lines (large imported G-code).
constexpr size_t MAX_SPHERES     = 3000;
constexpr size_t MAX_BEAD_PIECES = 60000;
// Beyond this number of segments, no middle or center to snap to and no highlight under the mouse.
constexpr size_t MAX_DETAILED    = 20000;
// Points of the drawing (the last ones) the cursor lines up with.
constexpr size_t MAX_ALIGNED     = 2000;
// Characters of a typed value.
constexpr size_t MAX_TYPED       = 10;

double content_scale(const wxWindow *window)
{
#if defined(__APPLE__) || defined(__WXGTK3__)
    return window->GetContentScaleFactor();
#else
    (void) window;
    return 1.;
#endif
}

struct Palette
{
    ColorRGBA background;
    ColorRGBA bed;
    ColorRGBA bed_grid;
    ColorRGBA bed_outline;
    ColorRGBA volume;
    ColorRGBA exclude;
    ColorRGBA plane;
    ColorRGBA plane_grid;
    ColorRGBA print;
    ColorRGBA travel;
    ColorRGBA layers;
    ColorRGBA ghost;
    ColorRGBA shadow;
    ColorRGBA point;
    ColorRGBA mid_point;
    ColorRGBA selected;
    ColorRGBA hovered;
    ColorRGBA issue;
    ColorRGBA cursor;
    ColorRGBA cursor_outside;
    ColorRGBA axes[3];
};

Palette palette(SketchPlane plane)
{
    Palette p;
    if (wxGetApp().dark_mode()) {
        p.background  = ColorRGBA(0.15f, 0.15f, 0.17f, 1.f);
        p.bed         = ColorRGBA(0.27f, 0.27f, 0.30f, 1.f);
        p.bed_grid    = ColorRGBA(0.36f, 0.36f, 0.40f, 1.f);
        p.bed_outline = ColorRGBA(0.58f, 0.58f, 0.62f, 1.f);
        p.volume      = ColorRGBA(0.42f, 0.42f, 0.46f, 1.f);
        p.shadow      = ColorRGBA(0.07f, 0.07f, 0.08f, 0.85f);
        p.point       = ColorRGBA(0.88f, 0.88f, 0.90f, 1.f);
    } else {
        p.background  = ColorRGBA(0.93f, 0.93f, 0.94f, 1.f);
        p.bed         = ColorRGBA(0.80f, 0.80f, 0.82f, 1.f);
        p.bed_grid    = ColorRGBA(0.68f, 0.68f, 0.71f, 1.f);
        p.bed_outline = ColorRGBA(0.40f, 0.40f, 0.43f, 1.f);
        p.volume      = ColorRGBA(0.62f, 0.62f, 0.66f, 1.f);
        p.shadow      = ColorRGBA(0.42f, 0.42f, 0.45f, 0.85f);
        p.point       = ColorRGBA(0.18f, 0.18f, 0.20f, 1.f);
    }
    const std::array<float, 3> rgb = sketch_plane_rgb(plane);
    p.exclude        = ColorRGBA(0.90f, 0.30f, 0.30f, 0.45f);
    p.plane          = ColorRGBA(rgb[0], rgb[1], rgb[2], 0.16f);
    p.plane_grid     = ColorRGBA(rgb[0], rgb[1], rgb[2], 0.45f);
    p.print          = ColorRGBA(0.95f, 0.55f, 0.15f, 1.f);
    p.travel         = ColorRGBA(0.25f, 0.55f, 0.95f, 1.f);
    p.layers         = ColorRGBA(0.95f, 0.55f, 0.15f, 0.35f);
    p.ghost          = ColorRGBA(0.25f, 0.55f, 0.95f, 0.55f);
    p.mid_point      = ColorRGBA(0.10f, 0.65f, 0.60f, 1.f);
    p.selected       = ColorRGBA(0.00f, 0.68f, 0.26f, 1.f);
    p.hovered        = ColorRGBA(1.00f, 0.80f, 0.15f, 1.f);
    p.issue          = ColorRGBA(0.90f, 0.20f, 0.20f, 1.f);
    p.cursor         = ColorRGBA(rgb[0] * 0.75f, rgb[1] * 0.75f, rgb[2] * 0.75f, 1.f);
    p.cursor_outside = ColorRGBA(0.90f, 0.20f, 0.20f, 1.f);
    p.axes[0]        = ColorRGBA(0.90f, 0.25f, 0.25f, 0.9f);
    p.axes[1]        = ColorRGBA(0.25f, 0.75f, 0.30f, 0.9f);
    p.axes[2]        = ColorRGBA(0.25f, 0.45f, 0.95f, 0.9f);
    return p;
}

// Colours of the X, Y and Z axes over the view.
ImU32 axis_colour(int axis)
{
    switch (axis) {
    case 0: return IM_COL32(230, 64, 64, 255);
    case 1: return IM_COL32(50, 170, 70, 255);
    default: return IM_COL32(64, 115, 242, 255);
    }
}

ImVec2 to_imgui(const Vec2d &p) { return ImVec2(float(p.x()), float(p.y())); }

void dashed_line(ImDrawList &draw, const Vec2d &a, const Vec2d &b, ImU32 colour, float thickness)
{
    const Vec2d  d      = b - a;
    const double length = d.norm();
    if (length < 1.)
        return;
    const Vec2d step = d / length;
    for (double t = 0.; t < length; t += 9.)
        draw.AddLine(to_imgui(a + t * step), to_imgui(a + std::min(t + 5., length) * step), colour, thickness);
}

// Distance on the screen from p to the segment from a to b.
double distance_to_segment(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d  ab = b - a;
    const double l2 = ab.squaredNorm();
    const double t  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
    return (a + t * ab - p).norm();
}

GLModel::Geometry make_geometry(GLModel::PrimitiveType type, GLModel::Geometry::EVertexLayout layout)
{
    GLModel::Geometry geometry;
    geometry.format = { type, layout };
    return geometry;
}

GLModel::Geometry make_lines() { return make_geometry(GLModel::PrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3); }

void init_model(GLModel &model, GLModel::Geometry &&geometry)
{
    model.reset();
    if (!geometry.is_empty())
        model.init_from(std::move(geometry));
}

void add_line(GLModel::Geometry &geometry, const Vec3d &a, const Vec3d &b)
{
    const unsigned int id = (unsigned int) geometry.vertices_count();
    geometry.add_vertex(Vec3f(a.cast<float>()));
    geometry.add_vertex(Vec3f(b.cast<float>()));
    geometry.add_line(id, id + 1);
}

void add_polyline(GLModel::Geometry &geometry, const std::vector<Vec3d> &points)
{
    for (size_t i = 1; i < points.size(); ++ i)
        add_line(geometry, points[i - 1], points[i]);
}

void add_quad(GLModel::Geometry &geometry, const Vec3d &p0, const Vec3d &p1, const Vec3d &p2, const Vec3d &p3, const Vec3d &normal)
{
    const unsigned int id = (unsigned int) geometry.vertices_count();
    const Vec3f        n(normal.cast<float>());
    geometry.add_vertex(Vec3f(p0.cast<float>()), n);
    geometry.add_vertex(Vec3f(p1.cast<float>()), n);
    geometry.add_vertex(Vec3f(p2.cast<float>()), n);
    geometry.add_vertex(Vec3f(p3.cast<float>()), n);
    geometry.add_triangle(id, id + 1, id + 2);
    geometry.add_triangle(id, id + 2, id + 3);
}

// Triangles of a polygon at the height z.
void add_polygon(GLModel::Geometry &geometry, const Pointfs &polygon, float z)
{
    if (polygon.size() < 3)
        return;
    const std::vector<Vec2f> triangles = triangulate_expolygon_2f(ExPolygon(Polygon::new_scale(polygon)));
    for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
        const unsigned int id = (unsigned int) geometry.vertices_count();
        for (size_t k = 0; k < 3; ++ k)
            geometry.add_vertex(Vec3f(triangles[i + k].x(), triangles[i + k].y(), z));
        geometry.add_triangle(id, id + 1, id + 2);
    }
}

// Box of an extrusion from a to b, its top on the path, as in the preview of the G-code.
void add_bead(GLModel::Geometry &geometry, const Vec3d &a, const Vec3d &b, double width, double height)
{
    const Vec3d  direction = b - a;
    const double length    = direction.norm();
    if (length < 1e-6)
        return;
    const Vec3d d = direction / length;
    Vec3d       side = d.cross(Vec3d::UnitZ());
    if (side.squaredNorm() < 1e-12)
        side = Vec3d::UnitX();
    else
        side.normalize();
    const Vec3d up   = side.cross(d).normalized();
    const Vec3d half = 0.5 * width * side;
    const Vec3d low  = -height * up;
    // Corners of the section.
    const Vec3d c0 = half;
    const Vec3d c1 = -half;
    const Vec3d c2 = low - half;
    const Vec3d c3 = low + half;
    add_quad(geometry, a + c0, a + c1, b + c1, b + c0, up);
    add_quad(geometry, a + c1, a + c2, b + c2, b + c1, -side);
    add_quad(geometry, a + c2, a + c3, b + c3, b + c2, -up);
    add_quad(geometry, a + c3, a + c0, b + c0, b + c3, side);
    add_quad(geometry, a + c0, a + c3, a + c2, a + c1, -d);
    add_quad(geometry, b + c0, b + c1, b + c2, b + c3, d);
}

GLModel::Geometry unit_disc(unsigned int resolution)
{
    GLModel::Geometry geometry = make_geometry(GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3);
    geometry.add_vertex(Vec3f(0.f, 0.f, 0.f));
    for (unsigned int i = 0; i < resolution; ++ i) {
        const double angle = 2. * PI * double(i) / double(resolution);
        geometry.add_vertex(Vec3f(float(std::cos(angle)), float(std::sin(angle)), 0.f));
    }
    for (unsigned int i = 0; i < resolution; ++ i)
        geometry.add_triangle(0, 1 + i, 1 + (i + 1) % resolution);
    return geometry;
}

// Points of a segment from its start, its arc made of straight pieces.
std::vector<Vec3d> segment_polyline(const Vec3d &from, const GCodeSketch::Segment &segment, double deviation = ARC_DEVIATION,
                                    size_t max_points = 1024)
{
    std::vector<Vec3d> polyline { from };
    if (segment.type == GCodeSketch::SegmentType::Arc) {
        const GCodeSketch::Arc arc = GCodeSketch::arc_through(from, segment.mid, segment.end);
        if (arc.valid) {
            const std::vector<Vec3d> points = GCodeSketch::discretize_arc(arc, deviation, max_points);
            polyline.insert(polyline.end(), points.begin(), points.end());
            return polyline;
        }
    }
    polyline.push_back(segment.end);
    return polyline;
}

Transform3d placed(const Vec3d &position, const Vec3d &scale)
{
    Transform3d matrix = Transform3d::Identity();
    matrix.translate(position);
    matrix.scale(scale);
    return matrix;
}

// The two axes of the plane of normal axis.
int first_axis(int axis) { return axis == 0 ? 1 : 0; }
int second_axis(int axis) { return axis == 2 ? 1 : 2; }

} // namespace

bool GCodeSketchView::Projector::operator()(const Vec3d &point, Vec2d &screen) const
{
    const Vec4d clip = matrix * Vec4d(point.x(), point.y(), point.z(), 1.);
    if (clip.w() <= 1e-9)
        return false;
    // As CameraUtils::project(), with the decimals.
    const double nx = clip.x() / clip.w();
    const double ny = clip.y() / clip.w();
    screen = Vec2d(x + 0.5 * (nx + 1.) * width, height - (y + 0.5 * (ny + 1.) * height));
    return true;
}

GCodeSketchView::GCodeSketchView(GCodeSketchPanel &panel, wxWindow *parent)
    : m_panel(panel)
{
    const auto     &ogl  = wxGetApp().get_opengl_manager();
    const EMSAAType msaa = (ogl && !ogl->is_fxaa_enabled()) ? ogl->get_msaa_type() : EMSAAType::Disabled;
    m_canvas = OpenGLManager::create_wxglcanvas(*parent, msaa);
    if (m_canvas == nullptr)
        return;
    m_context = wxGetApp().init_glcontext(*m_canvas);

    m_canvas->Bind(wxEVT_PAINT, &GCodeSketchView::on_paint, this);
    m_canvas->Bind(wxEVT_SIZE, &GCodeSketchView::on_size, this);
    m_canvas->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
    for (const auto &type : { wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_LEFT_DCLICK, wxEVT_RIGHT_DOWN, wxEVT_RIGHT_UP, wxEVT_RIGHT_DCLICK,
                              wxEVT_MIDDLE_DOWN, wxEVT_MIDDLE_UP, wxEVT_MIDDLE_DCLICK, wxEVT_MOTION, wxEVT_ENTER_WINDOW, wxEVT_LEAVE_WINDOW })
        m_canvas->Bind(type, &GCodeSketchView::on_mouse, this);
    m_canvas->Bind(wxEVT_MOUSEWHEEL, &GCodeSketchView::on_wheel, this);
    m_canvas->Bind(wxEVT_KEY_DOWN, &GCodeSketchView::on_key_down, this);
    m_canvas->Bind(wxEVT_KEY_UP, &GCodeSketchView::on_key_up, this);
    m_canvas->Bind(wxEVT_CHAR, &GCodeSketchView::on_char, this);
    m_canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, &GCodeSketchView::on_capture_lost, this);
    m_canvas->Bind(wxEVT_KILL_FOCUS, &GCodeSketchView::on_kill_focus, this);

    m_camera.select_view("iso");
}

GCodeSketchView::~GCodeSketchView()
{
    release_gl();
    // Destroyed with the view, so that no event reaches the view once deleted.
    if (m_canvas != nullptr) {
        if (m_canvas->HasCapture())
            m_canvas->ReleaseMouse();
        m_canvas->Destroy();
        m_canvas = nullptr;
    }
}

wxWindow *GCodeSketchView::window() const
{
    return m_canvas;
}

void GCodeSketchView::on_machine_changed()
{
    m_machine_dirty = true;
    m_drawing_dirty = true;
    m_zoom_pending  = true;
    refresh();
}

void GCodeSketchView::on_drawing_changed()
{
    m_drawing_dirty   = true;
    m_highlight_dirty = true;
    m_targets_dirty   = true;
    // The segment under the mouse may no longer be there.
    m_hover_segment = -1;
    m_pick_segment  = -1;
    refresh();
}

void GCodeSketchView::on_selection_changed()
{
    m_highlight_dirty = true;
    refresh();
}

void GCodeSketchView::refresh()
{
    if (m_canvas != nullptr)
        m_canvas->Refresh(false);
}

void GCodeSketchView::zoom_to_bed()
{
    m_zoom_pending = true;
    refresh();
}

void GCodeSketchView::select_view(const std::string &direction)
{
    m_camera.select_view(direction);
    zoom_to_bed();
}

void GCodeSketchView::toggle_camera_type()
{
    m_camera.select_next_type();
    refresh();
}

void GCodeSketchView::release_gl()
{
    // The buffers of the models are released by the OpenGL context: it must be current.
    if (m_rendered && make_current())
        reset_models();
    m_rendered         = false;
    m_machine_dirty    = true;
    m_drawing_dirty    = true;
    m_highlight_dirty  = true;
    m_plane_model_axis = -1;
}

bool GCodeSketchView::make_current()
{
    return m_canvas != nullptr && m_context != nullptr && m_canvas->SetCurrent(*m_context);
}

void GCodeSketchView::reset_models()
{
    for (GLModel *model : { &m_bed, &m_bed_grid, &m_bed_outline, &m_exclude, &m_volume, &m_plane, &m_plane_grid, &m_beads, &m_print_lines,
                            &m_travel_lines, &m_layer_lines, &m_shadow, &m_selected_lines, &m_hover_lines, &m_sphere, &m_disc, &m_unit_x,
                            &m_unit_y, &m_unit_z, &m_rubber, &m_ghost, &m_picked, &m_removed })
        model->reset();
}

void GCodeSketchView::build_machine_models()
{
    const GCodeSketch::Machine &machine = m_panel.machine();
    const Pointfs              &shape   = machine.bed_shape;

    m_bed_min = Vec2d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    m_bed_max = -m_bed_min;
    for (const Vec2d &p : shape) {
        m_bed_min = m_bed_min.cwiseMin(p);
        m_bed_max = m_bed_max.cwiseMax(p);
    }
    if (shape.size() < 3 || m_bed_min.x() >= m_bed_max.x() || m_bed_min.y() >= m_bed_max.y()) {
        m_bed_min = Vec2d::Zero();
        m_bed_max = Vec2d(256., 256.);
    }
    const double height = std::max(machine.max_height, 1.);

    GLModel::Geometry bed = make_geometry(GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3);
    add_polygon(bed, shape, BED_Z);
    init_model(m_bed, std::move(bed));

    // Lines every GRID_STEP over the bed.
    GLModel::Geometry grid = make_lines();
    for (double x = std::ceil(m_bed_min.x() / GRID_STEP) * GRID_STEP; x <= m_bed_max.x() + EPSILON; x += GRID_STEP)
        add_line(grid, Vec3d(x, m_bed_min.y(), 0.), Vec3d(x, m_bed_max.y(), 0.));
    for (double y = std::ceil(m_bed_min.y() / GRID_STEP) * GRID_STEP; y <= m_bed_max.y() + EPSILON; y += GRID_STEP)
        add_line(grid, Vec3d(m_bed_min.x(), y, 0.), Vec3d(m_bed_max.x(), y, 0.));
    init_model(m_bed_grid, std::move(grid));

    GLModel::Geometry outline = make_lines();
    for (size_t i = 0; shape.size() >= 3 && i < shape.size(); ++ i) {
        const Vec2d &a = shape[i];
        const Vec2d &b = shape[(i + 1) % shape.size()];
        add_line(outline, Vec3d(a.x(), a.y(), 0.), Vec3d(b.x(), b.y(), 0.));
    }
    init_model(m_bed_outline, std::move(outline));

    GLModel::Geometry exclude = make_geometry(GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3);
    add_polygon(exclude, machine.exclude_area, EXCLUDE_Z);
    init_model(m_exclude, std::move(exclude));

    // Edges of the build volume.
    GLModel::Geometry volume = make_lines();
    const Vec3d corners[4] = { Vec3d(m_bed_min.x(), m_bed_min.y(), 0.), Vec3d(m_bed_max.x(), m_bed_min.y(), 0.),
                               Vec3d(m_bed_max.x(), m_bed_max.y(), 0.), Vec3d(m_bed_min.x(), m_bed_max.y(), 0.) };
    const Vec3d top(0., 0., height);
    for (size_t i = 0; i < 4; ++ i) {
        const Vec3d &a = corners[i];
        const Vec3d &b = corners[(i + 1) % 4];
        add_line(volume, a, a + top);
        add_line(volume, a + top, b + top);
    }
    init_model(m_volume, std::move(volume));

    // Unit shapes.
    init_model(m_sphere, smooth_sphere(16, 1.f));
    init_model(m_disc, unit_disc(24));
    GLModel::Geometry unit_x = make_lines();
    add_line(unit_x, Vec3d::Zero(), Vec3d::UnitX());
    init_model(m_unit_x, std::move(unit_x));
    GLModel::Geometry unit_y = make_lines();
    add_line(unit_y, Vec3d::Zero(), Vec3d::UnitY());
    init_model(m_unit_y, std::move(unit_y));
    GLModel::Geometry unit_z = make_lines();
    add_line(unit_z, Vec3d::Zero(), Vec3d::UnitZ());
    init_model(m_unit_z, std::move(unit_z));

    // The working plane spans the build volume.
    m_plane_model_axis = -1;
}

void GCodeSketchView::build_plane_models(int axis)
{
    // The plane at 0 along its normal, over the build volume, with lines every GRID_STEP.
    const Vec3d min(m_bed_min.x(), m_bed_min.y(), 0.);
    const Vec3d max(m_bed_max.x(), m_bed_max.y(), std::max(m_panel.machine().max_height, 1.));
    const int   a = first_axis(axis);
    const int   b = second_axis(axis);
    auto corner = [a, b](double along_a, double along_b) {
        Vec3d p = Vec3d::Zero();
        p[a]    = along_a;
        p[b]    = along_b;
        return p;
    };

    GLModel::Geometry plane = make_geometry(GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3);
    for (const Vec3d &c : { corner(min[a], min[b]), corner(max[a], min[b]), corner(max[a], max[b]), corner(min[a], max[b]) })
        plane.add_vertex(Vec3f(c.cast<float>()));
    plane.add_triangle(0, 1, 2);
    plane.add_triangle(0, 2, 3);
    init_model(m_plane, std::move(plane));

    GLModel::Geometry grid = make_lines();
    for (double x = std::ceil(min[a] / GRID_STEP) * GRID_STEP; x <= max[a] + EPSILON; x += GRID_STEP)
        add_line(grid, corner(x, min[b]), corner(x, max[b]));
    for (double y = std::ceil(min[b] / GRID_STEP) * GRID_STEP; y <= max[b] + EPSILON; y += GRID_STEP)
        add_line(grid, corner(min[a], y), corner(max[a], y));
    add_line(grid, corner(min[a], min[b]), corner(max[a], min[b]));
    add_line(grid, corner(max[a], min[b]), corner(max[a], max[b]));
    add_line(grid, corner(max[a], max[b]), corner(min[a], max[b]));
    add_line(grid, corner(min[a], max[b]), corner(min[a], min[b]));
    init_model(m_plane_grid, std::move(grid));
    m_plane_model_axis = axis;
}

void GCodeSketchView::build_drawing_models()
{
    const GCodeSketch::Drawing  &drawing  = m_panel.drawing();
    const GCodeSketch::Settings &settings = m_panel.settings();

    std::vector<std::vector<Vec3d>> polylines;
    size_t                          pieces = 0;
    polylines.reserve(drawing.segments.size());
    for (size_t idx = 0; drawing.has_start && idx < drawing.segments.size(); ++ idx) {
        polylines.emplace_back(segment_polyline(drawing.segment_start(idx), drawing.segments[idx]));
        if (drawing.segments[idx].extrude)
            pieces += polylines.back().size() - 1;
    }

    GLModel::Geometry print_lines  = make_lines();
    GLModel::Geometry travel_lines = make_lines();
    GLModel::Geometry shadow       = make_lines();
    GLModel::Geometry beads        = make_geometry(GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3);
    const bool        with_beads   = pieces <= MAX_BEAD_PIECES;
    for (size_t idx = 0; idx < polylines.size(); ++ idx) {
        const GCodeSketch::Segment &segment  = drawing.segments[idx];
        const std::vector<Vec3d>   &polyline = polylines[idx];
        const double                width    = segment.width > 0. ? segment.width : settings.line_width;
        const double                height   = segment.height > 0. ? segment.height : settings.layer_height;
        for (size_t i = 1; i < polyline.size(); ++ i) {
            const Vec3d &a = polyline[i - 1];
            const Vec3d &b = polyline[i];
            add_line(segment.extrude ? print_lines : travel_lines, a, b);
            add_line(shadow, Vec3d(a.x(), a.y(), SHADOW_Z), Vec3d(b.x(), b.y(), SHADOW_Z));
            if (segment.extrude && with_beads)
                add_bead(beads, a, b, width, height);
        }
    }
    init_model(m_print_lines, std::move(print_lines));
    init_model(m_travel_lines, std::move(travel_lines));
    init_model(m_shadow, std::move(shadow));
    init_model(m_beads, std::move(beads));

    // All the layers, light (in spiral mode, the first one rises too).
    const GCodeSketch::Drawing &stacked = m_panel.stacked();
    GLModel::Geometry           layers  = make_lines();
    for (size_t idx = 0; stacked.has_start && idx < stacked.segments.size(); ++ idx)
        if (stacked.segments[idx].extrude)
            add_polyline(layers, segment_polyline(stacked.segment_start(idx), stacked.segments[idx]));
    init_model(m_layer_lines, std::move(layers));
}

void GCodeSketchView::build_highlights()
{
    const GCodeSketch::Drawing &drawing   = m_panel.drawing();
    const SketchSelection      &selection = m_panel.selection();
    auto lines_of = [&drawing](GLModel::Geometry &lines, size_t first, size_t last) {
        for (size_t idx = first; drawing.has_start && idx <= last && idx < drawing.segments.size(); ++ idx)
            add_polyline(lines, segment_polyline(drawing.segment_start(idx), drawing.segments[idx]));
    };
    // The selection, and the objects of a modify tool.
    GLModel::Geometry selected = make_lines();
    if (selection.kind == SketchSelection::Kind::Segments)
        lines_of(selected, selection.first, selection.last);
    for (const size_t idx : m_panel.objects())
        lines_of(selected, idx, idx);
    init_model(m_selected_lines, std::move(selected));
    GLModel::Geometry hovered = make_lines();
    if (m_hover_segment >= 0)
        lines_of(hovered, m_hover_first, m_hover_last);
    init_model(m_hover_lines, std::move(hovered));
    m_highlight_dirty = false;
}

void GCodeSketchView::build_targets()
{
    const GCodeSketch::Drawing &drawing = m_panel.drawing();
    m_targets.clear();
    m_targets_dirty = false;
    if (!drawing.has_start)
        return;
    const size_t count    = drawing.segments.size();
    // Middles and centers, unless the drawing is very large (imported G-code).
    const bool   detailed = count <= MAX_DETAILED;
    m_targets.reserve(1 + (detailed ? 4 : 2) * count);
    m_targets.push_back({ drawing.start, Snap::End });
    for (size_t idx = 0; idx < count; ++ idx) {
        const GCodeSketch::Segment &segment = drawing.segments[idx];
        const Vec3d                &from    = drawing.segment_start(idx);
        if (segment.type == GCodeSketch::SegmentType::Arc) {
            m_targets.push_back({ segment.mid, Snap::Passage });
            if (detailed) {
                const GCodeSketch::Arc arc = GCodeSketch::arc_through(from, segment.mid, segment.end);
                if (arc.valid) {
                    m_targets.push_back({ arc.point_at(0.5), Snap::Middle });
                    m_targets.push_back({ arc.center, Snap::Center });
                }
            }
        } else if (detailed && segment.extrude && (segment.end - from).norm() > EPSILON)
            m_targets.push_back({ 0.5 * (from + segment.end), Snap::Middle });
        m_targets.push_back({ segment.end, Snap::End });
    }
}

void GCodeSketchView::build_previews()
{
    // Segment or shape being drawn, the travel move to its start, its construction lines, the lines picked; or what a
    // modify tool would do: its new shapes, what it removes, where it splits.
    m_rubber.reset();
    m_ghost.reset();
    m_removed.reset();
    m_marks.clear();
    GLModel::Geometry picked = make_lines();
    for (const auto &[a, b] : m_panel.picked_lines())
        add_line(picked, a, b);
    init_model(m_picked, std::move(picked));
    const SketchInput input   = m_panel.input();
    // A segment picked under the mouse: what the tool would do with it.
    const bool        on_pick = m_pick_segment >= 0 && input != SketchInput::Point && input != SketchInput::Line;
    if ((!m_has_cursor && !on_pick) || (m_drag != Drag::None && m_drag != Drag::Arc))
        return;
    const GCodeSketch::Drawing &drawing = m_panel.drawing();
    const std::vector<Vec3d>   &clicks  = m_panel.clicks();
    const SketchTool            tool    = m_panel.tool();
    GLModel::Geometry           rubber  = make_lines();
    GLModel::Geometry           ghost   = make_lines();
    auto add_arc = [&rubber](const Vec3d &start, const Vec3d &mid, const Vec3d &end) {
        const GCodeSketch::Arc arc = GCodeSketch::arc_through(start, mid, end);
        if (!arc.valid) {
            add_line(rubber, start, end);
            return;
        }
        std::vector<Vec3d> points { start };
        const std::vector<Vec3d> on_arc = GCodeSketch::discretize_arc(arc, ARC_DEVIATION, 512);
        points.insert(points.end(), on_arc.begin(), on_arc.end());
        add_polyline(rubber, points);
    };
    // Travel move from the end of the drawing to the start of the next shape.
    auto travel_to = [&drawing, &ghost](const Vec3d &point) {
        if (drawing.has_start && (point - drawing.last_point()).norm() > EPSILON)
            add_line(ghost, drawing.last_point(), point);
    };

    if (tool == SketchTool::Line) {
        if (!m_panel.chain_active())
            travel_to(m_cursor);
        else {
            const Vec3d anchor = m_panel.anchor();
            if (!m_panel.chain_has_segments())
                travel_to(anchor);
            Vec3d middle;
            if (m_drag == Drag::Arc && GCodeSketch::tangent_arc_middle(anchor, m_panel.chain_tangent(), m_cursor, middle))
                add_arc(anchor, middle, m_cursor);
            else
                add_line(rubber, anchor, m_cursor);
        }
    } else if (tool != SketchTool::Select) {
        // The shape the tool would make there, or what is known of it; what a modify tool would do.
        SketchPreview preview;
        m_panel.preview(m_has_cursor ? &m_cursor : nullptr, on_pick ? m_pick_segment : -1, m_pick_at, preview);
        auto add_shape = [](GLModel::Geometry &lines, const GCodeSketch::Path &path) {
            Vec3d from = path.start;
            for (const GCodeSketch::Segment &segment : path.segments) {
                add_polyline(lines, segment_polyline(from, segment));
                from = segment.end;
            }
        };
        GLModel::Geometry removed = make_lines();
        for (const GCodeSketch::Path &path : preview.paths)
            add_shape(rubber, path);
        for (const GCodeSketch::Path &path : preview.removed)
            add_shape(removed, path);
        init_model(m_removed, std::move(removed));
        for (const auto &[a, b] : preview.guides)
            add_line(ghost, a, b);
        m_marks = std::move(preview.marks);
        if (!m_panel.modifying() && m_has_cursor) {
            // The travel move to the shape.
            if (!preview.paths.empty())
                travel_to(preview.paths.front().start);
            else
                travel_to(clicks.empty() ? m_cursor : clicks.front());
            // Radius of a center diameter circle.
            if (tool == SketchTool::Circle && !clicks.empty())
                add_line(ghost, clicks.front(), m_cursor);
        }
    }
    init_model(m_rubber, std::move(rubber));
    init_model(m_ghost, std::move(ghost));
}

void GCodeSketchView::on_paint(wxPaintEvent &)
{
    // The paint DC validates the area of the window, as wxWidgets requires.
    wxPaintDC dc(m_canvas);
    render();
}

void GCodeSketchView::on_size(wxSizeEvent &evt)
{
    refresh();
    evt.Skip();
}

void GCodeSketchView::render()
{
    if (m_canvas == nullptr || m_context == nullptr || !m_canvas->IsShownOnScreen())
        return;
    if (!make_current() || !wxGetApp().init_opengl())
        return;
    const auto &ogl = wxGetApp().get_opengl_manager();
    if (!ogl)
        return;

    const wxSize size  = m_canvas->GetClientSize();
    const double scale = content_scale(m_canvas);
    const int    w     = std::max(10, int(std::lround(size.GetWidth() * scale)));
    const int    h     = std::max(10, int(std::lround(size.GetHeight() * scale)));

    if (m_machine_dirty) {
        build_machine_models();
        m_machine_dirty = false;
    }
    const int axis = m_panel.plane_axis();
    if (m_plane_model_axis != axis)
        build_plane_models(axis);
    if (m_drawing_dirty) {
        build_drawing_models();
        m_drawing_dirty = false;
    }
    if (m_highlight_dirty)
        build_highlights();

    const GCodeSketch::Machine &machine = m_panel.machine();
    const GCodeSketch::Drawing &drawing = m_panel.drawing();
    const double                height  = std::max(machine.max_height, 1.);
    const BoundingBoxf3         volume(Vec3d(m_bed_min.x(), m_bed_min.y(), 0.), Vec3d(m_bed_max.x(), m_bed_max.y(), height));
    m_camera.set_scene_box(volume);
    m_camera.apply_viewport(0, 0, (unsigned int) w, (unsigned int) h);
    if (m_zoom_pending) {
        m_camera.zoom_to_box(volume);
        m_zoom_pending = false;
    }
    // The near and far planes keep the points out of the volume and the other layers visible.
    m_projection_box = volume;
    for (const GCodeSketch::PointRef &ref : GCodeSketch::list_points(drawing))
        if (const Vec3d *p = GCodeSketch::point_position(drawing, ref))
            m_projection_box.merge(*p);
    if (m_panel.stacked().has_start)
        m_projection_box.merge(m_panel.stacked().last_point());
    m_camera.apply_projection(m_projection_box);
    m_rendered = true;
    build_previews();

    const Palette colors = palette(m_panel.plane());
    ogl->bind_vao();
    // Draw on the window, whatever framebuffer the other 3D views left bound.
    if (OpenGLManager::are_framebuffers_supported()) {
        if (OpenGLManager::get_framebuffers_type() == OpenGLManager::EFramebufferType::Ext)
            glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0));
        else
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }
    glsafe(::glViewport(0, 0, w, h));
    glsafe(::glClearColor(colors.background.r(), colors.background.g(), colors.background.b(), 1.f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LEQUAL));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const auto &flat = wxGetApp().get_shader("flat");
    const auto &lit  = wxGetApp().get_shader("gouraud_light");
    if (flat && lit) {
        const Transform3d &view       = m_camera.get_view_matrix();
        const Transform3d &projection = m_camera.get_projection_matrix();
        const Transform3d  identity   = Transform3d::Identity();
        const Vec3d        volume_min(m_bed_min.x(), m_bed_min.y(), 0.);
        const Vec3d        volume_size(m_bed_max.x() - m_bed_min.x(), m_bed_max.y() - m_bed_min.y(), height);
        GLModel           *unit_lines[3] = { &m_unit_x, &m_unit_y, &m_unit_z };

        auto draw_flat = [&](GLModel &model, const ColorRGBA &color, const Transform3d &matrix) {
            if (!model.is_initialized())
                return;
            flat->set_uniform("view_model_matrix", view * matrix);
            flat->set_uniform("projection_matrix", projection);
            model.set_color(color);
            model.render_geometry();
        };
        auto draw_lit = [&](GLModel &model, const ColorRGBA &color, const Transform3d &matrix) {
            if (!model.is_initialized())
                return;
            const Transform3d view_model = view * matrix;
            lit->set_uniform("view_model_matrix", view_model);
            lit->set_uniform("projection_matrix", projection);
            lit->set_uniform("normal_matrix", (Matrix3d) view_model.matrix().block(0, 0, 3, 3).inverse().transpose());
            model.set_color(color);
            model.render_geometry();
        };
        auto bind_lit = [&](float emission) {
            wxGetApp().bind_shader(lit);
            lit->set_uniform("emission_factor", emission);
            // Clipping of the G-code preview, off here.
            lit->set_uniform("u_clip_xy_enabled", 0.f);
        };
        // Line along the axis k through the point, across the build volume.
        auto draw_axis_line = [&](int k, const Vec3d &through, const ColorRGBA &color) {
            Vec3d from  = through;
            from[k]     = volume_min[k];
            Vec3d along = Vec3d::Ones();
            along[k]    = volume_size[k];
            draw_flat(*unit_lines[k], color, placed(from, along));
        };

        // Bed, build volume, shadow and lines of the drawing.
        wxGetApp().bind_shader(flat);
        ogl->set_line_width(1.f);
        draw_flat(m_bed, colors.bed, identity);
        draw_flat(m_bed_grid, colors.bed_grid, identity);
        draw_flat(m_bed_outline, colors.bed_outline, identity);
        draw_flat(m_volume, colors.volume, identity);
        draw_flat(m_exclude, colors.exclude, identity);
        draw_flat(m_shadow, colors.shadow, identity);
        draw_flat(m_layer_lines, colors.layers, identity);
        ogl->set_line_width(2.f);
        draw_flat(m_print_lines, colors.print, identity);
        draw_flat(m_travel_lines, colors.travel, identity);
        wxGetApp().unbind_shader();

        // Extrusions and points.
        bind_lit(0.1f);
        draw_lit(m_beads, colors.print, identity);
        const SketchSelection                   &selection = m_panel.selection();
        const std::vector<GCodeSketch::PointRef> points    = GCodeSketch::list_points(drawing);
        std::vector<bool>                        with_issue(points.size(), false);
        for (const GCodeSketch::Issue &issue : m_panel.issues())
            if (issue.point < with_issue.size())
                with_issue[issue.point] = true;
        const double radius = pixels_to_mm(POINT_RADIUS);
        for (size_t i = 0; i < points.size(); ++ i) {
            const bool selected = selection.kind == SketchSelection::Kind::Point && selection.point == points[i];
            // Too many points (imported G-code): only the selected one and the one under the mouse.
            if (points.size() > MAX_SPHERES && !selected && int(i) != m_hover_point)
                continue;
            const Vec3d *p = GCodeSketch::point_position(drawing, points[i]);
            if (p == nullptr)
                continue;
            ColorRGBA color = points[i].mid ? colors.mid_point : colors.point;
            if (with_issue[i])
                color = colors.issue;
            if (selected)
                color = colors.selected;
            else if (int(i) == m_hover_point)
                color = colors.hovered;
            const double r = (points[i].mid ? 0.75 : 1.) * (selected ? 1.3 : 1.) * radius;
            draw_lit(m_sphere, color, placed(*p, Vec3d(r, r, r)));
        }
        // Points clicked of the shape being drawn.
        for (const Vec3d &click : m_panel.clicks()) {
            const double r = 0.85 * radius;
            draw_lit(m_sphere, colors.mid_point, placed(click, Vec3d(r, r, r)));
        }
        if (m_panel.chain_active() && !m_panel.chain_has_segments()) {
            const double r = 0.85 * radius;
            draw_lit(m_sphere, colors.mid_point, placed(m_panel.anchor(), Vec3d(r, r, r)));
        }
        wxGetApp().unbind_shader();

        // See-through working plane.
        wxGetApp().bind_shader(flat);
        glsafe(::glDepthMask(GL_FALSE));
        const Transform3d plane = placed(Vec3d::Unit(axis) * m_panel.plane_offset(), Vec3d::Ones());
        draw_flat(m_plane, colors.plane, plane);
        ogl->set_line_width(1.f);
        draw_flat(m_plane_grid, colors.plane_grid, plane);
        glsafe(::glDepthMask(GL_TRUE));

        if (m_has_cursor) {
            const ColorRGBA &tone = m_cursor_outside ? colors.cursor_outside : colors.cursor;
            // Lines across the volume through the cursor, along the axes of the plane.
            ColorRGBA cross = tone;
            cross.a(0.6f);
            draw_axis_line(first_axis(axis), m_cursor, cross);
            draw_axis_line(second_axis(axis), m_cursor, cross);
            // Stem down to the bed and shadow on it.
            if (m_cursor.z() > EPSILON)
                draw_flat(m_unit_z, tone, placed(Vec3d(m_cursor.x(), m_cursor.y(), 0.), Vec3d(1., 1., m_cursor.z())));
            const double shadow = pixels_to_mm(CURSOR_RADIUS);
            draw_flat(m_disc, colors.shadow, placed(Vec3d(m_cursor.x(), m_cursor.y(), SHADOW_Z), Vec3d(shadow, shadow, 1.)));
            // Axis of the last point followed by the cursor.
            if (m_inferred_axis >= 0 && m_inferred_axis < 3) {
                ogl->set_line_width(2.f);
                draw_axis_line(m_inferred_axis, m_anchor, colors.axes[m_inferred_axis]);
                ogl->set_line_width(1.f);
            }
        }
        // Highlights, segment or shape being drawn and cursor, over everything.
        glsafe(::glDisable(GL_DEPTH_TEST));
        ogl->set_line_width(3.f);
        draw_flat(m_hover_lines, colors.hovered, identity);
        draw_flat(m_selected_lines, colors.selected, identity);
        draw_flat(m_picked, colors.selected, identity);
        draw_flat(m_removed, colors.issue, identity);
        ogl->set_line_width(1.f);
        draw_flat(m_ghost, colors.ghost, identity);
        ogl->set_line_width(2.f);
        draw_flat(m_rubber, m_panel.extrude() ? colors.print : colors.travel, identity);
        wxGetApp().unbind_shader();
        if (m_has_cursor) {
            bind_lit(0.25f);
            const double r = pixels_to_mm(CURSOR_RADIUS);
            draw_lit(m_sphere, m_cursor_outside ? colors.cursor_outside : colors.cursor, placed(m_cursor, Vec3d(r, r, r)));
            wxGetApp().unbind_shader();
        }
        // Where a curve would be split.
        if (!m_marks.empty()) {
            bind_lit(0.25f);
            const double r = 1.2 * pixels_to_mm(POINT_RADIUS);
            for (const Vec3d &mark : m_marks)
                draw_lit(m_sphere, colors.hovered, placed(mark, Vec3d(r, r, r)));
            wxGetApp().unbind_shader();
        }
        glsafe(::glEnable(GL_DEPTH_TEST));
    }

    // Constraints and typed values, over the view.
    render_overlay(w, h);

    // Back to the state the other 3D views expect (GLCanvas3D::init()): they share the OpenGL context.
    ogl->set_line_width(1.f);
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glClearColor(1.f, 1.f, 1.f, 1.f));
    ogl->unbind_vao();
    m_canvas->SwapBuffers();
}

void GCodeSketchView::render_overlay(int width, int height)
{
    // The values typed for a modify tool picking under the mouse (fillet radius, chamfer distances) show by the mouse.
    const bool at_mouse    = !m_has_cursor && m_fields_at_mouse && m_has_mouse;
    const bool show_fields = (m_has_cursor || at_mouse) && !m_fields.empty();
    const bool show_glyph  = m_has_cursor && m_snap != Snap::None && m_snap != Snap::Grid;
    const bool show_lines  = m_has_cursor && !m_alignments.empty();
    ImGuiWrapper *imgui    = wxGetApp().imgui();
    if ((!show_fields && !show_glyph && !show_lines) || imgui == nullptr)
        return;
    // The ImGui of the 3D views draws here for a frame, at the size of this view.
    ImGuiIO     &io    = ImGui::GetIO();
    const ImVec2 saved = io.DisplaySize;
    imgui->set_display_size(float(width), float(height));
    imgui->new_frame();
    ImDrawList     *draw    = ImGui::GetForegroundDrawList();
    const Projector project = projector();
    Vec2d           cursor  = m_mouse;
    const bool      shown   = m_has_cursor ? project(m_cursor, cursor) : at_mouse;
    if (draw != nullptr && shown) {
        // Points the cursor lines up with.
        for (const Alignment &alignment : m_alignments) {
            Vec2d from;
            if (project(alignment.from, from))
                dashed_line(*draw, from, cursor, axis_colour(alignment.coordinate), 1.5f);
        }
        if (show_fields)
            draw_fields(*draw, project, cursor, width, height);
        if (show_glyph)
            draw_glyph(*draw, cursor);
    }
    imgui->render();
    imgui->set_display_size(saved.x, saved.y);
}

void GCodeSketchView::draw_glyph(ImDrawList &draw, const Vec2d &cursor) const
{
    const bool   dark   = wxGetApp().dark_mode();
    const ImU32  back   = dark ? IM_COL32(32, 32, 37, 235) : IM_COL32(255, 255, 255, 235);
    const ImU32  border = dark ? IM_COL32(110, 110, 118, 255) : IM_COL32(160, 165, 172, 255);
    const ImU32  ink    = dark ? IM_COL32(235, 235, 238, 255) : IM_COL32(35, 38, 42, 255);
    const ImU32  green  = IM_COL32(8, 119, 232, 255);
    const float  size   = 18.f;
    // Above right of the cursor, as the constraints of Fusion 360.
    const ImVec2 min(float(cursor.x()) + 10.f, float(cursor.y()) - 10.f - size);
    const ImVec2 max(min.x + size, min.y + size);
    const ImVec2 c(0.5f * (min.x + max.x), 0.5f * (min.y + max.y));
    draw.AddRectFilled(min, max, back, 4.f);
    draw.AddRect(min, max, border, 4.f);
    switch (m_snap) {
    case Snap::End: draw.AddRectFilled(ImVec2(c.x - 3.5f, c.y - 3.5f), ImVec2(c.x + 3.5f, c.y + 3.5f), green); break;
    case Snap::Passage:
        draw.AddQuadFilled(ImVec2(c.x, c.y - 5.f), ImVec2(c.x + 5.f, c.y), ImVec2(c.x, c.y + 5.f), ImVec2(c.x - 5.f, c.y), IM_COL32(26, 166, 153, 255));
        break;
    case Snap::Middle: draw.AddTriangleFilled(ImVec2(c.x, c.y - 5.f), ImVec2(c.x + 5.5f, c.y + 4.f), ImVec2(c.x - 5.5f, c.y + 4.f), green); break;
    case Snap::Center:
        draw.AddCircle(c, 5.f, ink, 16, 1.5f);
        draw.AddCircleFilled(c, 1.8f, ink, 8);
        break;
    case Snap::Axis:
        if (m_inferred_axis >= 0 && m_inferred_axis < 3) {
            static const char *names[] = { "X", "Y", "Z" };
            const ImVec2 text = ImGui::CalcTextSize(names[m_inferred_axis]);
            draw.AddText(ImVec2(c.x - 0.5f * text.x, c.y - 0.5f * text.y), axis_colour(m_inferred_axis), names[m_inferred_axis]);
        }
        break;
    case Snap::Perpendicular:
        draw.AddLine(ImVec2(c.x - 5.f, c.y + 5.f), ImVec2(c.x + 5.f, c.y + 5.f), ink, 1.5f);
        draw.AddLine(ImVec2(c.x, c.y + 5.f), ImVec2(c.x, c.y - 5.f), ink, 1.5f);
        break;
    case Snap::Parallel:
        draw.AddLine(ImVec2(c.x - 4.5f, c.y + 5.f), ImVec2(c.x - 1.f, c.y - 5.f), ink, 1.5f);
        draw.AddLine(ImVec2(c.x + 1.f, c.y + 5.f), ImVec2(c.x + 4.5f, c.y - 5.f), ink, 1.5f);
        break;
    case Snap::Tangent:
        draw.AddCircle(ImVec2(c.x, c.y + 2.f), 3.5f, ink, 16, 1.5f);
        draw.AddLine(ImVec2(c.x - 6.f, c.y - 1.5f), ImVec2(c.x + 6.f, c.y - 1.5f), ink, 1.5f);
        break;
    case Snap::Normal:
        draw.AddLine(ImVec2(c.x, c.y - 3.f), ImVec2(c.x, c.y + 3.f), ink, 1.5f);
        draw.AddTriangleFilled(ImVec2(c.x, c.y - 6.5f), ImVec2(c.x + 3.5f, c.y - 2.5f), ImVec2(c.x - 3.5f, c.y - 2.5f), ink);
        draw.AddTriangleFilled(ImVec2(c.x, c.y + 6.5f), ImVec2(c.x - 3.5f, c.y + 2.5f), ImVec2(c.x + 3.5f, c.y + 2.5f), ink);
        break;
    default: break;
    }
}

void GCodeSketchView::draw_fields(ImDrawList &draw, const Projector &project, const Vec2d &cursor, int width, int height) const
{
    const bool  dark        = wxGetApp().dark_mode();
    const ImU32 back        = dark ? IM_COL32(40, 40, 45, 240) : IM_COL32(255, 255, 255, 240);
    const ImU32 border      = dark ? IM_COL32(100, 100, 108, 255) : IM_COL32(170, 175, 182, 255);
    const ImU32 ink         = dark ? IM_COL32(238, 238, 240, 255) : IM_COL32(30, 34, 38, 255);
    const ImU32 faint       = dark ? IM_COL32(175, 175, 182, 255) : IM_COL32(90, 96, 102, 255);
    const ImU32 locked_back = dark ? IM_COL32(28, 72, 44, 255) : IM_COL32(220, 245, 229, 255);
    const ImU32 green       = IM_COL32(8, 119, 232, 255);
    // Lines of the dimensions, as the sketch dimensions of Fusion 360.
    const ImU32 line        = dark ? IM_COL32(140, 175, 235, 255) : IM_COL32(35, 85, 170, 255);
    const float font        = ImGui::GetFontSize();
    // Boxes with no place on the shape: by the cursor, one under the other.
    double      stacked_y   = cursor.y() + 24.;
    // Axes of the working plane, which name the counts and spacings of the rectangular pattern.
    static const char *axis_names[] = { "X", "Y", "Z" };
    const int          normal       = m_panel.plane_axis();

    // Arrow head at tip, pointing along the unit direction.
    auto arrow = [&](const Vec2d &tip, const Vec2d &direction) {
        const Vec2d side(-direction.y(), direction.x());
        const Vec2d base = tip - 8. * direction;
        draw.AddTriangleFilled(to_imgui(tip), to_imgui(Vec2d(base + 3. * side)), to_imgui(Vec2d(base - 3. * side)), line);
    };

    for (size_t i = 0; i < m_fields.size(); ++ i) {
        const Field     &field     = m_fields[i];
        const Dimension &dimension = field.dimension;
        const bool       active    = int(i) == m_active_field;
        const bool       typed     = !field.text.empty();

        // Its value: typed, otherwise the one of the cursor.
        std::string text;
        if (typed)
            text = field.text.ToStdString() + (active && !field.locked ? "|" : "");
        else if (field.unknown)
            text = "-";
        else {
            char buffer[64];
            switch (field.type) {
            case FieldType::Angle:
            case FieldType::Sweep: std::snprintf(buffer, sizeof(buffer), "%.1f", field.value); break;
            case FieldType::Sides:
            case FieldType::Instances:
            case FieldType::InstancesV: std::snprintf(buffer, sizeof(buffer), "%d", int(std::lround(field.value))); break;
            case FieldType::Factor: std::snprintf(buffer, sizeof(buffer), "%.3f", field.value); break;
            default: std::snprintf(buffer, sizeof(buffer), "%.2f", field.value); break;
            }
            text = buffer;
        }
        // What it is.
        switch (field.type) {
        case FieldType::Angle:
        case FieldType::Sweep: text += "\xC2\xB0"; break;
        case FieldType::Diameter: text = "\xC3\x98 " + text; break;
        case FieldType::Radius: text = "R " + text; break;
        case FieldType::Sides: text += " " + _u8L("sides"); break;
        case FieldType::Rho: text = "rho " + text; break;
        case FieldType::Instances:
            text = (m_panel.tool() == SketchTool::RectangularPattern ? std::string(axis_names[first_axis(normal)]) + " " : std::string()) +
                   "\xC3\x97 " + text;
            break;
        case FieldType::InstancesV: text = std::string(axis_names[second_axis(normal)]) + " \xC3\x97 " + text; break;
        case FieldType::SpacingU: text = std::string(axis_names[first_axis(normal)]) + " " + text; break;
        case FieldType::SpacingV: text = std::string(axis_names[second_axis(normal)]) + " " + text; break;
        case FieldType::Distance:
            if (m_panel.tool() == SketchTool::ChamferTwoDistances)
                text = "d1 " + text;
            else if (m_panel.tool() != SketchTool::Offset)
                text = "d " + text;
            break;
        case FieldType::Distance2: text = "d2 " + text; break;
        case FieldType::Factor: text = "\xC3\x97 " + text; break;
        default: break;
        }

        // Its dimension on the shape, and where its box goes.
        Vec2d box    = Vec2d::Zero();
        bool  has_place = false;
        switch (dimension.kind) {
        case DimensionKind::Linear: {
            Vec2d a, b;
            if (!project(dimension.a, a) || !project(dimension.b, b))
                break;
            const Vec2d  d      = b - a;
            const double length = d.norm();
            has_place           = true;
            if (length < 2.) {
                box = a + Vec2d(0., -28.);
                break;
            }
            const Vec2d dir = d / length;
            Vec2d       side(-dir.y(), dir.x());
            // Beside the segment, above it on the screen.
            if (side.y() > 0.)
                side = -side;
            const Vec2d from = a + 24. * side;
            const Vec2d to   = b + 24. * side;
            // Extension lines, then the dimension line and its arrows.
            draw.AddLine(to_imgui(Vec2d(a + 4. * side)), to_imgui(Vec2d(from + 4. * side)), line, 1.f);
            draw.AddLine(to_imgui(Vec2d(b + 4. * side)), to_imgui(Vec2d(to + 4. * side)), line, 1.f);
            draw.AddLine(to_imgui(from), to_imgui(to), line, 1.2f);
            if (length > 24.) {
                arrow(from, Vec2d(-dir));
                arrow(to, dir);
            }
            box = 0.5 * (from + to);
            break;
        }
        case DimensionKind::Angular: {
            Vec2d vertex;
            if (!project(dimension.a, vertex))
                break;
            has_place = true;
            Vec3d from = dimension.b - dimension.a;
            from -= from.dot(dimension.axis) * dimension.axis;
            if (from.norm() < 1e-9 || std::abs(dimension.angle) < 1e-9) {
                box = vertex + Vec2d(34., -30.);
                break;
            }
            from.normalize();
            const Vec3d  across = dimension.axis.cross(from);
            // The arc of the angle, about 46 pixels from its vertex, and its reference direction dashed.
            const double radius = pixels_to_mm(46.);
            const int    steps  = std::clamp(int(std::abs(dimension.angle) / (PI / 36.)) + 2, 3, 80);
            std::vector<ImVec2> points;
            for (int k = 0; k <= steps; ++ k) {
                const double phi = dimension.angle * double(k) / double(steps);
                Vec2d        s;
                if (!project(Vec3d(dimension.a + radius * (std::cos(phi) * from + std::sin(phi) * across)), s)) {
                    points.clear();
                    break;
                }
                points.push_back(to_imgui(s));
            }
            Vec2d reference;
            if (project(Vec3d(dimension.a + 1.6 * radius * from), reference))
                dashed_line(draw, vertex, reference, line, 1.f);
            if (points.size() >= 2)
                draw.AddPolyline(points.data(), int(points.size()), line, 0, 1.2f);
            // The box beyond the middle of the arc.
            const double half = 0.5 * dimension.angle;
            Vec2d        outside;
            if (project(Vec3d(dimension.a + 1.6 * radius * (std::cos(half) * from + std::sin(half) * across)), outside))
                box = outside;
            else
                box = vertex + Vec2d(34., -30.);
            break;
        }
        case DimensionKind::Radial: {
            Vec2d center, point;
            if (!project(dimension.a, center) || !project(dimension.b, point))
                break;
            has_place = true;
            draw.AddLine(to_imgui(center), to_imgui(point), line, 1.2f);
            draw.AddCircleFilled(to_imgui(center), 2.5f, line, 8);
            const Vec2d d = point - center;
            if (d.norm() > 12.)
                arrow(point, Vec2d(d.normalized()));
            box = 0.5 * (center + point);
            break;
        }
        case DimensionKind::Diametral: {
            Vec2d center, point, opposite;
            if (!project(dimension.a, center) || !project(dimension.b, point) || !project(Vec3d(2. * dimension.a - dimension.b), opposite))
                break;
            has_place = true;
            draw.AddLine(to_imgui(opposite), to_imgui(point), line, 1.2f);
            const Vec2d d = point - opposite;
            if (d.norm() > 24.) {
                arrow(point, Vec2d(d.normalized()));
                arrow(opposite, Vec2d(-d.normalized()));
            }
            box = 0.5 * (center + point);
            break;
        }
        case DimensionKind::Label: {
            Vec2d at;
            if (!project(dimension.a, at))
                break;
            has_place = true;
            box    = at + Vec2d(0., -30.);
            break;
        }
        default: break;
        }
        if (!has_place) {
            box = Vec2d(cursor.x() + 70., stacked_y);
            stacked_y += font + 16.;
        }

        // The box: green border for the value being typed, a padlock for the values kept (Tab).
        const float text_w = ImGui::CalcTextSize(text.c_str()).x;
        const float lock_w = field.locked ? 14.f : 0.f;
        const float w      = text_w + 14.f + lock_w;
        const float h      = font + 8.f;
        const float x      = std::clamp(float(box.x()) - 0.5f * w, 2.f, std::max(2.f, float(width) - w - 2.f));
        const float y      = std::clamp(float(box.y()) - 0.5f * h, 2.f, std::max(2.f, float(height) - h - 2.f));
        draw.AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), field.locked ? locked_back : back, 4.f);
        draw.AddRect(ImVec2(x, y), ImVec2(x + w, y + h), active ? green : border, 4.f, 0, active ? 2.f : 1.f);
        if (field.locked) {
            const ImVec2 lock(x + 9.f, y + 0.5f * h + 1.f);
            draw.AddRectFilled(ImVec2(lock.x - 4.f, lock.y - 1.f), ImVec2(lock.x + 4.f, lock.y + 5.f), green, 1.f);
            draw.PathArcTo(ImVec2(lock.x, lock.y - 1.f), 2.8f, float(PI), float(2. * PI), 8);
            draw.PathStroke(green, 0, 1.5f);
        }
        draw.AddText(ImVec2(x + 7.f + lock_w, y + 4.f), typed ? ink : faint, text.c_str());
    }
}

GCodeSketchView::Projector GCodeSketchView::projector() const
{
    Projector                  project;
    const std::array<int, 4> &viewport = m_camera.get_viewport();
    project.matrix = m_camera.get_projection_matrix().matrix() * m_camera.get_view_matrix().matrix();
    project.x      = double(viewport[0]);
    project.y      = double(viewport[1]);
    project.width  = double(std::max(1, viewport[2]));
    project.height = double(std::max(1, viewport[3]));
    return project;
}

Vec2d GCodeSketchView::mouse_position(const wxMouseEvent &evt) const
{
    const double scale = content_scale(m_canvas);
    return Vec2d(evt.GetX() * scale, evt.GetY() * scale);
}

bool GCodeSketchView::mouse_on_plane(const Vec2d &mouse, int axis, double offset, Vec3d &hit) const
{
    Vec3d origin;
    Vec3d direction;
    CameraUtils::ray_from_screen_pos(m_camera, mouse, origin, direction);
    if (std::abs(direction[axis]) < 1e-9)
        return false;
    const double t = (offset - origin[axis]) / direction[axis];
    if (t < 0.)
        return false;
    hit = origin + t * direction;
    return true;
}

bool GCodeSketchView::mouse_on_line(const Vec2d &mouse, const Vec3d &origin, const Vec3d &direction, double &t) const
{
    Vec3d ray_origin;
    Vec3d ray_direction;
    CameraUtils::ray_from_screen_pos(m_camera, mouse, ray_origin, ray_direction);
    // Closest points of the two lines.
    const Vec3d  w0    = origin - ray_origin;
    const double a     = direction.squaredNorm();
    const double b     = direction.dot(ray_direction);
    const double c     = ray_direction.squaredNorm();
    const double d     = direction.dot(w0);
    const double e     = ray_direction.dot(w0);
    const double denom = a * c - b * b;
    // Line seen end on: no position can be read along it.
    if (denom < 1e-9 * a * c)
        return false;
    t = (b * e - c * d) / denom;
    return true;
}

bool GCodeSketchView::infer_axis(const Vec2d &mouse, const Vec3d &anchor, double plane_offset, bool in_plane_only, Vec3d &point, int &axis) const
{
    const int       normal   = m_panel.plane_axis();
    const bool      on_plane = std::abs(anchor[normal] - plane_offset) < EPSILON;
    const Projector project  = projector();
    const double    length   = pixels_to_mm(100.);
    double          best     = AXIS_RADIUS;
    Vec2d           a;
    axis = -1;
    if (!project(anchor, a))
        return false;
    for (int k = 0; k < 3; ++ k) {
        // The axes of the plane when it goes through the anchor, its normal unless the cursor must stay in the plane.
        if (k == normal ? in_plane_only : !on_plane)
            continue;
        Vec2d b;
        if (!project(anchor + Vec3d::Unit(k) * length, b))
            continue;
        Vec2d        d     = b - a;
        const double image = d.norm();
        // Axis seen almost end on: it can't be followed.
        if (image < 25.)
            continue;
        d /= image;
        const Vec2d  m        = mouse - a;
        const double distance = std::abs(d.x() * m.y() - d.y() * m.x());
        if (distance < best) {
            best = distance;
            axis = k;
        }
    }
    if (axis < 0)
        return false;
    double t = 0.;
    if (!mouse_on_line(mouse, anchor, Vec3d::Unit(axis), t)) {
        axis = -1;
        return false;
    }
    // Close to the anchor, the cursor stays on the plane.
    if (axis == normal && std::abs(t) < pixels_to_mm(PICK_RADIUS)) {
        axis = -1;
        return false;
    }
    point       = anchor;
    point[axis] = snap_along(anchor[axis] + t, axis);
    return true;
}

bool GCodeSketchView::infer_direction(const Vec2d &mouse, const Vec3d &anchor, Vec3d &point, Snap &kind) const
{
    const GCodeSketch::Drawing &drawing = m_panel.drawing();
    const int                   normal  = m_panel.plane_axis();
    // The last segment of the line, in the working plane.
    Vec3d reference = m_panel.chain_tangent();
    reference[normal] = 0.;
    if (reference.norm() < 0.1 || std::abs(point[normal] - anchor[normal]) > EPSILON)
        return false;
    reference.normalize();
    const Projector project = projector();
    Vec2d           a;
    if (!project(anchor, a) || (mouse - a).norm() < MIN_DIRECTION)
        return false;
    // Along the end of an arc, a line is tangent to it.
    const bool  after_arc = !drawing.segments.empty() && drawing.segments.back().type == GCodeSketch::SegmentType::Arc;
    const Vec3d across    = Vec3d::Unit(normal).cross(reference);
    const std::pair<Vec3d, Snap> candidates[] = { { reference, after_arc ? Snap::Tangent : Snap::Parallel },
                                                  { Vec3d(-reference), Snap::Parallel },
                                                  { across, Snap::Perpendicular },
                                                  { Vec3d(-across), Snap::Perpendicular } };
    double best = AXIS_RADIUS;
    bool   found = false;
    Vec3d  best_point = point;
    for (const auto &[direction, candidate] : candidates) {
        const double along = (point - anchor).dot(direction);
        if (along <= 0.)
            continue;
        const Vec3d on_line = anchor + along * direction;
        Vec2d       screen;
        if (!project(on_line, screen))
            continue;
        const double distance = (screen - mouse).norm();
        if (distance < best) {
            best       = distance;
            best_point = on_line;
            kind       = candidate;
            found      = true;
        }
    }
    if (!found)
        return false;
    // Along an axis, the length follows the grid.
    for (int k = 0; k < 3; ++ k)
        if (k != normal && std::abs(best_point[k] - anchor[k]) > EPSILON) {
            const int other = 3 - normal - k;
            if (std::abs(best_point[other] - anchor[other]) <= EPSILON)
                best_point[k] = snap_along(best_point[k], k);
        }
    point = best_point;
    return true;
}

bool GCodeSketchView::infer_tangent_arc(const Vec2d &mouse, Vec3d &point) const
{
    const GCodeSketch::Drawing &drawing = m_panel.drawing();
    const std::vector<Vec3d>   &clicks  = m_panel.clicks();
    // The arc starts at the end of the drawing: it can continue its last segment.
    if (clicks.size() != 2 || drawing.segments.empty() || (clicks[0] - drawing.last_point()).norm() > EPSILON)
        return false;
    const Vec3d tangent = GCodeSketch::end_tangent(drawing, drawing.segments.size() - 1);
    Vec3d       middle;
    if (!GCodeSketch::tangent_arc_middle(clicks[0], tangent, clicks[1], middle))
        return false;
    const GCodeSketch::Arc arc = GCodeSketch::arc_through(clicks[0], middle, clicks[1]);
    if (!arc.valid)
        return false;
    // Point of the circle of the tangent arc the closest to the cursor.
    Vec3d radial = point - arc.center;
    radial -= radial.dot(arc.normal) * arc.normal;
    if (radial.norm() < EPSILON)
        return false;
    const Vec3d on_arc = arc.center + arc.radius * radial.normalized();
    // On the arc itself, not on the rest of its circle, which would turn the other way.
    const GCodeSketch::Arc through = GCodeSketch::arc_through(clicks[0], on_arc, clicks[1]);
    if (!through.valid || through.normal.dot(arc.normal) < 0.)
        return false;
    Vec2d screen;
    if (!projector()(on_arc, screen) || (screen - mouse).norm() > PICK_RADIUS)
        return false;
    point = on_arc;
    return true;
}

void GCodeSketchView::infer_alignments(const Vec2d &mouse, const Vec3d &anchor, bool has_anchor, Vec3d &point)
{
    const int        normal = m_panel.plane_axis();
    std::vector<int> coordinates;
    if (m_inferred_axis >= 0)
        // Along the axis followed, only its coordinate can change.
        coordinates.push_back(m_inferred_axis);
    else {
        coordinates.push_back(first_axis(normal));
        coordinates.push_back(second_axis(normal));
    }
    const Projector project = projector();
    const size_t    count   = m_targets.size();
    const size_t    begin   = count > MAX_ALIGNED ? count - MAX_ALIGNED : 0;
    for (const int c : coordinates) {
        double best        = ALIGN_RADIUS;
        int    best_target = -1;
        for (size_t i = begin; i < count; ++ i) {
            const Target &target = m_targets[i];
            if (target.kind != Snap::End && target.kind != Snap::Center)
                continue;
            // The axes of the anchor are already followed.
            if ((has_anchor && (target.position - anchor).norm() < EPSILON) || (target.position - point).norm() < EPSILON)
                continue;
            Vec3d candidate = point;
            candidate[c]    = target.position[c];
            Vec2d screen;
            if (!project(candidate, screen))
                continue;
            const double distance = (screen - mouse).norm();
            if (distance <= best) {
                best        = distance;
                best_target = int(i);
            }
        }
        if (best_target >= 0) {
            point[c] = m_targets[size_t(best_target)].position[c];
            m_alignments.push_back({ m_targets[size_t(best_target)].position, c });
        }
    }
}

int GCodeSketchView::point_under_mouse(const Vec2d &mouse) const
{
    if (!m_rendered)
        return -1;
    const GCodeSketch::Drawing               &drawing = m_panel.drawing();
    const std::vector<GCodeSketch::PointRef> points  = GCodeSketch::list_points(drawing);
    const Projector                          project = projector();
    int                                      best    = -1;
    double                                   best_d2 = PICK_RADIUS * PICK_RADIUS;
    for (size_t i = 0; i < points.size(); ++ i) {
        Vec2d screen;
        if (!project(*GCodeSketch::point_position(drawing, points[i]), screen))
            continue;
        const double d2 = (screen - mouse).squaredNorm();
        // The last point drawn wins over the ones it covers.
        if (d2 <= best_d2) {
            best_d2 = d2;
            best    = int(i);
        }
    }
    return best;
}

int GCodeSketchView::target_under_mouse(const Vec2d &mouse) const
{
    if (!m_rendered)
        return -1;
    const Projector project = projector();
    auto rank = [](Snap kind) { return kind == Snap::End ? 0 : kind == Snap::Passage ? 1 : kind == Snap::Center ? 2 : 3; };
    int    best      = -1;
    int    best_rank = 4;
    double best_d2   = 0.;
    for (size_t i = 0; i < m_targets.size(); ++ i) {
        Vec2d screen;
        if (!project(m_targets[i].position, screen))
            continue;
        const double d2 = (screen - mouse).squaredNorm();
        if (d2 > PICK_RADIUS * PICK_RADIUS)
            continue;
        // The ends before the passages, the centers and the middles; then the closest, the last drawn first.
        const int r = rank(m_targets[i].kind);
        if (r < best_rank || (r == best_rank && d2 <= best_d2)) {
            best      = int(i);
            best_rank = r;
            best_d2   = d2;
        }
    }
    return best;
}

int GCodeSketchView::segment_under_mouse(const Vec2d &mouse, Vec3d *at) const
{
    const GCodeSketch::Drawing &drawing = m_panel.drawing();
    if (!m_rendered || !drawing.has_start)
        return -1;
    const Projector project = projector();
    double          best    = PICK_RADIUS;
    int             found   = -1;
    for (size_t idx = 0; idx < drawing.segments.size(); ++ idx) {
        const std::vector<Vec3d> polyline = segment_polyline(drawing.segment_start(idx), drawing.segments[idx], PICK_DEVIATION, 64);
        Vec2d                    previous;
        Vec3d                    previous_point = Vec3d::Zero();
        bool                     has_previous   = false;
        for (const Vec3d &p : polyline) {
            Vec2d screen;
            if (!project(p, screen)) {
                has_previous = false;
                continue;
            }
            if (has_previous) {
                const double distance = distance_to_segment(mouse, previous, screen);
                // The last segment drawn wins over the ones it covers.
                if (distance <= best) {
                    best  = distance;
                    found = int(idx);
                    if (at != nullptr) {
                        // Its point there, from where the mouse is along the piece on the screen.
                        const Vec2d  piece = screen - previous;
                        const double l2    = piece.squaredNorm();
                        const double t     = l2 > 0. ? std::clamp((mouse - previous).dot(piece) / l2, 0., 1.) : 0.;
                        *at                = previous_point + t * (p - previous_point);
                    }
                }
            }
            previous       = screen;
            previous_point = p;
            has_previous   = true;
        }
    }
    return found;
}

int GCodeSketchView::line_under_mouse(const Vec2d &mouse, Vec3d &at) const
{
    const GCodeSketch::Drawing &drawing = m_panel.drawing();
    if (!m_rendered || !drawing.has_start)
        return -1;
    const Projector project = projector();
    double          best    = PICK_RADIUS;
    int             found   = -1;
    for (size_t idx = 0; idx < drawing.segments.size(); ++ idx) {
        const GCodeSketch::Segment &segment = drawing.segments[idx];
        const Vec3d                &from    = drawing.segment_start(idx);
        if (segment.type != GCodeSketch::SegmentType::Line || (segment.end - from).norm() < EPSILON)
            continue;
        Vec2d a, b;
        if (!project(from, a) || !project(segment.end, b))
            continue;
        const Vec2d  ab       = b - a;
        const double l2       = ab.squaredNorm();
        const double t        = l2 > 0. ? std::clamp((mouse - a).dot(ab) / l2, 0., 1.) : 0.;
        const double distance = (a + t * ab - mouse).norm();
        // The last line drawn wins over the ones it covers.
        if (distance <= best) {
            best  = distance;
            found = int(idx);
            at    = from + t * (segment.end - from);
        }
    }
    return found;
}

bool GCodeSketchView::on_plane_grid(const Vec2d &mouse, Vec3d &hit) const
{
    if (!m_rendered)
        return false;
    const int axis = m_panel.plane_axis();
    if (!mouse_on_plane(mouse, axis, m_panel.plane_offset(), hit))
        return false;
    // Its grid spans the build volume.
    const Vec3d min(m_bed_min.x(), m_bed_min.y(), 0.);
    const Vec3d max(m_bed_max.x(), m_bed_max.y(), std::max(m_panel.machine().max_height, 1.));
    for (int k = 0; k < 3; ++ k)
        if (k != axis && (hit[k] < min[k] - EPSILON || hit[k] > max[k] + EPSILON))
            return false;
    return true;
}

double GCodeSketchView::snap_along(double value, int axis) const
{
    const double step = axis == 2 ? m_panel.z_step() : m_panel.grid_step();
    return step > 0. ? std::round(value / step) * step : value;
}

Vec3d GCodeSketchView::snap_in_plane(const Vec3d &point, int axis) const
{
    Vec3d snapped = point;
    for (int k : { first_axis(axis), second_axis(axis) })
        snapped[k] = snap_along(point[k], k);
    return snapped;
}

Vec3d GCodeSketchView::clamp_to_volume(const Vec3d &point) const
{
    return Vec3d(std::clamp(point.x(), m_bed_min.x(), m_bed_max.x()), std::clamp(point.y(), m_bed_min.y(), m_bed_max.y()),
                 std::clamp(point.z(), 0., std::max(m_panel.machine().max_height, 1.)));
}

double GCodeSketchView::pixels_to_mm(double pixels) const
{
    // Size of a pixel at the target of the camera.
    return pixels * m_camera.get_inv_zoom();
}

void GCodeSketchView::hide_cursor()
{
    m_has_cursor      = false;
    m_snap            = Snap::None;
    m_inferred_axis   = -1;
    m_hover_point     = -1;
    m_pick_segment    = -1;
    m_fields_at_mouse = false;
    m_alignments.clear();
    set_hover_segment(-1);
    m_panel.show_cursor(nullptr, wxEmptyString);
    refresh();
}

void GCodeSketchView::set_hover_segment(int segment, HoverSpan span)
{
    size_t first = segment >= 0 ? size_t(segment) : 0;
    size_t last  = first;
    if (segment >= 0 && size_t(segment) < m_panel.drawing().segments.size()) {
        if (span == HoverSpan::Shape)
            m_panel.run_at(size_t(segment), first, last);
        else if (span == HoverSpan::Path) {
            // The closed outline holding it, as a click picks it (a rectangle drawn after a line...), otherwise the whole path.
            if (!GCodeSketch::find_loop(m_panel.drawing(), size_t(segment), first, last))
                GCodeSketch::find_path(m_panel.drawing(), size_t(segment), first, last);
        }
    }
    if (segment == m_hover_segment && first == m_hover_first && last == m_hover_last)
        return;
    m_hover_segment   = segment;
    m_hover_first     = first;
    m_hover_last      = last;
    m_highlight_dirty = true;
}

void GCodeSketchView::update_cursor_here()
{
    if (m_canvas == nullptr || (m_drag != Drag::None && m_drag != Drag::Arc))
        return;
    const wxPoint mouse = m_canvas->ScreenToClient(wxGetMousePosition());
    const double  scale = content_scale(m_canvas);
    Vec2d         position(mouse.x * scale, mouse.y * scale);
    if (!m_canvas->GetClientRect().Contains(mouse)) {
        // Typed values place the point from the last position of the mouse in the view.
        if (!m_has_mouse || m_fields.empty())
            return;
        position = m_mouse;
    }
    update_cursor(position, wxGetKeyState(WXK_SHIFT), wxGetKeyState(WXK_CONTROL));
    refresh();
}

void GCodeSketchView::update_cursor(const Vec2d &mouse, bool shift, bool unconstrained)
{
    const GCodeSketch::Drawing &drawing = m_panel.drawing();
    const SketchTool            tool    = m_panel.tool();
    const int                   normal  = m_panel.plane_axis();
    sync_fields();
    m_has_cursor     = false;
    m_snap           = Snap::None;
    m_cursor_outside = false;
    m_inferred_axis  = -1;
    m_hover_point    = -1;
    m_pick_segment    = -1;
    m_fields_at_mouse = false;
    m_alignments.clear();
    if (!m_rendered || (m_drag != Drag::None && m_drag != Drag::Arc)) {
        set_hover_segment(-1);
        m_panel.show_cursor(nullptr, wxEmptyString);
        return;
    }
    if (m_targets_dirty)
        build_targets();

    // Select: the point or the segments under the mouse.
    if (tool == SketchTool::Select) {
        int          hover   = -1;
        const Vec3d *hovered = nullptr;
        m_hover_point        = point_under_mouse(mouse);
        if (m_hover_point >= 0) {
            const std::vector<GCodeSketch::PointRef> points = GCodeSketch::list_points(drawing);
            hovered = GCodeSketch::point_position(drawing, points[size_t(m_hover_point)]);
        } else if (drawing.segments.size() <= MAX_DETAILED)
            hover = segment_under_mouse(mouse);
        set_hover_segment(hover);
        m_panel.show_cursor(hovered, hovered != nullptr ? _L("point") : wxString());
        return;
    }
    set_hover_segment(-1);
    const SketchInput input = m_panel.input();
    // Tangent circles: the line under the mouse, to pick it.
    if (input == SketchInput::Line) {
        Vec3d at;
        set_hover_segment(line_under_mouse(mouse, at), HoverSpan::Alone);
        m_panel.show_cursor(nullptr, wxEmptyString);
        return;
    }
    // An object, the path of an offset, a curve, a corner: the segment under the mouse, with what the tool would do.
    if (input == SketchInput::Object || input == SketchInput::Path || input == SketchInput::Curve || input == SketchInput::Corner) {
        Vec3d     at      = Vec3d::Zero();
        const int segment = drawing.segments.size() <= MAX_DETAILED ? segment_under_mouse(mouse, &at) : -1;
        // The travel moves go with the objects, but are neither offset nor modified: not highlighted then (a click says why).
        const bool shown  = segment >= 0 && size_t(segment) < drawing.segments.size() &&
                           (input == SketchInput::Object || drawing.segments[size_t(segment)].extrude);
        set_hover_segment(shown ? segment : -1, input == SketchInput::Object ? HoverSpan::Shape : input == SketchInput::Path ? HoverSpan::Path : HoverSpan::Alone);
        m_pick_segment = segment;
        m_pick_at      = at;
        // The values typed show by the mouse: the radius of the fillets, the distances of the chamfers.
        if (!m_fields.empty()) {
            Vec3d point = at;
            apply_fields(point);
            m_fields_at_mouse = true;
        }
        m_panel.show_cursor(segment >= 0 ? &m_pick_at : nullptr, wxEmptyString);
        return;
    }
    // Mirror line: a line of the drawing, unless the mouse is on a point of it, the first point of the line then.
    if (input == SketchInput::MirrorLine && (unconstrained || !m_panel.snap_to_points() || target_under_mouse(mouse) < 0)) {
        Vec3d     at;
        const int line = line_under_mouse(mouse, at);
        if (line >= 0) {
            set_hover_segment(line, HoverSpan::Alone);
            m_pick_segment = line;
            m_pick_at      = at;
            m_panel.show_cursor(&m_pick_at, _L("mirror line"));
            return;
        }
    }

    const std::vector<Vec3d> &clicks     = m_panel.clicks();
    // The next points of a flat shape (rectangle, circle, polygon, slot...) stay in the plane of its first one.
    const bool                flat_shape = m_panel.flat_tool() && !clicks.empty();
    // Third click of an arc: a point of the arc.
    const bool                arc_side   = (tool == SketchTool::Arc || tool == SketchTool::SlotArc3Points) && clicks.size() == 2;
    const double              offset     = flat_shape ? clicks.front()[normal] : m_panel.plane_offset();
    const bool                has_anchor = m_panel.has_anchor();
    Vec3d                     anchor     = Vec3d::Zero();
    if (has_anchor)
        anchor = m_panel.anchor();
    // Before a shape is started, the axes of the last point of the drawing place its start.
    const bool has_origin = has_anchor || drawing.has_start;
    Vec3d      origin     = anchor;
    if (!has_anchor && drawing.has_start)
        origin = drawing.last_point();

    Vec3d point     = Vec3d::Zero();
    bool  on_target = false;
    if (!unconstrained && m_panel.snap_to_points()) {
        // On the first point of a spline, which closes it.
        Vec2d first;
        if (tool == SketchTool::Spline && clicks.size() >= 2 && projector()(clicks.front(), first) && (first - mouse).norm() <= PICK_RADIUS) {
            point      = clicks.front();
            m_snap     = Snap::End;
            on_target  = true;
            m_has_base = true;
            m_base     = point;
        }
        // Coincident with an end, a middle, a center of the drawing.
        const int target = on_target ? -1 : target_under_mouse(mouse);
        if (target >= 0) {
            point = m_targets[size_t(target)].position;
            if (flat_shape)
                point[normal] = offset;
            m_snap     = m_targets[size_t(target)].kind;
            on_target  = true;
            m_has_base = true;
            m_base     = point;
        }
    }
    if (!on_target && shift && m_has_base && !flat_shape) {
        // Shift: along the normal of the plane, from the last point of the plane under the mouse.
        double t = 0.;
        if (!mouse_on_line(mouse, m_base, Vec3d::Unit(normal), t)) {
            m_panel.show_cursor(nullptr, wxEmptyString);
            return;
        }
        point         = m_base;
        point[normal] = unconstrained ? m_base[normal] + t : snap_along(m_base[normal] + t, normal);
        m_snap        = Snap::Normal;
    } else if (!on_target) {
        if (!mouse_on_plane(mouse, normal, offset, point)) {
            m_panel.show_cursor(nullptr, wxEmptyString);
            return;
        }
        if (!unconstrained) {
            point = snap_in_plane(point, normal);
            if (m_panel.grid_step() > 0.)
                m_snap = Snap::Grid;
            // X, Y and Z axes through the anchor, or through the last point: horizontal and vertical constraints.
            if (m_panel.snap_to_axes() && has_origin && !arc_side) {
                Vec3d on_axis;
                int   axis = -1;
                if (infer_axis(mouse, origin, offset, flat_shape, on_axis, axis)) {
                    point           = on_axis;
                    m_inferred_axis = axis;
                    m_anchor        = origin;
                    m_snap          = Snap::Axis;
                }
            }
            if (m_panel.constraints()) {
                bool directed = false;
                Snap kind     = Snap::None;
                if (m_inferred_axis < 0 && tool == SketchTool::Line && m_panel.chain_has_segments() &&
                    infer_direction(mouse, anchor, point, kind)) {
                    m_snap   = kind;
                    directed = true;
                }
                if (arc_side && infer_tangent_arc(mouse, point)) {
                    m_snap   = Snap::Tangent;
                    directed = true;
                }
                if (!directed)
                    infer_alignments(mouse, anchor, has_anchor, point);
            }
        }
        m_has_base = true;
        m_base     = point;
    }
    // In the build volume, the values shown being the ones of the point placed; a typed length goes beyond it if it
    // does: the cursor turns red.
    if (!on_target && !has_typed_value())
        point = clamp_to_volume(point);
    apply_fields(point);

    m_cursor       = point;
    m_has_cursor   = true;
    m_cursor_known = true;
    // Same checks as the G-code export.
    GCodeSketch::Drawing single;
    single.has_start = true;
    single.start     = point;
    m_cursor_outside = !GCodeSketch::check(single, m_panel.machine()).empty();
    m_panel.show_cursor(&m_cursor, snap_text());
}

wxString GCodeSketchView::snap_text() const
{
    static const char *axes[] = { "X", "Y", "Z" };
    wxString           text;
    switch (m_snap) {
    case Snap::Grid: text = _L("grid"); break;
    case Snap::End: text = _L("on an end"); break;
    case Snap::Passage: text = _L("on the passage point of an arc"); break;
    case Snap::Middle: text = _L("on a middle"); break;
    case Snap::Center: text = _L("on a center"); break;
    case Snap::Axis:
        if (m_inferred_axis >= 0 && m_inferred_axis < 3)
            text = wxString::Format(_L("on the %s axis of the last point"), axes[m_inferred_axis]);
        break;
    case Snap::Perpendicular: text = _L("perpendicular to the last segment"); break;
    case Snap::Parallel: text = _L("parallel to the last segment"); break;
    case Snap::Tangent: text = _L("tangent to the last segment"); break;
    case Snap::Normal: text = _L("out of the plane"); break;
    default: break;
    }
    if (!m_alignments.empty()) {
        if (!text.empty())
            text += ", ";
        text += _L("lined up with a point");
    }
    return text;
}

std::vector<GCodeSketchView::FieldType> GCodeSketchView::field_types() const
{
    using T              = FieldType;
    const size_t clicks  = m_panel.clicks().size();
    switch (m_panel.tool()) {
    case SketchTool::Line:
        if (m_panel.has_anchor())
            return { T::Length, T::Angle };
        break;
    case SketchTool::Spline:
        if (clicks > 0)
            return { T::Length, T::Angle };
        break;
    case SketchTool::Arc:
        if (clicks == 1)
            return { T::Length, T::Angle };
        if (clicks == 2)
            return { T::Radius };
        break;
    case SketchTool::Rectangle:
    case SketchTool::RectangleCenter:
        if (clicks == 1)
            return { T::SideU, T::SideV };
        break;
    case SketchTool::Rectangle3Points:
        if (clicks == 1)
            return { T::Length, T::Angle };
        if (clicks == 2)
            return { T::Width };
        break;
    case SketchTool::Circle:
        if (clicks == 1)
            return { T::Diameter };
        break;
    case SketchTool::Circle2Points:
        if (clicks == 1)
            return { T::Diameter, T::Angle };
        break;
    case SketchTool::CircleTangent2:
        if (!m_panel.picking())
            return { T::Radius };
        break;
    case SketchTool::ArcCenter:
        if (clicks == 1)
            return { T::Radius, T::Angle };
        if (clicks == 2)
            return { T::Sweep };
        break;
    case SketchTool::ArcTangent:
        if (clicks == 1)
            return { T::Radius, T::Sweep };
        break;
    case SketchTool::PolygonCircumscribed:
    case SketchTool::PolygonInscribed:
        if (clicks == 1)
            return { T::Diameter, T::Sides };
        break;
    case SketchTool::PolygonEdge:
        if (clicks == 1)
            return { T::Length, T::Angle, T::Sides };
        if (clicks == 2)
            return { T::Sides };
        break;
    case SketchTool::Ellipse:
        if (clicks == 1)
            return { T::Length, T::Angle };
        if (clicks == 2)
            return { T::Radius };
        break;
    case SketchTool::SlotCenters:
    case SketchTool::SlotOverall:
    case SketchTool::SlotCenter:
        if (clicks == 1)
            return { T::Length, T::Angle };
        if (clicks == 2)
            return { T::Width };
        break;
    case SketchTool::SlotArc3Points:
        if (clicks == 1)
            return { T::Length, T::Angle };
        if (clicks == 2)
            return { T::Radius };
        if (clicks == 3)
            return { T::Width };
        break;
    case SketchTool::SlotArcCenter:
        if (clicks == 1)
            return { T::Radius, T::Angle };
        if (clicks == 2)
            return { T::Sweep };
        if (clicks == 3)
            return { T::Width };
        break;
    case SketchTool::Conic:
        if (clicks == 1)
            return { T::Length, T::Angle };
        if (clicks == 2)
            return { T::Rho };
        break;
    case SketchTool::Mirror:
        if (m_panel.objects_chosen() && clicks == 1)
            return { T::Angle };
        break;
    case SketchTool::CircularPattern:
        if (m_panel.objects_chosen() && clicks == 1)
            return { T::Instances, T::Sweep };
        break;
    case SketchTool::RectangularPattern:
        if (m_panel.objects_chosen())
            return { T::Instances, T::SpacingU, T::InstancesV, T::SpacingV };
        break;
    case SketchTool::Fillet: return { T::Radius };
    case SketchTool::ChamferEqual: return { T::Distance };
    case SketchTool::ChamferDistanceAngle: return { T::Distance, T::Angle };
    case SketchTool::ChamferTwoDistances: return { T::Distance, T::Distance2 };
    case SketchTool::Offset:
        if (m_panel.objects_chosen())
            return { T::Distance };
        break;
    case SketchTool::Scale:
        if (m_panel.objects_chosen() && clicks == 1)
            return { T::Factor };
        break;
    case SketchTool::Move:
    case SketchTool::Copy:
        if (m_panel.objects_chosen() && clicks == 1)
            return { T::Length, T::Angle };
        break;
    case SketchTool::Rotate:
        if (m_panel.objects_chosen() && clicks == 2)
            return { T::Sweep };
        break;
    default: break;
    }
    return {};
}

void GCodeSketchView::sync_fields()
{
    if (m_fields_step == m_panel.step())
        return;
    m_fields_step  = m_panel.step();
    m_active_field = 0;
    m_fields.clear();
    for (const FieldType type : field_types()) {
        Field field;
        field.type = type;
        m_fields.push_back(field);
    }
}

bool GCodeSketchView::field_value(const Field &field, double &value) const
{
    if (field.text.empty())
        return false;
    wxString text = field.text;
    text.Replace(",", ".");
    return text.ToCDouble(&value);
}

bool GCodeSketchView::has_typed_value() const
{
    double value = 0.;
    for (const Field &field : m_fields)
        if (field_value(field, value))
            return true;
    return false;
}

void GCodeSketchView::apply_fields(Vec3d &point)
{
    if (m_fields.empty())
        return;
    const SketchTool          tool   = m_panel.tool();
    const std::vector<Vec3d> &clicks = m_panel.clicks();
    const size_t              step   = clicks.size();
    const int                 normal = m_panel.plane_axis();
    const Vec3d               u      = Vec3d::Unit(first_axis(normal));
    const Vec3d               v      = Vec3d::Unit(second_axis(normal));
    // Counterclockwise from u toward v around it, as the panel turns its shapes.
    const Vec3d               n      = u.cross(v);
    // Values typed, by type of field.
    double typed[int(FieldType::Count)] = {};
    bool   has[int(FieldType::Count)]   = {};
    for (const Field &field : m_fields) {
        const int k = int(field.type);
        has[k]      = field_value(field, typed[k]);
    }
    auto value_of = [&has, &typed](FieldType type, double &value) {
        if (!has[int(type)])
            return false;
        value = typed[int(type)];
        return true;
    };
    // Values of the cursor, shown where nothing is typed, and their dimensions on the shape.
    auto show = [this](FieldType type, double value, bool unknown, const Dimension &dimension) {
        for (Field &field : m_fields)
            if (field.type == type) {
                field.value     = value;
                field.unknown   = unknown;
                field.dimension = dimension;
            }
    };
    auto dimension = [](DimensionKind kind, const Vec3d &a, const Vec3d &b) {
        Dimension result;
        result.kind = kind;
        result.a    = a;
        result.b    = b;
        return result;
    };
    // At vertex, from the direction of from, turning by angle around axis.
    auto angular = [](const Vec3d &vertex, const Vec3d &from, double angle, const Vec3d &axis) {
        Dimension result;
        result.kind  = DimensionKind::Angular;
        result.a     = vertex;
        result.b     = from;
        result.angle = angle;
        result.axis  = axis;
        return result;
    };
    double value = 0.;

    // Length (times scale) and angle from an anchor, in the plane from u toward v: lines, chords, sides, radii, diameters.
    auto polar = [&](const Vec3d &anchor, FieldType type, double scale, DimensionKind kind) {
        const Vec3d offset    = point - anchor;
        double      length    = offset.norm();
        Vec3d       direction = length > EPSILON ? Vec3d(offset / length) : u;
        bool        in_plane  = std::abs(offset[normal]) < EPSILON;
        if (value_of(FieldType::Angle, value)) {
            const double angle = value * PI / 180.;
            direction          = std::cos(angle) * u + std::sin(angle) * v;
            length             = std::max(0., offset.dot(direction));
            in_plane           = true;
        }
        if (value_of(type, value))
            length = std::abs(value) / scale;
        point = anchor + length * direction;
        show(type, scale * length, false, dimension(kind, anchor, point));
        const double angle = std::atan2(direction.dot(v), direction.dot(u));
        show(FieldType::Angle, angle * 180. / PI, !in_plane, in_plane ? angular(anchor, Vec3d(anchor + u), angle, n) : Dimension());
    };
    // Distance of the cursor across the line through a and b, in the plane: the other side of a rectangle, half the
    // width of a slot (scale 2), half the other axis of an ellipse. Its dimension from from across the line, or both
    // ways.
    auto across_line = [&](const Vec3d &a, const Vec3d &b, FieldType type, double scale, const Vec3d &from, bool both_ways) {
        Vec3d side = b - a;
        side -= side.dot(n) * n;
        if (side.norm() < EPSILON) {
            show(type, 0., true, Dimension());
            return;
        }
        const Vec3d  across   = n.cross(side.normalized());
        const double current  = (point - b).dot(across);
        double       distance = current;
        if (value_of(type, value))
            distance = (current < 0. ? -1. : 1.) * std::abs(value) / scale;
        point += (distance - current) * across;
        show(type, scale * std::abs(distance), false,
             dimension(DimensionKind::Linear, both_ways ? Vec3d(from - distance * across) : from, Vec3d(from + distance * across)));
    };
    // Width of a slot along an arc: twice the distance from the arc.
    auto arc_width = [&](const GCodeSketch::Arc &arc) {
        Vec3d radial = point - arc.center;
        radial -= radial.dot(arc.normal) * arc.normal;
        if (!arc.valid || radial.norm() < EPSILON) {
            show(FieldType::Width, 0., true, Dimension());
            return;
        }
        const Vec3d  dir  = radial.normalized();
        const double sign = radial.norm() >= arc.radius ? 1. : -1.;
        double       half = std::abs(radial.norm() - arc.radius);
        if (value_of(FieldType::Width, value))
            half = 0.5 * std::abs(value);
        point = arc.center + (arc.radius + sign * half) * dir;
        show(FieldType::Width, 2. * half, false,
             dimension(DimensionKind::Linear, Vec3d(arc.center + (arc.radius - half) * dir), Vec3d(arc.center + (arc.radius + half) * dir)));
    };
    // Third point of an arc through 3 points: on it, of the radius typed.
    auto arc_radius = [&]() {
        Vec3d middle;
        if (value_of(FieldType::Radius, value) && GCodeSketch::arc_middle_with_radius(clicks[0], clicks[1], point, value, middle))
            point = middle;
        const GCodeSketch::Arc arc = GCodeSketch::arc_through(clicks[0], point, clicks[1]);
        show(FieldType::Radius, arc.valid ? arc.radius : 0., !arc.valid,
             arc.valid ? dimension(DimensionKind::Radial, arc.center, arc.point_at(0.5)) : Dimension());
    };
    // Center point arcs: the angle swept from their start, following the mouse, or typed.
    auto sweep = [&]() {
        const Vec3d &center = clicks[0];
        const Vec3d &start  = clicks[1];
        if (value_of(FieldType::Sweep, value)) {
            const double angle = value * PI / 180.;
            m_panel.set_sweep(angle);
            // The cursor at that angle, as far from the center as it is.
            Vec3d from = start - center;
            Vec3d to   = point - center;
            from -= from.dot(n) * n;
            to -= to.dot(n) * n;
            if (from.norm() > EPSILON) {
                const double radius = to.norm() > EPSILON ? to.norm() : from.norm();
                const Vec3d  dir    = from.normalized();
                point = center + radius * (std::cos(angle) * dir + std::sin(angle) * n.cross(dir));
            }
        } else
            m_panel.track_sweep(point);
        const double angle = m_panel.sweep();
        show(FieldType::Sweep, angle * 180. / PI, false, angular(center, start, angle, n));
    };
    // Polygons: the number of sides typed, kept for the next ones.
    auto sides = [&](const Vec3d &at) {
        if (value_of(FieldType::Sides, value) && std::lround(value) >= 3)
            m_panel.set_polygon_sides(int(std::lround(value)));
        show(FieldType::Sides, double(m_panel.polygon_sides()), false, dimension(DimensionKind::Label, at, at));
    };

    switch (tool) {
    case SketchTool::Line:
        if (m_panel.has_anchor())
            polar(m_panel.anchor(), FieldType::Length, 1., DimensionKind::Linear);
        break;
    case SketchTool::Spline:
        if (step > 0)
            polar(clicks.back(), FieldType::Length, 1., DimensionKind::Linear);
        break;
    case SketchTool::Arc:
    case SketchTool::SlotArc3Points:
        if (step == 1)
            polar(clicks[0], FieldType::Length, 1., DimensionKind::Linear);
        else if (step == 2)
            arc_radius();
        else if (step == 3)
            arc_width(GCodeSketch::arc_through(clicks[0], clicks[2], clicks[1]));
        break;
    case SketchTool::Rectangle:
    case SketchTool::RectangleCenter:
        if (step == 1) {
            const Vec3d &first = clicks[0];
            // The sides typed are whole sides; from the center, the corner is half of them away.
            const double scale = tool == SketchTool::RectangleCenter ? 2. : 1.;
            double       du    = (point - first).dot(u);
            double       dv    = (point - first).dot(v);
            if (value_of(FieldType::SideU, value))
                du = (du < 0. ? -1. : 1.) * std::abs(value) / scale;
            if (value_of(FieldType::SideV, value))
                dv = (dv < 0. ? -1. : 1.) * std::abs(value) / scale;
            point = first + du * u + dv * v;
            // Its sides, one under it and one beside it.
            const Vec3d corner  = tool == SketchTool::RectangleCenter ? Vec3d(first - du * u - dv * v) : first;
            const Vec3d along_u = corner + scale * du * u;
            show(FieldType::SideU, scale * std::abs(du), false, dimension(DimensionKind::Linear, corner, along_u));
            show(FieldType::SideV, scale * std::abs(dv), false, dimension(DimensionKind::Linear, along_u, Vec3d(along_u + scale * dv * v)));
        }
        break;
    case SketchTool::Rectangle3Points:
        if (step == 1)
            polar(clicks[0], FieldType::Length, 1., DimensionKind::Linear);
        else if (step == 2)
            across_line(clicks[0], clicks[1], FieldType::Width, 1., clicks[1], false);
        break;
    case SketchTool::Circle:
        if (step == 1) {
            const Vec3d &center    = clicks[0];
            Vec3d        radial    = point - center;
            radial[normal]         = 0.;
            double       radius    = radial.norm();
            const Vec3d  direction = radius > EPSILON ? Vec3d(radial / radius) : u;
            if (value_of(FieldType::Diameter, value))
                radius = 0.5 * std::abs(value);
            point = center + radius * direction;
            show(FieldType::Diameter, 2. * radius, false, dimension(DimensionKind::Diametral, center, point));
        }
        break;
    case SketchTool::Circle2Points:
        if (step == 1)
            polar(clicks[0], FieldType::Diameter, 1., DimensionKind::Linear);
        break;
    case SketchTool::CircleTangent2: {
        // Its center on the bisector of the lines picked: the one of the radius typed.
        const std::vector<std::pair<Vec3d, Vec3d>> lines = m_panel.picked_lines();
        if (lines.size() < 2)
            break;
        const bool fixed  = value_of(FieldType::Radius, value) && std::abs(value) > EPSILON;
        double     radius = 0.;
        Vec3d      center;
        if (GCodeSketch::circle_tangent_2(lines[0].first, lines[0].second, lines[1].first, lines[1].second, point, fixed ? std::abs(value) : 0.,
                                          n, center, radius)) {
            if (fixed)
                point = center;
            show(FieldType::Radius, radius, false, dimension(DimensionKind::Radial, center, Vec3d(center + radius * u)));
        } else
            show(FieldType::Radius, 0., true, Dimension());
        break;
    }
    case SketchTool::ArcCenter:
    case SketchTool::SlotArcCenter:
        if (step == 1)
            polar(clicks[0], FieldType::Radius, 1., DimensionKind::Radial);
        else if (step == 2)
            sweep();
        else if (step == 3)
            arc_width(GCodeSketch::arc_around(clicks[0], clicks[1], m_panel.sweep(), n));
        break;
    case SketchTool::ArcTangent:
        if (step == 1) {
            const Vec3d     &start = clicks[0];
            Vec3d            middle;
            GCodeSketch::Arc arc;
            if (GCodeSketch::tangent_arc_middle(start, m_panel.arc_tangent(), point, middle))
                arc = GCodeSketch::arc_through(start, middle, point);
            double     radius     = 0.;
            double     swept      = 0.;
            const bool has_radius = value_of(FieldType::Radius, radius) && std::abs(radius) > EPSILON;
            const bool has_sweep  = value_of(FieldType::Sweep, swept) && std::abs(swept) > 1e-3;
            if (arc.valid && (has_radius || has_sweep)) {
                // On the same side of the tangent: the circle of the radius typed, its end where the cursor is or at the
                // angle typed.
                const double r      = has_radius ? std::abs(radius) : arc.radius;
                const Vec3d  center = start + r * (arc.center - start).normalized();
                double       angle  = arc.angle;
                if (has_sweep)
                    // At most 355 degrees: closer to a full turn, its end is too close to its start to find the arc
                    // again from the tangent (tangent_arc_middle()).
                    angle = std::clamp(std::abs(swept) * PI / 180., 1e-3, 355. * PI / 180.);
                else {
                    const Vec3d from = start - center;
                    Vec3d       to   = point - center;
                    to -= to.dot(arc.normal) * arc.normal;
                    if (to.norm() > EPSILON) {
                        angle = std::atan2(from.cross(to).dot(arc.normal), from.dot(to));
                        if (angle <= 0.)
                            angle += 2. * PI;
                    }
                }
                arc = GCodeSketch::arc_around(center, start, angle, arc.normal);
                if (arc.valid)
                    point = arc.end;
            }
            if (arc.valid) {
                show(FieldType::Radius, arc.radius, false, dimension(DimensionKind::Radial, arc.center, arc.point_at(0.5)));
                show(FieldType::Sweep, arc.angle * 180. / PI, false, angular(arc.center, arc.start, arc.angle, arc.normal));
            } else {
                show(FieldType::Radius, 0., true, Dimension());
                show(FieldType::Sweep, 0., true, Dimension());
            }
        }
        break;
    case SketchTool::PolygonCircumscribed:
    case SketchTool::PolygonInscribed:
        if (step == 1) {
            polar(clicks[0], FieldType::Diameter, 2., DimensionKind::Diametral);
            sides(clicks[0]);
        }
        break;
    case SketchTool::PolygonEdge:
        if (step == 1) {
            polar(clicks[0], FieldType::Length, 1., DimensionKind::Linear);
            sides(Vec3d(0.5 * (clicks[0] + point)));
        } else if (step == 2)
            sides(point);
        break;
    case SketchTool::Ellipse:
        if (step == 1)
            polar(clicks[0], FieldType::Length, 1., DimensionKind::Linear);
        else if (step == 2)
            across_line(clicks[0], clicks[1], FieldType::Radius, 1., clicks[0], false);
        break;
    case SketchTool::SlotCenters:
    case SketchTool::SlotOverall:
        if (step == 1)
            polar(clicks[0], FieldType::Length, 1., DimensionKind::Linear);
        else if (step == 2)
            across_line(clicks[0], clicks[1], FieldType::Width, 2., Vec3d(0.5 * (clicks[0] + clicks[1])), true);
        break;
    case SketchTool::SlotCenter:
        if (step == 1)
            polar(clicks[0], FieldType::Length, 1., DimensionKind::Linear);
        else if (step == 2)
            across_line(Vec3d(2. * clicks[0] - clicks[1]), clicks[1], FieldType::Width, 2., clicks[0], true);
        break;
    case SketchTool::Conic:
        if (step == 1)
            polar(clicks[0], FieldType::Length, 1., DimensionKind::Linear);
        else if (step == 2) {
            if (value_of(FieldType::Rho, value) && value > 0. && value < 1.)
                m_panel.set_conic_rho(value);
            show(FieldType::Rho, m_panel.conic_rho(), false, dimension(DimensionKind::Label, point, point));
        }
        break;
    case SketchTool::Mirror:
    case SketchTool::Move:
    case SketchTool::Copy:
        // The direction of the mirror line; how far and which way the objects go.
        if (step == 1) {
            const double distance = (point - clicks[0]).norm();
            polar(clicks[0], FieldType::Length, 1., DimensionKind::Linear);
            // The angle of the mirror line typed with the cursor behind its first point: the line all the same.
            if (tool == SketchTool::Mirror && (point - clicks[0]).norm() < EPSILON && value_of(FieldType::Angle, value)) {
                const double angle = value * PI / 180.;
                point              = clicks[0] + std::max(distance, 10.) * (std::cos(angle) * u + std::sin(angle) * v);
            }
        }
        break;
    case SketchTool::CircularPattern:
        if (step == 1) {
            const Vec3d &center = clicks[0];
            if (value_of(FieldType::Instances, value) && std::lround(value) >= 2)
                m_panel.set_pattern_count(int(std::lround(value)));
            if (value_of(FieldType::Sweep, value))
                m_panel.set_pattern_angle(value);
            show(FieldType::Instances, double(m_panel.pattern_count()), false, dimension(DimensionKind::Label, center, center));
            show(FieldType::Sweep, m_panel.pattern_angle(), false, angular(center, Vec3d(center + u), m_panel.pattern_angle() * PI / 180., n));
        }
        break;
    case SketchTool::RectangularPattern:
        if (m_panel.objects_chosen()) {
            // The first copy where the cursor is, from the middle of the objects.
            const Vec3d center = m_panel.objects_center();
            double      du     = (point - center).dot(u);
            double      dv     = (point - center).dot(v);
            if (value_of(FieldType::SpacingU, value))
                du = (du < 0. ? -1. : 1.) * std::abs(value);
            if (value_of(FieldType::SpacingV, value))
                dv = (dv < 0. ? -1. : 1.) * std::abs(value);
            if (value_of(FieldType::Instances, value) && std::lround(value) >= 1)
                m_panel.set_grid_count(0, int(std::lround(value)));
            if (value_of(FieldType::InstancesV, value) && std::lround(value) >= 1)
                m_panel.set_grid_count(1, int(std::lround(value)));
            m_panel.set_grid_spacing(du, dv);
            const Vec3d along_u = center + du * u;
            point               = along_u + dv * v;
            show(FieldType::SpacingU, std::abs(du), false, dimension(DimensionKind::Linear, center, along_u));
            show(FieldType::SpacingV, std::abs(dv), false, dimension(DimensionKind::Linear, along_u, point));
            show(FieldType::Instances, double(m_panel.grid_count(0)), false, Dimension());
            show(FieldType::InstancesV, double(m_panel.grid_count(1)), false, Dimension());
        }
        break;
    case SketchTool::Fillet:
        if (value_of(FieldType::Radius, value) && std::abs(value) > EPSILON)
            m_panel.set_fillet_radius(std::abs(value));
        show(FieldType::Radius, m_panel.fillet_radius(), false, Dimension());
        break;
    case SketchTool::ChamferEqual:
    case SketchTool::ChamferDistanceAngle:
    case SketchTool::ChamferTwoDistances:
        if (value_of(FieldType::Distance, value) && std::abs(value) > EPSILON)
            m_panel.set_chamfer_distance(0, std::abs(value));
        if (value_of(FieldType::Distance2, value) && std::abs(value) > EPSILON)
            m_panel.set_chamfer_distance(1, std::abs(value));
        if (value_of(FieldType::Angle, value))
            m_panel.set_chamfer_angle(value);
        show(FieldType::Distance, m_panel.chamfer_distance(0), false, Dimension());
        show(FieldType::Distance2, m_panel.chamfer_distance(1), false, Dimension());
        show(FieldType::Angle, m_panel.chamfer_angle(), false, Dimension());
        break;
    case SketchTool::Offset:
        if (m_panel.objects_chosen()) {
            // The side of the cursor; its distance to the path, or the one typed.
            double distance = 0.;
            if (m_panel.offset_distance_at(point, distance)) {
                const double side   = distance < 0. ? -1. : 1.;
                double       length = std::round(std::abs(distance) * 100.) / 100.;
                if (value_of(FieldType::Distance, value) && std::abs(value) > EPSILON)
                    length = std::abs(value);
                m_panel.set_offset_distance(side * length);
                show(FieldType::Distance, length, false, Dimension());
            } else
                show(FieldType::Distance, 0., true, Dimension());
        }
        break;
    case SketchTool::Scale:
        if (step == 1) {
            // The factor: how far the cursor is from the base point, against the farthest point of the objects.
            const Vec3d &base   = clicks[0];
            const double reach  = m_panel.objects_reach(base);
            Vec3d        radial = point - base;
            radial -= radial.dot(n) * n;
            double factor = reach > EPSILON ? radial.norm() / reach : 1.;
            if (value_of(FieldType::Factor, value) && std::abs(value) > 1e-3) {
                factor = std::abs(value);
                if (radial.norm() > EPSILON && reach > EPSILON)
                    point = base + (factor * reach / radial.norm()) * radial;
            }
            if (factor > 1e-3)
                m_panel.set_scale_factor(factor);
            show(FieldType::Factor, m_panel.scale_factor(), false, dimension(DimensionKind::Linear, base, point));
        }
        break;
    case SketchTool::Rotate:
        if (step == 2)
            sweep();
        break;
    default: break;
    }
}

void GCodeSketchView::type_char(wxChar c)
{
    if (m_fields.empty())
        return;
    m_active_field = std::clamp(m_active_field, 0, int(m_fields.size()) - 1);
    Field &field   = m_fields[size_t(m_active_field)];
    // Typing in a locked field replaces its value.
    if (field.locked) {
        field.text.clear();
        field.locked = false;
    }
    if ((c == '-' && !field.text.empty()) || (c == '.' && field.text.Find('.') != wxNOT_FOUND) || field.text.length() >= MAX_TYPED)
        return;
    field.text += c;
    update_cursor_here();
    refresh();
}

void GCodeSketchView::erase_char()
{
    if (m_fields.empty())
        return;
    m_active_field = std::clamp(m_active_field, 0, int(m_fields.size()) - 1);
    Field &field   = m_fields[size_t(m_active_field)];
    if (!field.text.empty())
        field.text.RemoveLast();
    if (field.text.empty())
        field.locked = false;
    update_cursor_here();
    refresh();
}

void GCodeSketchView::next_field(int direction)
{
    if (m_fields.empty())
        return;
    const int count = int(m_fields.size());
    m_active_field  = std::clamp(m_active_field, 0, count - 1);
    // Tab keeps the value typed: it no longer follows the mouse.
    Field &field = m_fields[size_t(m_active_field)];
    if (!field.text.empty())
        field.locked = true;
    m_active_field = (m_active_field + direction + count) % count;
    update_cursor_here();
    refresh();
}

void GCodeSketchView::on_mouse(wxMouseEvent &evt)
{
    const Vec2d position = mouse_position(evt);

    if (evt.Entering()) {
        evt.Skip();
        return;
    }
    if (evt.Leaving()) {
        if (m_drag == Drag::None)
            hide_cursor();
        evt.Skip();
        return;
    }
    if (evt.Moving() || evt.Dragging()) {
        m_has_mouse = true;
        m_mouse     = position;
    }
    const SketchTool tool = m_panel.tool();
    const bool       unconstrained = evt.ControlDown() || evt.CmdDown();

    if (evt.ButtonDown() || evt.ButtonDClick()) {
        // Another button while one is held: ignored, the drag in progress (a move, the plane...) goes on to its end.
        if (m_down_button != 0) {
            const bool held = (m_down_button == wxMOUSE_BTN_LEFT && evt.LeftIsDown()) ||
                              (m_down_button == wxMOUSE_BTN_MIDDLE && evt.MiddleIsDown()) ||
                              (m_down_button == wxMOUSE_BTN_RIGHT && evt.RightIsDown());
            if (held)
                return;
            // Its release was missed: the move it made ends, this press starts afresh.
            if (m_drag == Drag::MovePoint || m_drag == Drag::MoveSegments)
                m_panel.end_move();
            m_drag = Drag::None;
        }
        // The keys go to the view (Delete, Escape, Ctrl+Z, the typed values...).
        m_canvas->SetFocus();
        if (!m_canvas->HasCapture())
            m_canvas->CaptureMouse();
        m_down_button     = evt.GetButton();
        m_down_position   = position;
        m_last_position   = position;
        m_moved           = false;
        m_double_click    = evt.ButtonDClick();
        m_pressed_point   = -1;
        m_pressed_segment = -1;
        m_arc_armed       = false;
        m_plane_armed     = false;
        // Alt with a click isn't Alt alone.
        m_alt_alone       = false;
        if (m_down_button == wxMOUSE_BTN_LEFT) {
            if (tool == SketchTool::Select) {
                // The Select tool moves the point or the segments it presses.
                m_pressed_point = point_under_mouse(position);
                if (m_pressed_point < 0)
                    m_pressed_segment = segment_under_mouse(position);
            } else if (tool == SketchTool::Line && m_panel.chain_has_segments()) {
                // Pressed on the end of the line: dragging draws an arc tangent to it, as in Fusion 360.
                Vec2d end;
                m_arc_armed = projector()(m_panel.anchor(), end) && (end - position).norm() <= PICK_RADIUS;
            }
        } else if (m_down_button == wxMOUSE_BTN_MIDDLE) {
            // Pressed on the grid of the working plane: dragging moves the plane, even while drawing. Elsewhere it pans.
            m_plane_armed       = on_plane_grid(position, m_plane_grab);
            m_plane_grab_offset = m_panel.plane_offset();
        }
        return;
    }

    if (evt.Dragging() && m_down_button != 0) {
        if (!m_moved) {
            if ((position - m_down_position).norm() < DRAG_THRESHOLD)
                return;
            m_moved = true;
            m_drag  = m_down_button == wxMOUSE_BTN_LEFT ? Drag::Rotate : Drag::Pan;
            if (m_down_button == wxMOUSE_BTN_MIDDLE && m_plane_armed)
                m_drag = Drag::Plane;
            if (m_down_button == wxMOUSE_BTN_LEFT) {
                if (m_pressed_point >= 0) {
                    const std::vector<GCodeSketch::PointRef> points = GCodeSketch::list_points(m_panel.drawing());
                    if (size_t(m_pressed_point) < points.size()) {
                        m_moved_point = points[size_t(m_pressed_point)];
                        m_drag        = Drag::MovePoint;
                        m_panel.begin_move(m_moved_point);
                    }
                } else if (m_pressed_segment >= 0 && size_t(m_pressed_segment) < m_panel.drawing().segments.size()) {
                    // The selected segments when pressed, otherwise the ones a click selects there (the segment is
                    // checked again, as the point above: the drawing must not have changed since the press).
                    const SketchSelection &selection = m_panel.selection();
                    size_t                 first     = 0;
                    size_t                 last      = 0;
                    if (selection.kind == SketchSelection::Kind::Segments && size_t(m_pressed_segment) >= selection.first &&
                        size_t(m_pressed_segment) <= selection.last) {
                        first = selection.first;
                        last  = selection.last;
                    } else {
                        m_panel.run_at(size_t(m_pressed_segment), first, last);
                        m_panel.select_segments(first, last);
                    }
                    // Grabbed in the plane parallel to the working plane through the start of the segments.
                    const int    normal = m_panel.plane_axis();
                    const double level  = m_panel.drawing().segment_start(first)[normal];
                    if (mouse_on_plane(m_down_position, normal, level, m_grab)) {
                        m_drag = Drag::MoveSegments;
                        m_panel.begin_move_segments(first, last);
                    }
                } else if (m_arc_armed)
                    m_drag = Drag::Arc;
            }
            if (m_drag != Drag::Arc) {
                m_has_cursor = false;
                set_hover_segment(-1);
                m_panel.show_cursor(nullptr, wxEmptyString);
            }
        }
        switch (m_drag) {
        case Drag::Rotate: {
            const Vec2d delta  = (position - m_last_position) * (PI * TRACKBALL / 180.);
            const Vec3d center = Vec3d(0.5 * (m_bed_min.x() + m_bed_max.x()), 0.5 * (m_bed_min.y() + m_bed_max.y()), 0.);
            m_camera.recover_from_free_camera();
            m_camera.rotate_on_sphere_with_target(delta.x(), delta.y(), true, center);
            break;
        }
        case Drag::Pan: {
            const Vec2d  delta = position - m_last_position;
            const double mm    = pixels_to_mm(1.);
            m_camera.set_target(m_camera.get_target() - delta.x() * mm * m_camera.get_dir_right() + delta.y() * mm * m_camera.get_dir_up());
            break;
        }
        case Drag::MovePoint: {
            const Vec3d *current = GCodeSketch::point_position(m_panel.drawing(), m_moved_point);
            if (current == nullptr)
                break;
            const Vec3d from   = *current;
            const int   normal = m_panel.plane_axis();
            if (evt.ShiftDown()) {
                // Along the normal of the working plane.
                double t = 0.;
                if (mouse_on_line(position, from, Vec3d::Unit(normal), t)) {
                    Vec3d p   = from;
                    p[normal] = snap_along(from[normal] + t, normal);
                    m_panel.move_point(m_moved_point, clamp_to_volume(p));
                }
            } else {
                // In the plane parallel to the working plane through the point.
                Vec3d hit;
                if (mouse_on_plane(position, normal, from[normal], hit)) {
                    Vec3d p   = clamp_to_volume(unconstrained ? hit : snap_in_plane(hit, normal));
                    p[normal] = from[normal];
                    m_panel.move_point(m_moved_point, p);
                }
            }
            break;
        }
        case Drag::MoveSegments: {
            const int normal = m_panel.plane_axis();
            Vec3d     offset = Vec3d::Zero();
            bool      valid  = false;
            if (evt.ShiftDown()) {
                // Along the normal of the working plane, by layers.
                double t = 0.;
                if (mouse_on_line(position, m_grab, Vec3d::Unit(normal), t)) {
                    offset[normal] = unconstrained ? t : snap_along(t, normal);
                    valid          = true;
                }
            } else {
                Vec3d hit;
                if (mouse_on_plane(position, normal, m_grab[normal], hit)) {
                    offset         = hit - m_grab;
                    offset[normal] = 0.;
                    // By steps of the grid.
                    if (!unconstrained)
                        for (int k : { first_axis(normal), second_axis(normal) })
                            offset[k] = snap_along(offset[k], k);
                    valid = true;
                }
            }
            if (valid)
                m_panel.move_segments(offset);
            break;
        }
        case Drag::Arc: update_cursor(position, evt.ShiftDown(), unconstrained); break;
        case Drag::Plane: {
            // The working plane along its normal, from where it was grabbed; by steps of the plane unless Ctrl.
            const Vec3d     normal  = Vec3d::Unit(m_panel.plane_axis());
            const Projector project = projector();
            Vec2d           a, b;
            double          t      = 0.;
            double          offset = m_plane_grab_offset;
            // Its normal seen end on (the plane seen from the front): the mouse moves it up and down the screen.
            const bool seen = project(m_plane_grab, a) && project(Vec3d(m_plane_grab + pixels_to_mm(100.) * normal), b) && (b - a).norm() > 25.;
            if (seen && mouse_on_line(position, m_plane_grab, normal, t))
                offset += t;
            else
                offset += (m_down_position.y() - position.y()) * pixels_to_mm(1.);
            const double step = m_panel.plane_step();
            if (!unconstrained && step > 0.)
                offset = std::round(offset / step) * step;
            m_panel.set_plane_offset(offset);
            break;
        }
        default: break;
        }
        m_last_position = position;
        refresh();
        return;
    }

    if (evt.ButtonUp()) {
        if (m_canvas->HasCapture())
            m_canvas->ReleaseMouse();
        const int  button       = m_down_button;
        const bool click        = button != 0 && !m_moved && evt.GetButton() == button;
        const bool double_click = m_double_click;
        const Drag drag         = m_drag;
        m_drag            = Drag::None;
        m_moved           = false;
        m_down_button     = 0;
        m_double_click    = false;
        m_pressed_point   = -1;
        m_pressed_segment = -1;
        m_arc_armed       = false;
        m_plane_armed     = false;
        if (drag == Drag::MovePoint || drag == Drag::MoveSegments)
            m_panel.end_move();
        else if (drag == Drag::Arc) {
            update_cursor(position, evt.ShiftDown(), unconstrained);
            if (m_has_cursor)
                m_panel.add_tangent_arc(m_cursor);
        }
        if (click && button == wxMOUSE_BTN_LEFT) {
            update_cursor(position, evt.ShiftDown(), unconstrained);
            if (tool == SketchTool::Select) {
                const int point   = point_under_mouse(position);
                const int segment = point < 0 ? segment_under_mouse(position) : -1;
                if (point >= 0)
                    m_panel.select_point(GCodeSketch::list_points(m_panel.drawing())[size_t(point)]);
                else if (segment >= 0)
                    // Click: the segment, or its rectangle or circle. Double click: its whole path.
                    m_panel.select_at(size_t(segment), double_click);
                else
                    m_panel.clear_selection();
            } else if (m_panel.input() == SketchInput::Line) {
                // Tangent circles: the line clicked, and where.
                Vec3d     at;
                const int line = line_under_mouse(position, at);
                if (line >= 0)
                    m_panel.pick(size_t(line), at);
            } else if (m_panel.input() == SketchInput::Object) {
                // An object, its whole path with a double click whose first click chose an object too (not the click which
                // applied the tool and came back to its objects).
                if (m_pick_segment >= 0 && (!double_click || m_object_click_step == m_panel.step())) {
                    m_panel.pick(size_t(m_pick_segment), m_pick_at, double_click);
                    m_object_click_step = m_panel.step();
                }
            } else if (m_panel.input() != SketchInput::Point && m_pick_segment >= 0) {
                // The path of an offset, a curve, a corner, the mirror line: once, not again by the second click of a double
                // click.
                if (!double_click)
                    m_panel.pick(size_t(m_pick_segment), m_pick_at);
            } else if (double_click) {
                // A double click ends the line or the spline; its first click placed its last point.
                if (tool == SketchTool::Line || tool == SketchTool::Spline)
                    m_panel.end_chain();
            } else if (m_has_cursor)
                m_panel.click(m_cursor);
        } else if (click && button == wxMOUSE_BTN_RIGHT)
            show_menu(evt.GetPosition());
        update_cursor(position, evt.ShiftDown(), unconstrained);
        refresh();
        return;
    }

    if (evt.Moving()) {
        update_cursor(position, evt.ShiftDown(), unconstrained);
        refresh();
    }
}

void GCodeSketchView::show_menu(const wxPoint &position)
{
    wxMenu            menu;
    GCodeSketchPanel &panel = m_panel;
    // An item and what it does, bound on the menu holding it: its events go there first.
    auto add = [](wxMenu &to, const wxString &text, std::function<void()> action) {
        wxMenuItem *item = to.Append(wxID_ANY, text);
        to.Bind(wxEVT_MENU, [action](wxCommandEvent &) { action(); }, item->GetId());
    };
    auto tool = [&add, &panel](wxMenu &to, SketchTool sketch_tool) {
        add(to, sketch_tool_name(sketch_tool), [&panel, sketch_tool]() { panel.set_tool(sketch_tool); });
    };
    if (panel.drawing_in_progress()) {
        add(menu, _L("OK (Enter)"), [&panel]() { panel.finish(); });
        add(menu, _L("Cancel (Esc)"), [&panel]() { panel.escape(); });
        if ((panel.tool() == SketchTool::Line && panel.chain_active()) || panel.tool() == SketchTool::Spline)
            add(menu, _L("End the line (double click)"), [&panel]() { panel.end_chain(); });
        menu.AppendSeparator();
    } else if (panel.selection().kind != SketchSelection::Kind::None) {
        add(menu, _L("Delete (Del)"), [&panel]() { panel.delete_selection(); });
        menu.AppendSeparator();
    }
    tool(menu, SketchTool::Select);
    // The tools, as the menu Create of Fusion 360, those of a kind in a submenu.
    auto *create = new wxMenu();
    auto  group  = [&tool, create](const wxString &name, std::initializer_list<SketchTool> tools) {
        auto *submenu = new wxMenu();
        for (const SketchTool sketch_tool : tools)
            tool(*submenu, sketch_tool);
        create->AppendSubMenu(submenu, name);
    };
    tool(*create, SketchTool::Line);
    group(_L("Rectangle"), { SketchTool::Rectangle, SketchTool::Rectangle3Points, SketchTool::RectangleCenter });
    group(_L("Circle"), { SketchTool::Circle, SketchTool::Circle2Points, SketchTool::Circle3Points, SketchTool::CircleTangent2,
                          SketchTool::CircleTangent3 });
    group(_L("Arc"), { SketchTool::Arc, SketchTool::ArcCenter, SketchTool::ArcTangent });
    group(_L("Polygon"), { SketchTool::PolygonCircumscribed, SketchTool::PolygonInscribed, SketchTool::PolygonEdge });
    tool(*create, SketchTool::Ellipse);
    group(_L("Slot"), { SketchTool::SlotCenters, SketchTool::SlotOverall, SketchTool::SlotCenter, SketchTool::SlotArc3Points,
                        SketchTool::SlotArcCenter });
    tool(*create, SketchTool::Spline);
    tool(*create, SketchTool::Conic);
    tool(*create, SketchTool::Point);
    tool(*create, SketchTool::Text);
    create->AppendSeparator();
    tool(*create, SketchTool::Mirror);
    tool(*create, SketchTool::CircularPattern);
    tool(*create, SketchTool::RectangularPattern);
    menu.AppendSubMenu(create, _L("Create"));
    auto *modify  = new wxMenu();
    auto *chamfer = new wxMenu();
    auto *move    = new wxMenu();
    tool(*modify, SketchTool::Fillet);
    for (const SketchTool sketch_tool : { SketchTool::ChamferEqual, SketchTool::ChamferDistanceAngle, SketchTool::ChamferTwoDistances })
        tool(*chamfer, sketch_tool);
    modify->AppendSubMenu(chamfer, _L("Chamfer"));
    tool(*modify, SketchTool::Offset);
    tool(*modify, SketchTool::Trim);
    tool(*modify, SketchTool::Extend);
    tool(*modify, SketchTool::Break);
    tool(*modify, SketchTool::Scale);
    for (const SketchTool sketch_tool : { SketchTool::Move, SketchTool::Copy, SketchTool::Rotate })
        tool(*move, sketch_tool);
    modify->AppendSubMenu(move, _L("Move/Copy"));
    menu.AppendSubMenu(modify, _L("Modify"));
    menu.AppendSeparator();
    // The Extrusion button: the selection, otherwise the next segments.
    if (panel.selection().kind == SketchSelection::Kind::Segments)
        add(menu, panel.extrude_shown() ? _L("Selection: travel moves (E)") : _L("Selection: extrusion (E)"), [&panel]() { panel.toggle_extrude(); });
    else
        add(menu, panel.extrude() ? _L("Draw travel moves (E)") : _L("Draw extrusions (E)"), [&panel]() { panel.toggle_extrude(); });
    add(menu, _L("Next working plane (P, Alt)"), [&panel]() { panel.next_plane(); });
    add(menu, _L("Frame the bed (Z)"), [this]() { zoom_to_bed(); });
    m_canvas->PopupMenu(&menu, position);
    update_cursor_here();
}

void GCodeSketchView::on_wheel(wxMouseEvent &evt)
{
    // Alt with the wheel isn't Alt alone.
    m_alt_alone = false;
    if (evt.GetWheelRotation() == 0 || evt.GetWheelDelta() == 0)
        return;
    const double steps = double(evt.GetWheelRotation()) / double(evt.GetWheelDelta());
    const Vec2d  mouse = mouse_position(evt);
    if (evt.ControlDown() || evt.CmdDown()) {
        // Ctrl + wheel: working plane forward or backward along its normal.
        m_panel.step_plane(steps > 0. ? 1 : -1);
        update_cursor(mouse, evt.ShiftDown(), true);
        refresh();
        return;
    }
    if (!m_rendered)
        return;
    // Zoom toward the point of the working plane under the mouse.
    const int  axis = m_panel.plane_axis();
    Vec3d      before;
    Vec3d      after;
    const bool anchored = mouse_on_plane(mouse, axis, m_panel.plane_offset(), before);
    m_camera.update_zoom(steps);
    m_camera.apply_projection(m_projection_box);
    if (anchored && mouse_on_plane(mouse, axis, m_panel.plane_offset(), after))
        m_camera.set_target(m_camera.get_target() + before - after);
    update_cursor(mouse, evt.ShiftDown(), false);
    refresh();
}

void GCodeSketchView::on_key_down(wxKeyEvent &evt)
{
    const int  key  = evt.GetKeyCode();
    const bool ctrl = evt.ControlDown() || evt.CmdDown();
    m_digit_key     = -1;
    // Alt alone (AltGr is Ctrl + Alt, Shift + Alt changes the keyboard layout): the next working plane when it is
    // released, even while drawing.
    if (key == WXK_ALT) {
        m_alt_alone = !ctrl && !evt.ShiftDown();
        return;
    }
    m_alt_alone = false;
    if (key == WXK_SHIFT || key == WXK_CONTROL) {
        // Shift: the cursor leaves the plane; Ctrl: it is free. At once, without moving the mouse.
        update_cursor_here();
        return;
    }
    sync_fields();
    // While drawing, the numbers go into the fields by the cursor, as in Fusion 360.
    if (!m_fields.empty() && !ctrl && !evt.AltDown()) {
        // A key of the top row: what it types comes with wxEVT_CHAR (on_char()), so that the minus of a French
        // keyboard, on the key of the 6, types a minus.
        if (key >= '0' && key <= '9') {
            m_digit_key = key - '0';
            evt.Skip();
            return;
        }
        if (key >= WXK_NUMPAD0 && key <= WXK_NUMPAD9) {
            type_char(wxChar('0' + (key - WXK_NUMPAD0)));
            return;
        }
        switch (key) {
        case '.':
        case ',':
        case WXK_DECIMAL:
        case WXK_NUMPAD_DECIMAL: type_char('.'); return;
        case '-':
        case WXK_SUBTRACT:
        case WXK_NUMPAD_SUBTRACT: type_char('-'); return;
        case WXK_BACK: erase_char(); return;
        case WXK_TAB: next_field(evt.ShiftDown() ? -1 : 1); return;
        default: break;
        }
    }

    // While a mouse button is held, only the drag changes the drawing: Delete, Ctrl + Z and Ctrl + Y wait for its end.
    const bool held = m_down_button != 0;
    switch (key) {
    case WXK_ESCAPE:
        if (m_drag == Drag::MovePoint || m_drag == Drag::MoveSegments) {
            // Back to where the move started; the rest of the drag does nothing.
            m_panel.cancel_move();
            m_drag = Drag::None;
        } else
            m_panel.escape();
        break;
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER:
        if (has_typed_value() && m_panel.input() != SketchInput::Object) {
            // The point of the typed values; a modify tool picking under the mouse keeps them for its next clicks.
            update_cursor_here();
            if (m_has_cursor)
                m_panel.click(m_cursor);
            else if (m_cursor_known && m_panel.input() == SketchInput::Point) {
                // The mouse off the working plane: the values typed from the last cursor.
                Vec3d point = m_cursor;
                apply_fields(point);
                m_panel.click(point);
            }
        } else
            // Nothing typed: done, as OK; the objects chosen: the next step of the tool.
            m_panel.finish();
        break;
    case WXK_DELETE:
    case WXK_BACK:
        if (!held)
            m_panel.delete_selection();
        break;
    case WXK_TAB:
    case 'P':
        if (ctrl) {
            evt.Skip();
            return;
        }
        m_panel.next_plane();
        break;
    case WXK_PAGEUP: m_panel.step_plane(evt.ShiftDown() ? 10 : 1); break;
    case WXK_PAGEDOWN: m_panel.step_plane(evt.ShiftDown() ? -10 : -1); break;
    case 'Z':
        // Z alone frames the bed, as in the other 3D views (MainFrame leaves it to this view).
        if (!ctrl)
            zoom_to_bed();
        else if (!held) {
            if (evt.ShiftDown())
                m_panel.redo();
            else
                m_panel.undo();
        }
        break;
    case 'Y':
        if (!ctrl) {
            evt.Skip();
            return;
        }
        if (!held)
            m_panel.redo();
        break;
    case 'E':
        if (ctrl)
            m_panel.export_gcode();
        else if (!held)
            m_panel.toggle_extrude();
        break;
    case 'B':
    case 'V':
    case 'L':
    case 'A':
    case 'R':
    case 'C':
    case 'K':
    case 'T':
    case 'O':
    case 'M':
        if (ctrl) {
            evt.Skip();
            return;
        }
        switch (key) {
        case 'B': zoom_to_bed(); break;
        case 'V': m_panel.set_tool(SketchTool::Select); break;
        case 'L': m_panel.set_tool(SketchTool::Line); break;
        case 'A': m_panel.set_tool(SketchTool::Arc); break;
        case 'R': m_panel.set_tool(SketchTool::Rectangle); break;
        case 'C': m_panel.set_tool(SketchTool::Circle); break;
        // Trim, offset, move/copy: the shortcuts of Fusion 360.
        case 'T': m_panel.set_tool(SketchTool::Trim); break;
        case 'O': m_panel.set_tool(SketchTool::Offset); break;
        case 'M': m_panel.set_tool(SketchTool::Move); break;
        default: toggle_camera_type(); break;
        }
        break;
    case '0': case WXK_NUMPAD0: select_view("iso"); break;
    case '1': case WXK_NUMPAD1: select_view("top"); break;
    case '2': case WXK_NUMPAD2: select_view("bottom"); break;
    case '3': case WXK_NUMPAD3: select_view("front"); break;
    case '4': case WXK_NUMPAD4: select_view("rear"); break;
    case '5': case WXK_NUMPAD5: select_view("left"); break;
    case '6': case WXK_NUMPAD6: select_view("right"); break;
    default:
        evt.Skip();
        return;
    }
    update_cursor_here();
    refresh();
}

void GCodeSketchView::on_key_up(wxKeyEvent &evt)
{
    const int key = evt.GetKeyCode();
    if (key == WXK_ALT) {
        // Alt pressed and released alone: the next working plane. Not skipped, so that Windows doesn't go to the menu
        // of the window.
        if (m_alt_alone && m_down_button == 0) {
            m_panel.next_plane();
            update_cursor_here();
            refresh();
        }
        m_alt_alone = false;
        return;
    }
    if (key == WXK_SHIFT || key == WXK_CONTROL)
        update_cursor_here();
    evt.Skip();
}

void GCodeSketchView::on_char(wxKeyEvent &evt)
{
    // What the keys on_key_down() leaves type: the top row (the minus of a French keyboard, the digits with Shift),
    // and the dot of the keyboards giving it only with Shift (AZERTY).
    sync_fields();
    const wxChar c         = wxChar(evt.GetUnicodeKey());
    const int    digit_key = m_digit_key;
    m_digit_key            = -1;
    if (!m_fields.empty() && !evt.ControlDown() && !evt.AltDown()) {
        if (c == '.' || c == ',') {
            type_char('.');
            return;
        }
        if (c == '-') {
            type_char('-');
            return;
        }
        if (c >= '0' && c <= '9') {
            type_char(c);
            return;
        }
        // A key of the top row typing something no number has (é, &, ç... of a French keyboard without Shift): its
        // digit, so that the digits are typed without Shift.
        if (digit_key >= 0) {
            type_char(wxChar('0' + digit_key));
            return;
        }
    }
    evt.Skip();
}

void GCodeSketchView::on_capture_lost(wxMouseCaptureLostEvent &)
{
    if (m_drag == Drag::MovePoint || m_drag == Drag::MoveSegments)
        m_panel.end_move();
    m_drag            = Drag::None;
    m_moved           = false;
    m_down_button     = 0;
    m_double_click    = false;
    m_pressed_point   = -1;
    m_pressed_segment = -1;
    m_arc_armed       = false;
    m_plane_armed     = false;
}

void GCodeSketchView::on_kill_focus(wxFocusEvent &evt)
{
    // Alt + Tab: no plane change when coming back.
    m_alt_alone = false;
    evt.Skip();
}

} // namespace GUI
} // namespace Slic3r
