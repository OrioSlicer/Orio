// Orio: the bar of the tools painting along the X / Y axes of an object, see PaintAxisBar.hpp.

#include "libslic3r/libslic3r.h"
#include "PaintAxisBar.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"

#include "3DScene.hpp"
#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "GLShader.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "ImGuiWrapper.hpp"
#include "MeshUtils.hpp"
#include "format.hpp"

#include <imgui/imgui_internal.h>

// GLCanvas3D::get_wxglcanvas() only declares wxGLCanvas, whose GetContentScaleFactor() thickness() calls.
#include <GL/glew.h>
#include <wx/glcanvas.h>

namespace Slic3r {
namespace GUI {

// The panel holding the bar, in pixels before its screen scale (paint_axis_bar_scale()).
static constexpr float PANEL_GAP_CUBE = 12.f; // between the navigation cube and the panel
static constexpr float PANEL_MARGIN   = 8.f;  // under the panel, and above it up to what goes over it
static constexpr float PANEL_RIGHT    = 12.f; // between the panel and the right of the scene
static constexpr float PANEL_PADDING  = 8.f;  // inside the panel, around what it holds
static constexpr float PANEL_ROUNDING = 6.f;
static constexpr float HEADER_GAP     = 6.f;  // between the header and the bar
static constexpr float TICK_LENGTH    = 3.f;  // the marks of the graduations, under the bar
static constexpr float BUBBLE_GAP     = 4.f;  // between the panel and the value bubble over it
static constexpr float HATCH_STEP     = 6.f;  // between the lines hatching the margins of the bar
static constexpr float GRADUATION_GAP = 2.f;  // along Z, between the marks of the graduations and their texts
// Size of the text of the graduations, against the size of the ImGui font.
static constexpr float SMALL_TEXT     = 0.85f;

void PaintAxisBar::set_object(const ModelObject *object)
{
    if (object == nullptr) {
        m_extent_x = m_extent_y = { 0., 0. };
        return;
    }
    m_extent_x = paint_axis_object_extent(*object, paX);
    m_extent_y = paint_axis_object_extent(*object, paY);
}

double PaintAxisBar::length() const
{
    if (! m_xy)
        return std::max(0., m_height);
    const std::pair<double, double> e = this->extent(m_axis);
    return std::max(0., e.second - e.first);
}

void PaintAxisBar::set_margins(double start, double end)
{
    m_margin_start = std::max(0., start);
    m_margin_end   = std::max(0., end);
}

std::pair<float, float> PaintAxisBar::paintable_range() const
{
    const double length = this->length();
    // Along Z, the margins are outside the bar.
    if (! m_xy || length <= 0.)
        return { 0.f, 1.f };
    // The margins leave at least a fifth of the axis.
    return { float(std::min(m_margin_start, 0.4 * length) / length), float(1. - std::min(m_margin_end, 0.4 * length) / length) };
}

float PaintAxisBar::paintable(float relative) const
{
    const std::pair<float, float> range = this->paintable_range();
    return std::clamp(relative, range.first, range.second);
}

void PaintAxisBar::update_orientation(const GLCanvas3D &canvas, const ModelObject &object, int instance_idx)
{
    if (object.instances.empty())
        return;
    if (instance_idx < 0 || instance_idx >= (int)object.instances.size())
        instance_idx = 0;
    // The edited axis of the object in the world, against the horizontal of the screen.
    const Transform3d matrix = object.instances[instance_idx]->get_matrix();
    const Vec3d       dir    = (matrix.linear() * (m_axis == paY ? Vec3d::UnitY() : Vec3d::UnitX())).normalized();
    const double      cos_axis = dir.dot(canvas.get_active_camera().get_dir_right());
    m_flipped   = cos_axis < 0.;
    m_alignment = float(std::abs(cos_axis));
}

float PaintAxisBar::thickness(const GLCanvas3D &canvas)
{
    return
#if ENABLE_RETINA_GL
        canvas.get_canvas_size().get_scale_factor()
#else
        canvas.get_wxglcanvas()->GetContentScaleFactor()
#endif
        * THICKNESS;
}

// Screen scale of the panel: the one of the navigation cube (GLCanvas3D::_render_3d_navigator()).
static float paint_axis_bar_scale(const GLCanvas3D &canvas)
{
    float sc = canvas.get_scale();
#ifdef WIN32
    if (const wxWindow *top_window = wxGetApp().GetTopWindow(); top_window != nullptr)
        sc *= float(get_dpi_for_window(top_window)) / float(DPI_DEFAULT);
#endif
    return sc;
}

// Width kept free on the left for the navigation cube, at the bottom left of the scene (GLCanvas3D::_render_3d_navigator(),
// the "Fit camera" button is over the cube).
static float paint_axis_bar_left(const GLCanvas3D &canvas)
{
    return wxGetApp().show_3d_navigator() ? 128.f * paint_axis_bar_scale(canvas) : 0.f;
}

// The panel and its parts, in screen coordinates (y down).
struct PaintAxisBarLayout
{
    bool  vertical { false }; // along Z, at the right of the scene
    float scale    { 1.f };   // screen scale of the margins, the one of the navigation cube
    Rect  panel;              // the framed panel
    Rect  header;             // its top: the title, the X / Y switch, the hints
    Rect  bar;                // the bar
    Rect  graduations;        // under the bar, or on its left along Z
    Rect  start_margin;       // along Z, outside the bar: under it (PaintAxisBar::set_margins())
    Rect  end_margin;         // along Z, outside the bar: above it
};

// Text of the graduation i (0 to 4) of an axis: 0, the quarters, the length in mm.
static std::string paint_axis_bar_graduation_text(double length, int i)
{
    if (i == 0)
        return "0";
    char value[32];
    // Tenths of mm along a short axis, whole mm along a long one.
    std::snprintf(value, sizeof(value), length < 100. ? "%.1f" : "%.0f", length * double(i) / 4.);
    return (i == 4) ? std::string(value) + " mm" : std::string(value);
}

// From the size of the scene and of the ImGui font, which the header and the graduations follow; along Z, also from the
// length of the axis (the width of the texts of the graduations), the height of the title and the margins, in mm.
static PaintAxisBarLayout paint_axis_bar_layout(const GLCanvas3D &canvas, bool vertical, double length, float title_height,
                                                double margin_start, double margin_end)
{
    const Size  cnv_size = canvas.get_canvas_size();
    const float w        = float(cnv_size.get_width());
    const float h        = float(cnv_size.get_height());
    PaintAxisBarLayout layout;
    layout.vertical = vertical;
    layout.scale    = paint_axis_bar_scale(canvas);
    const float sc      = layout.scale;
    const float padding = std::round(PANEL_PADDING * sc);
    const float bottom  = std::round(h - PANEL_MARGIN * sc);
    if (vertical) {
        // From the top down: the margin under the main toolbar, the padding, the title, the gap, then a padding above the
        // bar and one under it, where the mouse paints its ends (as beside the ends of the bar along X / Y).
        const float top        = std::round(float(canvas.get_main_toolbar_height()) + PANEL_MARGIN * sc);
        const float header_end = top + padding + std::round(std::max(title_height, ImGui::GetFrameHeight()) + HEADER_GAP * sc);
        // From the right: the padding, the bar, the marks and the texts of the graduations, the padding.
        const float font_size  = std::round(SMALL_TEXT * ImGui::GetFontSize());
        float       texts      = 0.f;
        // No font before the first ImGui frame.
        if (const ImFont *font = ImGui::GetFont(); font != nullptr)
            for (int i = 0; i <= 4; ++ i)
                texts = std::max(texts, font->CalcTextSizeA(font_size, FLT_MAX, 0.f, paint_axis_bar_graduation_text(length, i).c_str()).x);
        const float right      = std::round(w - PANEL_RIGHT * sc);
        const float bar_left   = right - padding - std::round(PaintAxisBar::thickness(canvas));
        const float left       = std::max(0.f, bar_left - std::round(TICK_LENGTH * sc + GRADUATION_GAP * sc + texts) - padding);
        // Between the paddings, the bar and, outside it, its margins: the one of its start under it, the one of its end
        // above it, at the scale of the bar, at least a padding high to be seen, at most 40 % of the height each.
        const float inner_top    = header_end + padding;
        const float inner_bottom = std::max(inner_top + 1.f, bottom - padding);
        const float inner        = inner_bottom - inner_top;
        const float mm_to_px     = inner / float(std::max(length + std::max(0., margin_start) + std::max(0., margin_end), EPSILON));
        auto        margin_px    = [&](double mm) { return mm > 0. ? std::round(std::min(std::max(float(mm) * mm_to_px, padding), 0.4f * inner)) : 0.f; };
        const float start_px     = margin_px(margin_start);
        const float end_px       = margin_px(margin_end);
        layout.panel        = Rect(left, top, right, bottom);
        layout.header       = Rect(left, top, right, header_end);
        layout.end_margin   = Rect(bar_left, inner_top, right - padding, inner_top + end_px);
        layout.bar          = Rect(bar_left, inner_top + end_px, right - padding, inner_bottom - start_px);
        layout.start_margin = Rect(bar_left, inner_bottom - start_px, right - padding, inner_bottom);
        layout.graduations  = Rect(left, header_end, bar_left, bottom);
        return layout;
    }
    // From the bottom up: the margin, the padding, the graduations, the bar, the gap, the header, the padding.
    const float bar_bottom = bottom - padding - std::round(TICK_LENGTH * sc + SMALL_TEXT * ImGui::GetFontSize());
    const float bar_top    = bar_bottom - std::round(PaintAxisBar::thickness(canvas));
    const float top        = bar_top - std::round(HEADER_GAP * sc + ImGui::GetFrameHeight()) - padding;
    const float left       = std::round(std::min(paint_axis_bar_left(canvas) + PANEL_GAP_CUBE * sc, 0.5f * w));
    const float right      = std::max(left + 2.f * padding + 1.f, std::round(w - PANEL_RIGHT * sc));
    layout.panel       = Rect(left, top, right, bottom);
    layout.header      = Rect(left, top, right, bar_top);
    layout.bar         = Rect(left + padding, bar_top, right - padding, bar_bottom);
    layout.graduations = Rect(left, bar_bottom, right, bottom);
    return layout;
}

PaintAxisBarLayout PaintAxisBar::layout(const GLCanvas3D &canvas) const
{
    return paint_axis_bar_layout(canvas, ! m_xy, this->length(), m_header_height, m_margin_start, m_margin_end);
}

Rect PaintAxisBar::rect_screen(const GLCanvas3D &canvas) const
{
    return this->layout(canvas).bar;
}

Rect PaintAxisBar::rect_viewport(const GLCanvas3D &canvas) const
{
    const Size  cnv_size = canvas.get_canvas_size();
    const float half_w   = 0.5f * float(cnv_size.get_width());
    const float half_h   = 0.5f * float(cnv_size.get_height());
    const float inv_zoom = float(canvas.get_active_camera().get_inv_zoom());
    const Rect  screen   = this->rect_screen(canvas);
    return { (screen.get_left() - half_w) * inv_zoom, (half_h - screen.get_top()) * inv_zoom,
             (screen.get_right() - half_w) * inv_zoom, (half_h - screen.get_bottom()) * inv_zoom };
}

bool PaintAxisBar::contains(const GLCanvas3D &canvas, float x, float y) const
{
    // The bar, and the padding of the panel beside its ends (above and under it along Z), where relative_at() gives
    // the end.
    const PaintAxisBarLayout layout = this->layout(canvas);
    if (layout.vertical)
        return layout.bar.get_left() <= x && x <= layout.panel.get_right() && layout.header.get_bottom() <= y && y <= layout.panel.get_bottom();
    return layout.panel.get_left() <= x && x <= layout.panel.get_right() && layout.bar.get_top() <= y && y <= layout.bar.get_bottom();
}

float PaintAxisBar::reserved_height(const GLCanvas3D &canvas) const
{
    if (! m_xy)
        return 0.f;
    const PaintAxisBarLayout layout = this->layout(canvas);
    // The panel, then the value bubble over it (render_value_tip()): what goes above them does not hide it.
    const float bubble = BUBBLE_GAP * layout.scale + ImGui::GetFontSize() + 2.f * ImGui::GetStyle().WindowPadding.y;
    return float(canvas.get_canvas_size().get_height()) - layout.panel.get_top() + bubble + PANEL_MARGIN * layout.scale;
}

float PaintAxisBar::reserved_width(const GLCanvas3D &canvas) const
{
    if (m_xy)
        return 0.f;
    const PaintAxisBarLayout layout = this->layout(canvas);
    return float(canvas.get_canvas_size().get_width()) - layout.panel.get_left() + PANEL_MARGIN * layout.scale;
}

float PaintAxisBar::relative_along(const GLCanvas3D &canvas, float x, float y) const
{
    if (m_xy)
        return this->relative_along(canvas, x);
    // Along Z, from the bottom of the bar up.
    const Rect rect = this->rect_screen(canvas);
    return (rect.get_bottom() - y) / std::max(1.f, rect.get_bottom() - rect.get_top());
}

float PaintAxisBar::relative_along(const GLCanvas3D &canvas, float x) const
{
    const Rect  rect = this->rect_screen(canvas);
    const float t    = (x - rect.get_left()) / std::max(1.f, rect.get_right() - rect.get_left());
    return m_flipped ? 1.f - t : t;
}

float PaintAxisBar::relative_at(const GLCanvas3D &canvas, float x, float y) const
{
    return this->contains(canvas, x, y) ? this->paintable(this->relative_along(canvas, x, y)) : -1000.f;
}

float PaintAxisBar::cursor_relative(const GLCanvas3D &canvas) const
{
    const Vec2d mouse_pos = canvas.get_local_mouse_position();
    return this->relative_at(canvas, float(mouse_pos.x()), float(mouse_pos.y()));
}

void PaintAxisBar::render_panel_background(const GLCanvas3D &canvas) const
{
    const Size  cnv_size = canvas.get_canvas_size();
    const float w        = float(cnv_size.get_width());
    const float h        = float(cnv_size.get_height());
    const auto &shader   = wxGetApp().get_shader("flat");
    if (w <= 0.f || h <= 0.f || shader == nullptr)
        return;

    const PaintAxisBarLayout layout = this->layout(canvas);
    const Rect &panel  = layout.panel;
    const float width  = panel.get_right() - panel.get_left();
    const float height = panel.get_bottom() - panel.get_top();
    const float radius = std::min(PANEL_ROUNDING * layout.scale, 0.5f * std::min(width, height));
    if (! m_panel.is_initialized() || (m_panel_shape - Vec3f(width, height, radius)).cwiseAbs().maxCoeff() > 0.01f) {
        // The rounded rectangle in pixels around its center, y up: a fan around the center, ARC segments per corner.
        constexpr int ARC = 6;
        const float   hw  = 0.5f * width - radius;
        const float   hh  = 0.5f * height - radius;
        const Vec2f   centers[4] = { Vec2f(hw, hh), Vec2f(- hw, hh), Vec2f(- hw, - hh), Vec2f(hw, - hh) };
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(1 + 4 * (ARC + 1));
        init_data.reserve_indices(3 * 4 * (ARC + 1));
        init_data.add_vertex(Vec3f(0.f, 0.f, 0.f));
        for (int corner = 0; corner < 4; ++ corner)
            for (int i = 0; i <= ARC; ++ i) {
                const float angle = 0.5f * float(PI) * (float(corner) + float(i) / float(ARC));
                init_data.add_vertex(Vec3f(centers[corner].x() + radius * std::cos(angle), centers[corner].y() + radius * std::sin(angle), 0.f));
            }
        const unsigned int count = 4 * (ARC + 1);
        for (unsigned int i = 0; i < count; ++ i)
            init_data.add_triangle(0, 1 + i, 1 + (i + 1) % count);
        m_panel.reset();
        m_panel.init_from(std::move(init_data));
        m_panel_shape = Vec3f(width, height, radius);
    }

    // From the pixels around the center of the panel to normalized device coordinates.
    Transform3d matrix = Transform3d::Identity();
    matrix.matrix()(0, 0) = 2. / double(w);
    matrix.matrix()(1, 1) = 2. / double(h);
    matrix.matrix()(0, 3) = double(panel.get_left() + panel.get_right()) / double(w) - 1.;
    matrix.matrix()(1, 3) = 1. - double(panel.get_top() + panel.get_bottom()) / double(h);

    // The color of the ImGui windows of the scene, see paint_axis_bar_text().
    const bool blend = ::glIsEnabled(GL_BLEND) == GL_TRUE;
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    wxGetApp().bind_shader(shader);
    shader->set_uniform("view_model_matrix", matrix);
    shader->set_uniform("projection_matrix", Transform3d::Identity());
    const ImVec4 &bg_dark = ImGuiWrapper::COL_WINDOW_BG_DARK; // Orio: follows the dark theme
    m_panel.set_color(wxGetApp().dark_mode() ? ColorRGBA(bg_dark.x, bg_dark.y, bg_dark.z, 0.85f) : ColorRGBA(1.f, 1.f, 1.f, 0.85f));
    m_panel.render_geometry();
    wxGetApp().unbind_shader();
    if (! blend)
        glsafe(::glDisable(GL_BLEND));
}

void PaintAxisBar::render_background(const GLCanvas3D &canvas, GLShaderProgram &shader) const
{
    const Size  cnv_size = canvas.get_canvas_size();
    const float w        = float(cnv_size.get_width());
    const float h        = float(cnv_size.get_height());
    if (w <= 0.f || h <= 0.f)
        return;

    // The bar in normalized device coordinates, as the vertical bar of the tools.
    const Rect  rect = this->rect_screen(canvas);
    const float l    = 2.f * rect.get_left() / w - 1.f;
    const float r    = 2.f * rect.get_right() / w - 1.f;
    const float b    = 1.f - 2.f * rect.get_bottom() / h;
    const float t    = 1.f - 2.f * rect.get_top() / h;
    Transform3d model_matrix = Transform3d::Identity();
    model_matrix.data()[3 * 4 + 0] = 0.5f * (l + r);
    model_matrix.data()[3 * 4 + 1] = 0.5f * (t + b);
    model_matrix.data()[3 * 4 + 2] = 0.0f;
    model_matrix.data()[0 * 4 + 0] = r - l;
    model_matrix.data()[1 * 4 + 1] = t - b;
    model_matrix.data()[2 * 4 + 2] = 0.0f;
    shader.set_uniform("view_model_matrix", model_matrix);
    shader.set_uniform("projection_matrix", Transform3d::Identity());
    shader.set_uniform("normal_matrix", (Matrix3d)model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());

    const bool flipped = this->flipped();
    if (! m_background.is_initialized() || m_background_flipped != flipped || m_background_vertical != ! m_xy) {
        // The shader reads the position along the bar from the second texture coordinate, and the first one across it:
        // along X / Y, from left to right (right to left when flipped) and from bottom to top; along Z, from bottom to
        // top and from left to right.
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);
        for (const Vec2f &corner : { Vec2f(-0.5f, -0.5f), Vec2f(0.5f, -0.5f), Vec2f(0.5f, 0.5f), Vec2f(-0.5f, 0.5f) }) {
            const float right = corner.x() + 0.5f;
            const float up    = corner.y() + 0.5f;
            init_data.add_vertex(Vec3f(corner.x(), corner.y(), 0.0f), Vec3f(0.0f, 0.0f, 1.0f),
                                 m_xy ? Vec2f(up, flipped ? 1.0f - right : right) : Vec2f(right, up));
        }
        init_data.add_triangle(0, 1, 2);
        init_data.add_triangle(2, 3, 0);
        m_background.reset();
        m_background.init_from(std::move(init_data));
        m_background_flipped  = flipped;
        m_background_vertical = ! m_xy;
    }
    m_background.render_geometry();
}

Transform3d PaintAxisBar::volume_axis_matrix(const GLVolume &volume) const
{
    // The volume matrix places the vertex in the frame of the object. The shader reads the third row of the result:
    // make it the coordinate along the axis, minus the origin of the axis.
    Transform3d pick = Transform3d::Identity();
    pick.matrix().row(2) = (m_axis == paY) ? Eigen::RowVector4d(0., 1., 0., - this->origin()) : Eigen::RowVector4d(1., 0., 0., - this->origin());
    return pick * volume.get_volume_transformation().get_matrix();
}

// Orio: main text of the panel and of its bubbles, in the colors of the theme (the default text of ImGui is white:
// unreadable on the light theme).
static ImVec4 paint_axis_bar_text_color(bool dark)
{
    return dark ? ImVec4(0.92f, 0.92f, 0.92f, 1.f) : ImVec4(0.16f, 0.16f, 0.16f, 1.f);
}

// A small window of text, anchored at (x, y) by the point (pivot_x, pivot_y) of its box, which lets the mouse through. In
// front of the other windows (the notifications).
static void paint_axis_bar_text(const GLCanvas3D &canvas, const char *name, float x, float y, float pivot_x, float pivot_y, const wxString &text)
{
    ImGuiWrapper &imgui = *wxGetApp().imgui();
    const float   sc    = canvas.get_scale();
    imgui.set_next_window_pos(x, y, ImGuiCond_Always, pivot_x, pivot_y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f * sc);
    const bool dark = wxGetApp().dark_mode();
    const ImVec4 &bg_dark = ImGuiWrapper::COL_WINDOW_BG_DARK; // Orio: follows the dark theme
    ImGui::PushStyleColor(ImGuiCol_WindowBg, dark ? ImVec4(bg_dark.x, bg_dark.y, bg_dark.z, 0.85f) : ImVec4(1.f, 1.f, 1.f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, paint_axis_bar_text_color(dark));
    imgui.begin(std::string(name), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    imgui.text(text);
    imgui.end();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// Secondary text of the panel: where the coordinate grows, the hints, the graduations.
static ImVec4 paint_axis_bar_secondary_color(bool dark)
{
    return dark ? ImVec4(0.62f, 0.62f, 0.65f, 1.f) : ImVec4(0.42f, 0.42f, 0.42f, 1.f);
}

// The graduations of the bar, in mm along the axis from its origin, drawn by the current ImGui window: the two ends, and
// the quarters when their texts have room. Under the bar, or on its left along Z (the axis going up).
static void paint_axis_bar_graduations(const PaintAxisBarLayout &layout, double length, bool flipped, ImU32 color)
{
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImFont     *font      = ImGui::GetFont();
    const float font_size = std::round(SMALL_TEXT * ImGui::GetFontSize());
    const Rect &bar       = layout.bar;
    const float mark      = std::round(TICK_LENGTH * layout.scale);
    const float gap       = std::round(GRADUATION_GAP * layout.scale);
    const float spacing   = std::round(8.f * layout.scale);

    struct Graduation
    {
        float       at;   // along the bar on the screen: x, or y along Z
        float       from; // the text along the bar on the screen
        float       to;
        std::string text;
    };
    // Along the bar on the screen: from its left, or from its bottom up along Z.
    const float start = layout.vertical ? bar.get_bottom() : bar.get_left();
    const float end   = layout.vertical ? bar.get_top() : bar.get_right();
    std::vector<Graduation> graduations;
    for (int i = 0; i <= 4; ++ i) {
        std::string  text = paint_axis_bar_graduation_text(length, i);
        const ImVec2 size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, text.c_str());
        const float  t    = float(i) / 4.f;
        const float  at   = std::round(start + (flipped ? 1.f - t : t) * (end - start));
        // The texts of the ends stay along the bar, those of the quarters are centered on their mark.
        const float  extent = layout.vertical ? size.y : size.x;
        const float  lo     = std::min(start, end);
        const float  from   = std::clamp(std::round(at - 0.5f * extent), lo, std::max(lo, std::max(start, end) - extent));
        graduations.push_back({ at, from, from + extent, std::move(text) });
    }
    std::vector<Graduation> sorted = graduations;
    std::sort(sorted.begin(), sorted.end(), [](const Graduation &a, const Graduation &b) { return a.from < b.from; });
    bool room = true;
    for (size_t i = 1; i < sorted.size(); ++ i)
        room = room && sorted[i - 1].to + spacing <= sorted[i].from;

    for (size_t i = 0; i < graduations.size(); ++ i) {
        if (! room && i != 0 && i + 1 != graduations.size())
            continue;
        const Graduation &graduation = graduations[i];
        // The marks of the ends along the edges of the bar.
        if (layout.vertical) {
            const float y     = std::clamp(std::floor(graduation.at) + 0.5f, bar.get_top() + 0.5f, bar.get_bottom() - 0.5f);
            const float width = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, graduation.text.c_str()).x;
            draw_list->AddLine(ImVec2(bar.get_left() - mark, y), ImVec2(bar.get_left(), y), color);
            draw_list->AddText(font, font_size, ImVec2(bar.get_left() - mark - gap - width, graduation.from), color, graduation.text.c_str());
        } else {
            const float x = std::clamp(std::floor(graduation.at) + 0.5f, bar.get_left() + 0.5f, bar.get_right() - 0.5f);
            draw_list->AddLine(ImVec2(x, bar.get_bottom()), ImVec2(x, bar.get_bottom() + mark), color);
            draw_list->AddText(font, font_size, ImVec2(graduation.from, bar.get_bottom() + mark), color, graduation.text.c_str());
        }
    }
}

// The margins of the bar, which cannot be painted (PaintAxisBar::set_margins()): hatched over the bar drawn in GL, under
// the ImGui windows, with a line at the limit of the part that can be painted.
static void paint_axis_bar_margins(const PaintAxisBarLayout &layout, std::pair<float, float> paintable, bool flipped, bool dark, ImU32 color)
{
    ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
    const Rect &bar   = layout.bar;
    const float width = bar.get_right() - bar.get_left();
    const ImVec4 &bg_dark = ImGuiWrapper::COL_WINDOW_BG_DARK; // Orio: follows the dark theme
    const ImU32 fill  = ImGui::GetColorU32(dark ? ImVec4(bg_dark.x, bg_dark.y, bg_dark.z, 0.7f) : ImVec4(1.f, 1.f, 1.f, 0.7f));
    const float step  = std::max(2.f, std::round(HATCH_STEP * layout.scale));
    struct Margin
    {
        float from; // along the axis, 0 at its origin
        float to;
        bool  start;
    };
    for (const Margin &margin : { Margin{ 0.f, paintable.first, true }, Margin{ paintable.second, 1.f, false } }) {
        // Along Z, outside the bar: under it for its start, above it for its end. Along X / Y, on the bar, which runs from
        // right to left on the screen when flipped.
        ImVec2 min, max;
        if (layout.vertical) {
            const Rect &rect = margin.start ? layout.start_margin : layout.end_margin;
            min = ImVec2(rect.get_left(), rect.get_top());
            max = ImVec2(rect.get_right(), rect.get_bottom());
            if (max.y - min.y < 1.f)
                continue;
        } else {
            min = ImVec2(bar.get_left() + (flipped ? 1.f - margin.to : margin.from) * width, bar.get_top());
            max = ImVec2(bar.get_left() + (flipped ? 1.f - margin.from : margin.to) * width, bar.get_bottom());
            if (max.x - min.x < 1.f)
                continue;
        }
        draw_list->AddRectFilled(min, max, fill);
        draw_list->PushClipRect(min, max, true);
        const float height = max.y - min.y;
        for (float x = min.x - height; x < max.x; x += step)
            draw_list->AddLine(ImVec2(x, max.y), ImVec2(x + height, min.y), color);
        draw_list->PopClipRect();
        // The limit is on the side of the part that can be painted.
        if (layout.vertical) {
            const float limit = std::floor(margin.start ? min.y : max.y) + 0.5f;
            draw_list->AddLine(ImVec2(min.x, limit), ImVec2(max.x, limit), color);
        } else {
            const float limit = std::floor((margin.start != flipped) ? max.x : min.x) + 0.5f;
            draw_list->AddLine(ImVec2(limit, min.y), ImVec2(limit, max.y), color);
        }
    }
}

PaintAxis PaintAxisBar::render_panel(const GLCanvas3D &canvas, const std::string &title, bool x_painted, bool y_painted) const
{
    ImGuiWrapper &imgui = *wxGetApp().imgui();
    const PaintAxisBarLayout layout = this->layout(canvas);
    const float  sc        = layout.scale;
    const float  padding   = std::round(PANEL_PADDING * sc);
    const bool   dark      = wxGetApp().dark_mode();
    const ImVec4 secondary = paint_axis_bar_secondary_color(dark);
    const int    flags     = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing;

    // The outline of the panel: under the ImGui windows, over the background and the bar drawn in GL.
    ImGui::GetBackgroundDrawList()->AddRect(ImVec2(layout.panel.get_left(), layout.panel.get_top()),
        ImVec2(layout.panel.get_right(), layout.panel.get_bottom()),
        ImGui::GetColorU32(dark ? ImGuiWrapper::COL_SEPARATOR_DARK : ImVec4(0.82f, 0.82f, 0.82f, 1.f)), PANEL_ROUNDING * sc);
    // The hatching lighter than the texts.
    paint_axis_bar_margins(layout, this->paintable_range(), this->flipped(), dark, ImGui::GetColorU32(ImVec4(secondary.x, secondary.y, secondary.z, 0.7f)));

    // The header and the graduations: two windows without background, which also keep the mouse off the scene.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * sc);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    imgui.set_next_window_pos(layout.header.get_left(), layout.header.get_top(), ImGuiCond_Always, 0.f, 0.f);
    imgui.set_next_window_size(layout.header.get_width(), layout.header.get_bottom() - layout.header.get_top(), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_Text, paint_axis_bar_text_color(dark)); // Orio: the title, readable on the light theme
    imgui.begin(std::string("paint_axis_bar_header"), flags);

    PaintAxis clicked = paZ;
    if (layout.vertical) {
        // Along Z, the title of the tool only, in bold, over as many lines as needed: the layout follows its height.
        ImGui::PushTextWrapPos(0.f);
        imgui.bold_text(title);
        ImGui::PopTextWrapPos();
        m_header_height = ImGui::GetItemRectSize().y;
    } else {
        // The title of the tool, in bold.
        ImGui::AlignTextToFramePadding();
        imgui.bold_text(title);
        ImGui::SameLine(0.f, 2.f * ImGui::GetStyle().ItemSpacing.x);

        // The switch: the edited axis in the colors of a confirmation button, the other one in those of a cancel button.
        const float button_width = 1.6f * ImGui::GetFrameHeight();
        for (PaintAxis axis : { paX, paY }) {
            const bool active = axis == m_axis;
            if (active)
                ImGuiWrapper::push_confirm_button_style();
            else
                ImGuiWrapper::push_cancel_button_style();
            if (ImGui::Button(axis == paX ? "X##paint_axis_x" : "Y##paint_axis_y", ImVec2(button_width, 0.f)) && ! active)
                clicked = axis;
            if (active)
                ImGuiWrapper::pop_confirm_button_style();
            else
                ImGuiWrapper::pop_cancel_button_style();
            ImGui::SameLine();
        }

        // In the secondary color: the arrows tell where the coordinate grows on the screen; on the right, the hints.
        ImGui::PushStyleColor(ImGuiCol_Text, secondary);
        const wxString axis_name = (m_axis == paY) ? _L("Object Y axis") : _L("Object X axis");
        ImGui::AlignTextToFramePadding();
        imgui.text(m_flipped ? ("<<  " + axis_name) : (axis_name + "  >>"));
        wxString hints;
        if (m_alignment < 0.35f)
            hints = _L("(seen end-on: turn the view)");
        if ((m_axis == paY) ? x_painted : y_painted)
            // Its painting applies too, though the object is colored by the edited axis only.
            hints += (hints.empty() ? "" : "  ") + format_wxstr(_L("(%1% painted too)"), (m_axis == paY) ? "X" : "Y");
        if (! hints.empty()) {
            // On the right of the header, without lengthening it: the window clips them when there is no room.
            const std::string text      = into_u8(hints);
            const float       label_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 2.f * ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine(std::max(label_end, ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(text.c_str()).x));
            imgui.text(text);
        }
        ImGui::PopStyleColor();
    }
    imgui.end();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    imgui.set_next_window_pos(layout.graduations.get_left(), layout.graduations.get_top(), ImGuiCond_Always, 0.f, 0.f);
    imgui.set_next_window_size(layout.graduations.get_width(), layout.graduations.get_bottom() - layout.graduations.get_top(), ImGuiCond_Always);
    imgui.begin(std::string("paint_axis_bar_footer"), flags);
    paint_axis_bar_graduations(layout, this->length(), this->flipped(), ImGui::GetColorU32(secondary));
    imgui.end();
    ImGui::PopStyleVar(5);
    return clicked;
}

void PaintAxisBar::render_value_tip(const GLCanvas3D &canvas, const std::string &text, float relative) const
{
    if (text.empty())
        return;
    const Vec2d mouse = canvas.get_local_mouse_position();
    const PaintAxisBarLayout layout = this->layout(canvas);
    // At the mouse over the panel; with the mouse over the object, where its value is along the bar.
    float at_x = float(mouse.x());
    float at_y = float(mouse.y());
    if (! this->contains(canvas, at_x, at_y)) {
        if (relative < 0.f || relative > 1.f)
            return;
        const Rect &bar = layout.bar;
        if (layout.vertical)
            at_y = bar.get_bottom() - relative * (bar.get_bottom() - bar.get_top());
        else
            at_x = this->flipped() ? bar.get_right() - relative * (bar.get_right() - bar.get_left()) : bar.get_left() + relative * (bar.get_right() - bar.get_left());
    }
    const float margin     = BUBBLE_GAP * layout.scale;
    if (layout.vertical) {
        // On the left of the panel, at the height of the mouse (of the value): it hides neither the bar nor its
        // graduations; it stays on the screen at both ends of the bar.
        const float half_height = 0.5f * ImGui::GetFontSize() + ImGui::GetStyle().WindowPadding.y;
        const float y           = std::clamp(at_y, half_height + margin,
                                             std::max(half_height + margin, float(canvas.get_canvas_size().get_height()) - half_height - margin));
        paint_axis_bar_text(canvas, "paint_axis_bar_value", layout.panel.get_left() - margin, y, 1.f, 0.5f, wxString::FromUTF8(text.c_str()));
        return;
    }
    // Half the width of the bubble: it stays on the screen at both ends of the bar.
    const float half_width = 0.5f * ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().WindowPadding.x;
    const float x          = std::clamp(at_x, half_width + margin,
                                        std::max(half_width + margin, float(canvas.get_canvas_size().get_width()) - half_width - margin));
    // Above the panel, at the abscissa of the mouse (of the value): it hides neither the bar nor the header.
    paint_axis_bar_text(canvas, "paint_axis_bar_value", x, layout.panel.get_top() - margin, 0.5f, 1.f, wxString::FromUTF8(text.c_str()));
}

float PaintAxisBar::part_coordinate_at(const GLCanvas3D &canvas, const ModelObject &object, float x, float y) const
{
    float ret = std::numeric_limits<float>::quiet_NaN();
    // A ray caster per solid part, built again when the parts or their meshes changed (building it for a big mesh takes a
    // while).
    size_t i    = 0;
    bool   same = true;
    for (const ModelVolume *volume : object.volumes)
        if (volume->is_model_part()) {
            same = same && i < m_part_raycasters.size() && m_part_raycasters[i].volume == volume && m_part_raycasters[i].mesh == volume->get_mesh_shared_ptr();
            ++ i;
        }
    if (! same || i != m_part_raycasters.size()) {
        m_part_raycasters.clear();
        for (const ModelVolume *volume : object.volumes)
            if (volume->is_model_part())
                m_part_raycasters.push_back({ volume, volume->get_mesh_shared_ptr(), std::make_shared<MeshRaycaster>(volume->mesh()) });
    }
    const int    instance_idx = canvas.get_selection().get_instance_idx();
    const double length       = this->length();
    if (instance_idx < 0 || instance_idx >= (int)object.instances.size() || length <= 0.)
        return ret;
    const Camera     &camera   = canvas.get_active_camera();
    const Transform3d instance = object.instances[instance_idx]->get_matrix();
    // The nearest of the hits on the solid parts.
    double nearest = std::numeric_limits<double>::max();
    for (const PartRaycaster &part : m_part_raycasters) {
        const Transform3d volume = part.volume->get_matrix();
        Vec3f position, normal;
        if (! part.raycaster->unproject_on_mesh(Vec2d(x, y), instance * volume, camera, position, normal))
            continue;
        const Vec3d  world = instance * volume * position.cast<double>();
        const double depth = (world - camera.get_position()).dot(camera.get_dir_forward());
        if (depth >= nearest)
            continue;
        nearest = depth;
        const Vec3d  in_object = volume * position.cast<double>();
        const double along     = m_xy ? (m_axis == paY ? in_object.y() : in_object.x()) - this->origin() : world.z();
        ret = float(std::clamp(along, 0., length));
    }
    return ret;
}

// Along Z, the tops of the layers of the object (as GLCanvas3D::LayersEditing slices it), where a bound goes: the values
// change layer by layer.
static std::vector<double> paint_axis_bar_layer_tops(const DynamicPrintConfig *config, const ModelObject &object, double height)
{
    std::vector<double> tops;
    if (config == nullptr || height <= 0.)
        return tops;
    const SlicingParameters params = PrintObject::slicing_parameters(*config, object, float(height));
    std::vector<coordf_t>   profile;
    bool                    nozzle_range_reset = false;
    PrintObject::update_layer_height_profile(object, params, profile, nozzle_range_reset);
    if (profile.empty())
        return tops;
    const std::vector<coordf_t> layers = generate_object_layers(params, profile, false);
    for (size_t i = 1; i < layers.size(); i += 2)
        tops.push_back(layers[i]);
    return tops;
}

static const char *PAINT_AXIS_BAR_BOUNDS_MENU = "paint_axis_bar_bounds_menu";

PaintAxisBar::BoundsEdit PaintAxisBar::render_bounds(const GLCanvas3D &canvas, const ModelObject &object, const DynamicPrintConfig *config,
                                                     std::vector<double> &bounds)
{
    BoundsEdit   ret    = BoundsEdit::None;
    const double length = this->length();
    if (length <= 0.)
        return ret;
    const Rect   bar   = this->rect_screen(canvas);
    const float  size  = thickness(canvas); // the width of the bar
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // Over the graduations, which a window of the panel covers: the scene does not get these clicks. Not while the menu is
    // open: a click on it is not for a bound under it.
    const bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && ! ImGui::IsPopupOpen(PAINT_AXIS_BAR_BOUNDS_MENU) &&
        (m_xy ? (bar.get_left() <= mouse.x && mouse.x <= bar.get_right() && bar.get_bottom() < mouse.y && mouse.y <= bar.get_bottom() + 0.6f * size) :
                (bar.get_left() - 1.2f * size <= mouse.x && mouse.x < bar.get_left() && bar.get_top() <= mouse.y && mouse.y <= bar.get_bottom()));
    // A coordinate along the bar on the screen, and the coordinate under the mouse; along Z, brought to the top of a layer.
    auto screen_of = [&](double coordinate) {
        const float t = float(coordinate / length);
        if (! m_xy)
            return bar.get_bottom() - t * (bar.get_bottom() - bar.get_top());
        return this->flipped() ? bar.get_right() - t * (bar.get_right() - bar.get_left()) : bar.get_left() + t * (bar.get_right() - bar.get_left());
    };
    std::vector<double> layer_tops;
    auto tops = [&]() -> const std::vector<double>& {
        if (layer_tops.empty())
            layer_tops = paint_axis_bar_layer_tops(config, object, length);
        return layer_tops;
    };
    auto coordinate_at_mouse = [&]() {
        double coordinate = length * std::clamp(double(this->relative_along(canvas, mouse.x, mouse.y)), 0., 1.);
        if (! m_xy) {
            double       nearest = std::numeric_limits<double>::max();
            const double wanted  = coordinate;
            for (const double top : tops())
                if (top > BOUND_MIN_GAP && top < length - BOUND_MIN_GAP && std::abs(top - wanted) < nearest) {
                    nearest    = std::abs(top - wanted);
                    coordinate = top;
                }
        }
        return coordinate;
    };

    // The bound under the mouse.
    int hovered = -1;
    if (over && m_bound_drag < 0) {
        const float along = m_xy ? mouse.x : mouse.y;
        float       best  = 0.15f * size;
        for (size_t i = 0; i < bounds.size(); ++ i)
            if (const float distance = std::abs(screen_of(bounds[i]) - along); distance <= best) {
                best    = distance;
                hovered = int(i);
            }
    }
    // Dragging one, between its neighbours; the tool commits it when it is released.
    if (m_bound_drag >= int(bounds.size()))
        m_bound_drag = -1;
    if (m_bound_drag >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const double low        = (m_bound_drag > 0 ? bounds[m_bound_drag - 1] : 0.) + BOUND_MIN_GAP;
            const double high       = (m_bound_drag + 1 < int(bounds.size()) ? bounds[m_bound_drag + 1] : length) - BOUND_MIN_GAP;
            const double coordinate = coordinate_at_mouse();
            if (coordinate >= low && coordinate <= high && coordinate != bounds[m_bound_drag]) {
                bounds[m_bound_drag] = coordinate;
                ret = BoundsEdit::Dragging;
            }
        } else {
            m_bound_drag = -1;
            ret = BoundsEdit::Moved;
        }
    } else if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_bound_drag = hovered;
    // The menu, by a right click over the graduations.
    if (over && m_bound_drag < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_bounds_menu_coordinate = coordinate_at_mouse();
        m_bounds_menu_bound      = hovered;
        ImGui::OpenPopup(PAINT_AXIS_BAR_BOUNDS_MENU);
    }
    if (ImGui::BeginPopup(PAINT_AXIS_BAR_BOUNDS_MENU)) {
        const double coordinate = m_bounds_menu_coordinate;
        // Not on another one, nor at an end of the bar.
        const bool room = coordinate > BOUND_MIN_GAP && coordinate < length - BOUND_MIN_GAP &&
            std::none_of(bounds.begin(), bounds.end(), [coordinate](double bound) { return std::abs(bound - coordinate) < BOUND_MIN_GAP; });
        if (ImGui::MenuItem(_u8L("Place a bound").c_str(), nullptr, false, room)) {
            bounds.insert(std::upper_bound(bounds.begin(), bounds.end(), coordinate), coordinate);
            ret = BoundsEdit::Added;
        } else if (m_bounds_menu_bound >= 0 && m_bounds_menu_bound < int(bounds.size()) && ImGui::MenuItem(_u8L("Delete the bound").c_str())) {
            bounds.erase(bounds.begin() + m_bounds_menu_bound);
            ret = BoundsEdit::Deleted;
        } else if (! bounds.empty() && ImGui::MenuItem(_u8L("Delete all the bounds").c_str())) {
            bounds.clear();
            ret = BoundsEdit::DeletedAll;
        }
        ImGui::EndPopup();
    }

    // Drawn over the bar drawn in GL, under the ImGui windows: a line across the bar, and a tab over the graduations.
    ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
    const ImU32 color     = ImGui::GetColorU32(ImGuiWrapper::COL_ORANGE_LIGHT);
    const float tab_long  = std::round(0.3f * size);
    const float tab_half  = std::max(2.0f, std::round(0.09f * size));
    for (size_t i = 0; i < bounds.size(); ++ i) {
        const float p     = std::round(screen_of(bounds[i]));
        const float width = (int(i) == hovered || int(i) == m_bound_drag) ? std::max(2.0f, std::round(0.07f * size)) : std::max(1.0f, std::round(0.035f * size));
        if (m_xy) {
            draw_list->AddLine(ImVec2(p, bar.get_top()), ImVec2(p, bar.get_bottom()), color, width);
            draw_list->AddRectFilled(ImVec2(p - tab_half, bar.get_bottom()), ImVec2(p + tab_half, bar.get_bottom() + tab_long), color, 0.5f * tab_half);
        } else {
            draw_list->AddLine(ImVec2(bar.get_left(), p), ImVec2(bar.get_right(), p), color, width);
            draw_list->AddRectFilled(ImVec2(bar.get_left() - tab_long, p - tab_half), ImVec2(bar.get_left(), p + tab_half), color, 0.5f * tab_half);
        }
    }
    // The bound under the mouse or dragged: its height and its layer along Z (the layer whose top it is, the last one on its
    // lower side), its coordinate along X / Y. Over everything, as the graduations are under it.
    const int labelled = (m_bound_drag >= 0) ? m_bound_drag : hovered;
    if (labelled >= 0 && labelled < int(bounds.size())) {
        const double coordinate = bounds[labelled];
        char         mm[64];
        snprintf(mm, sizeof(mm), "%.2f mm", coordinate);
        std::string text = mm;
        if (! m_xy) {
            const std::vector<double> &layers = tops();
            const auto                 top    = std::lower_bound(layers.begin(), layers.end(), coordinate - EPSILON);
            if (top != layers.end())
                text = _u8L("Layer") + " " + std::to_string(int(top - layers.begin()) + 1) + " \xC2\xB7 " + text;
        }
        const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
        const float  padding   = std::max(2.0f, std::round(0.08f * size));
        const float  p         = screen_of(coordinate);
        const ImVec2 min       = m_xy ? ImVec2(p + tab_half + padding, bar.get_bottom() + padding) :
                                        ImVec2(bar.get_left() - tab_long - 2.0f * padding - (text_size.x + 2.0f * padding), p - 0.5f * text_size.y - padding);
        const ImVec2 max(min.x + text_size.x + 2.0f * padding, min.y + text_size.y + 2.0f * padding);
        ImDrawList  *top_list  = ImGui::GetForegroundDrawList();
        top_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_PopupBg), padding);
        top_list->AddRect(min, max, color, padding);
        top_list->AddText(ImVec2(min.x + padding, min.y + padding), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    }
    return ret;
}

bool PaintAxisBar::render_axis_toggle(bool &xy, bool z_edited, bool xy_edited)
{
    ImGuiWrapper &imgui   = *wxGetApp().imgui();
    bool          changed = false;
    if (imgui.bbl_radio_button("##paint_axis_z", ! xy) && xy) {
        xy      = false;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text((xy && z_edited) ? std::string("Z  ") + _u8L("(edited)") : std::string("Z"));
    ImGui::SameLine();
    if (imgui.bbl_radio_button("##paint_axis_xy", xy) && ! xy) {
        xy      = true;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text((! xy && xy_edited) ? std::string("X / Y  ") + _u8L("(edited)") : std::string("X / Y"));
    return changed;
}

} // namespace GUI
} // namespace Slic3r
