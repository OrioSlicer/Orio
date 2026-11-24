#include "libslic3r/libslic3r.h"
#include "GLCanvas3D.hpp"

#include <chrono>
#include <igl/unproject.h>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Technologies.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/GUI_Preview.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/Utils/MacDarkMode.hpp"

#include "WipeTowerDialog.hpp"
#include "GLToolbar.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Colors.hpp"
#include "Mouse3DController.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"
#include "format.hpp"
#include "DailyTips.hpp"
#include "FilamentMapDialog.hpp"
#include "Jobs/ArrangePrusa.hpp"
#include "DeviceCore/DevConfigUtil.h"
#include "../Utils/CpuMemory.hpp"
#if ENABLE_RETINA_GL
#include "slic3r/Utils/RetinaHelper.hpp"
#endif
#ifdef __APPLE__
#include "libslic3r/MacUtils.hpp"
#endif
#include <GL/glew.h>

#include <wx/glcanvas.h>
#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/tooltip.h>
#include <wx/debug.h>
#include <wx/fontutil.h>
// Print now includes tbb, and tbb includes Windows. This breaks compilation of wxWidgets if included before wx.
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "GCodeViewer.hpp"

#include "wxExtensions.hpp"

#include <tbb/parallel_for.h>
#include <tbb/spin_mutex.h>

#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <iostream>
#include <float.h>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>

#define TINYBVH_NO_SIMD
#ifdef _WINDOWS_
#ifdef MessageBox
#undef MessageBox
#endif
#define MessageBox MessageBoxA
#endif
#define TINYBVH_IMPLEMENTATION
#include "../../tinybvh/tiny_bvh.h"
#undef TINYBVH_IMPLEMENTATION
#ifdef _WINDOWS_
#undef MessageBox
#endif
#undef TINYBVH_NO_SIMD

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>
#include <imguizmo/ImGuizmo.h>
static constexpr const float TRACKBALLSIZE = 0.8f;

static const float SLIDER_DEFAULT_RIGHT_MARGIN  = 10.0f;
static const float SLIDER_DEFAULT_BOTTOM_MARGIN = 10.0f;
static const float SLIDER_RIGHT_MARGIN          = 124.0f;
static const float SLIDER_BOTTOM_MARGIN         = 64.0f;

float GLCanvas3D::DEFAULT_BG_LIGHT_COLOR[3] = { 0.906f, 0.906f, 0.906f };
float GLCanvas3D::DEFAULT_BG_LIGHT_COLOR_DARK[3] = { 0.204f, 0.204f, 0.224f }; // Orio : thème sombre plus noir (était 0.329, 0.329, 0.353)
float GLCanvas3D::ERROR_BG_LIGHT_COLOR[3] = { 0.753f, 0.192f, 0.039f };
float GLCanvas3D::ERROR_BG_LIGHT_COLOR_DARK[3] = { 0.753f, 0.192f, 0.039f };

void GLCanvas3D::update_render_colors()
{
    GLCanvas3D::DEFAULT_BG_LIGHT_COLOR[0] = RenderColor::colors[RenderCol_3D_Background].x;
    GLCanvas3D::DEFAULT_BG_LIGHT_COLOR[1] = RenderColor::colors[RenderCol_3D_Background].y;
    GLCanvas3D::DEFAULT_BG_LIGHT_COLOR[2] = RenderColor::colors[RenderCol_3D_Background].z;
}

void GLCanvas3D::load_render_colors()
{
    RenderColor::colors[RenderCol_3D_Background] = ImVec4(GLCanvas3D::DEFAULT_BG_LIGHT_COLOR[0],
                                                          GLCanvas3D::DEFAULT_BG_LIGHT_COLOR[1],
                                                          GLCanvas3D::DEFAULT_BG_LIGHT_COLOR[2],
                                                          1.0f);
}

//static constexpr const float AXES_COLOR[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

// Number of floats
static constexpr const size_t MAX_VERTEX_BUFFER_SIZE     = 131072 * 6; // 3.15MB
// Reserve size in number of floats.
static constexpr const size_t VERTEX_BUFFER_RESERVE_SIZE = 131072 * 2; // 1.05MB
// Reserve size in number of floats, maximum sum of all preallocated buffers.
//static constexpr const size_t VERTEX_BUFFER_RESERVE_SIZE_SUM_MAX = 1024 * 1024 * 128 / 4; // 128MB

namespace Slic3r {
namespace GUI {

bool                                        GLCanvas3D::s_enable_bvh = true;
std::vector<GLCanvas3D::IsolatedVolumeInfo> GLCanvas3D::s_isolated_volumes;
int                                         GLCanvas3D::s_assemble_candidate_volumes_size;
bool                                        GLCanvas3D::s_isolated_notification_shown = false;
bool                                        GLCanvas3D::s_intersects_notification_shown = false;
int                                         GLCanvas3D::s_assemble_ratio = 0;
int                                         GLCanvas3D::s_assemble_volume_ratio = 0;
bool                                        GLCanvas3D::s_far_from_origin_notification_shown = false;
BoundingBoxf3                               GLCanvas3D::s_bvh_primary_bounds;
double                                      GLCanvas3D::s_expand_bvh_box_dist = 50.0;
BoundingBoxf3                               GLCanvas3D::s_bvh_expanded_bounds;
// Orio : vignette de la vue d'assemblage masquée tant que l'outil « Vue d'assemblage » n'a pas servi, puis par sa
// croix ; réaffichée en entrant de nouveau dans la vue d'assemblage (_render_assembly_view_thumbnail_toolbar)
static bool                                 s_assembly_view_thumbnail_hidden = true;
BoundingBoxf3                               GLCanvas3D::s_first_primary_bounds;

#ifdef __WXGTK3__
// wxGTK3 seems to simulate OSX behavior in regard to HiDPI scaling support.
RetinaHelper::RetinaHelper(wxWindow* window) : m_window(window), m_self(nullptr) {}
RetinaHelper::~RetinaHelper() {}
float RetinaHelper::get_scale_factor() { return float(m_window->GetContentScaleFactor()); }
#endif // __WXGTK3__

// Fixed the collision between BuildVolume::Type::Convex and macro Convex defined inside /usr/include/X11/X.h that is included by WxWidgets 3.0.
#if defined(__linux__) && defined(Convex)
#undef Convex
#endif


std::string get_object_limited_text() {
    std::string object_limited_text = _u8L("An object is placed in the left/right nozzle-only area or exceeds the printable height of the left nozzle.\n"
            "Please ensure the filaments used by this object are not arranged to other nozzles.");
    return object_limited_text;
}

// serve as a text container, not the real text
std::string& get_object_clashed_text() {
    static std::string object_clashed_text = _u8L("An object is laid over the boundary of plate or exceeds the height limit.\n"
            "Please solve the problem by moving it totally on or off the plate, and confirming that the height is within the build volume.");
    return object_clashed_text;
}

std::string& get_assembly_too_far_text() {
    static std::string assembly_warning_too_far{};
    return assembly_warning_too_far;
}

std::string& get_left_extruder_unprintable_text() {
    static std::string left_unprintable_text;
    return left_unprintable_text;
}

std::string& get_right_extruder_unprintable_text() {
    static std::string right_unprintable_text;
    return right_unprintable_text;
}

std::string& get_nozzle_filament_incompatible_text() {
    static std::string nozzle_filament_incompatible_text;
    return nozzle_filament_incompatible_text;
}

std::string& get_filament_mixture_warning_text(){
    static std::string filament_mixture_warning_text;
    return filament_mixture_warning_text;
}
std::string &get_filament_flow_warning_text()
{
    static std::string filament_flow_warning_text;
    return filament_flow_warning_text;
}
std::string& get_tpu_nozzle_multi_filaments_warning_text()
{
    static std::string tpu_nozzle_multi_filaments_warning_text;
    return tpu_nozzle_multi_filaments_warning_text;
}

std::string& get_high_temp_wrapping_warning_text()
{
    static std::string high_temp_wrapping_warning_text;
    return high_temp_wrapping_warning_text;
}

static std::string& get_single_extruder_mixed_filament_warning_text()
{
    static std::string text;
    return text;
}

static std::string format_number(float value)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision((static_cast<int>(value * 10) % 10 == 0) ? 0 : 1) << value;
    return oss.str();
}

wxString filament_printable_error_msg;

GLModel GLCanvas3D::s_full_screen_mesh;

GLCanvas3D::LayersEditing::~LayersEditing()
{
    if (m_z_texture_id != 0) {
        glsafe(::glDeleteTextures(1, &m_z_texture_id));
        m_z_texture_id = 0;
    }
    delete m_slicing_parameters;

    m_baseline.reset();
    m_profile_curve.reset();
}

void GLCanvas3D::LayersEditing::init()
{
    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    const auto& gl_info = p_ogl_manager->get_gl_info();
    const auto formated_gl_version = gl_info.get_formated_gl_version();
    glsafe(::glGenTextures(1, (GLuint*)&m_z_texture_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, formated_gl_version >= 30 ? GL_CLAMP_TO_EDGE : GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, formated_gl_version >= 30 ? GL_CLAMP_TO_EDGE : GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    // Orio: "Hide the other objects", as left in a previous session.
    if (const auto* app_config = wxGetApp().app_config; app_config != nullptr) {
        const std::string v = app_config->get("layers_editing", "hide_other_objects");
        m_hide_other_objects = v == "true" || v == "1";
    }
}

void GLCanvas3D::LayersEditing::set_config(const DynamicPrintConfig* config)
{
    m_config = config;
    delete m_slicing_parameters;
    m_slicing_parameters = nullptr;
    m_layers_texture.valid = false;
    m_layer_height_profile.clear();
    m_profile_dirty = true;
}

void GLCanvas3D::LayersEditing::select_object(const Model& model, int object_id)
{
    // Orio: within the model, as SpeedEditing::select_object() (the selection may still name an object of the
    // previous model while a project is being closed or opened).
    const ModelObject* model_object_new = (object_id >= 0 && object_id < (int)model.objects.size()) ? model.objects[object_id] : nullptr;
    // Maximum height of an object changes when the object gets rotated or scaled.
    // Changing maximum height of an object will invalidate the layer heigth editing profile.
    // m_model_object->bounding_box() is cached, therefore it is cheap even if this method is called frequently.
    const float new_max_z = (model_object_new == nullptr) ? 0.0f : static_cast<float>(model_object_new->bounding_box().max.z());
    if (m_model_object != model_object_new || this->last_object_id != object_id || m_object_max_z != new_max_z ||
        (model_object_new != nullptr && m_model_object->id() != model_object_new->id())) {
        m_layer_height_profile.clear();
        delete m_slicing_parameters;
        m_slicing_parameters = nullptr;
        m_layers_texture.valid = false;
        this->last_object_id = object_id;
        m_model_object = model_object_new;
        m_object_max_z = new_max_z;
        m_profile_dirty = true;
        m_bound_drag = -1;
    }
}

bool GLCanvas3D::LayersEditing::is_allowed() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr && m_z_texture_id > 0;
}

bool GLCanvas3D::LayersEditing::is_enabled() const
{
    return m_enabled;
}

void GLCanvas3D::LayersEditing::set_enabled(bool enabled)
{
    m_enabled = is_allowed() && enabled;
    if (! m_enabled)
        m_part_raycasters.clear();
}

float GLCanvas3D::LayersEditing::s_overlay_window_width;
void GLCanvas3D::LayersEditing::show_tooltip_information(const GLCanvas3D& canvas, std::map<wxString, wxString> captions_texts, float x, float y)
{
    //Orio: shown by the keyboard icon right of the name of the tool, in the title bar of its window.
    wxGetApp().imgui()->tool_window_shortcuts(std::vector<std::pair<wxString, wxString>>(captions_texts.begin(), captions_texts.end()));
}

void GLCanvas3D::LayersEditing::render_variable_layer_height_dialog(const GLCanvas3D& canvas) {
    if (!m_enabled)
        return;

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const Size& cnv_size = canvas.get_canvas_size();
    float         zoom   = (float) canvas.get_active_camera().get_zoom();
    const auto& p_main_toolbar = canvas.get_main_toolbar();
    float left_pos = 0.0f;
    float main_toolbar_height = 0.0f;
    if (p_main_toolbar) {
        left_pos = p_main_toolbar->get_item("layersediting")->render_rect[0];
        main_toolbar_height = canvas.main_toolbar_window_top(p_main_toolbar->get_item("layersediting")->render_rect[3]); //Orio
    }
    float x = 0.5 * cnv_size.get_width() + left_pos * zoom;

    const auto canvas_width = cnv_size.get_width();
    if (abs(GLCanvas3D::LayersEditing::s_overlay_window_width) > 0.01f) {
        if (x + GLCanvas3D::LayersEditing::s_overlay_window_width > canvas_width) {
            if (GLCanvas3D::LayersEditing::s_overlay_window_width > canvas_width)
                x = 0;
            else
                x = canvas_width - GLCanvas3D::LayersEditing::s_overlay_window_width;
        }
    }
    x = std::max(x, 0.0f);

    imgui.set_next_window_pos(x, main_toolbar_height, ImGuiCond_Always, 0.0f, 0.0f);

    imgui.push_toolbar_style(canvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * canvas.get_scale(), 4.0f * canvas.get_scale()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * canvas.get_scale(), 10.0f * canvas.get_scale()));
    // Orio: floating, collapsible window (ImGuiWrapper::begin_tool_window), its place remembered under the name of
    // the icon of the tool; default place under the icon, as above.
    imgui.begin_tool_window("toolbar_variable_layer_height", _L("Variable layer height"), wxGetApp().dark_mode() ? "toolbar_variable_layer_height_dark.svg" : "toolbar_variable_layer_height.svg",
                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    const float sliders_width = imgui.scaled(7.0f);
    const float input_box_width = 1.5 * imgui.get_slider_icon_size().x;

    if (imgui.button(_L("Adaptive")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), Event<float>(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, m_adaptive_quality));
    ImGui::SameLine();
    static float text_align = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    text_align = std::max(text_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(text_align);
    imgui.text(_L("Quality / Speed"));
    if (ImGui::IsItemHovered()) {
        //ImGui::BeginTooltip();
        //ImGui::TextUnformatted(_L("Higher print quality versus higher print speed.").ToUTF8());
        //ImGui::EndTooltip();
    }
    ImGui::SameLine();
    static float slider_align = ImGui::GetCursorPosX();
    ImGui::PushItemWidth(sliders_width);
    m_adaptive_quality = std::clamp(m_adaptive_quality, 0.0f, 1.f);
    slider_align = std::max(slider_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(slider_align);
    imgui.bbl_slider_float_style("##adaptive_slider", &m_adaptive_quality, 0.0f, 1.f, "%.2f");
    ImGui::SameLine();
    static float input_align = ImGui::GetCursorPosX();
    ImGui::PushItemWidth(input_box_width);
    input_align = std::max(input_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(input_align);
    ImGui::BBLDragFloat("##adaptive_input", &m_adaptive_quality, 0.05f, 0.0f, 0.0f, "%.2f");

    if (imgui.button(_L("Smooth")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), HeightProfileSmoothEvent(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, m_smooth_params));
    ImGui::SameLine();
    text_align = std::max(text_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(text_align);
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Radius"));
    ImGui::SameLine();
    slider_align = std::max(slider_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(slider_align);
    ImGui::PushItemWidth(sliders_width);
    int radius = (int)m_smooth_params.radius;
    int v_min = 1, v_max = 10;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.81f, 0.81f, 0.81f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.00f));
    if(ImGui::BBLSliderScalar("##radius_slider", ImGuiDataType_S32, &radius, &v_min, &v_max)){
        radius = std::clamp(radius, 1, 10);
        m_smooth_params.radius = (unsigned int)radius;
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::SameLine();
    input_align = std::max(input_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(input_align);
    ImGui::PushItemWidth(input_box_width);
    ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 0.00f));
    if (ImGui::BBLDragScalar("##radius_input", ImGuiDataType_S32, &radius, 1, &v_min, &v_max)) {
        radius = std::clamp(radius, 1, 10);
        m_smooth_params.radius = (unsigned int)radius;
    }
    ImGui::PopStyleColor(3);

    imgui.bbl_checkbox("##keep_min", m_smooth_params.keep_min);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Keep min"));

    // Orio: the other objects hidden while this one is edited (hides()), as in the variable speed tool.
    if (imgui.bbl_checkbox("##layers_hide_other_objects", m_hide_other_objects)) {
        if (auto* app_config = wxGetApp().app_config; app_config != nullptr)
            app_config->set("layers_editing", "hide_other_objects", m_hide_other_objects);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Hide the other objects"));

    ImGui::Separator();

    // Orio: under the window where it is (it can be moved).
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + ImGui::GetWindowPos().y;
    std::map<wxString, wxString> captions_texts = {
        {_L("Left mouse button:") ,_L("Add detail")},
        {_L("Right mouse button:"), _L("Remove detail")},
        {_L("Shift + Left mouse button:"),_L("Reset to base")},
        {_L("Shift + Right mouse button:"), _L("Smoothing")},
        {_L("Mouse wheel:"), _L("Increase/decrease edit area")},
        // Orio: the same anywhere in the scene, without going to the bar.
        {_L("Ctrl + Mouse wheel:"), _L("Increase/decrease edit area")},
        // Orio: also on the part, at the height of the point clicked.
        {_L("On the part:"), _L("Same buttons as on the bar")},
        // Orio: the bounds the painting does not cross.
        {_L("Right click on the graduations:"), _L("Place or delete a bound")}
    };
    show_tooltip_information(canvas, captions_texts, ImGui::GetWindowPos().x, get_cur_y);
    if (imgui.button(_L("Reset")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), SimpleEvent(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE));

    GLCanvas3D::LayersEditing::s_overlay_window_width = ImGui::GetWindowSize().x;
    imgui.end_tool_window();
    ImGui::PopStyleVar(2);
    imgui.pop_toolbar_style();
}

void GLCanvas3D::LayersEditing::render_overlay(const GLCanvas3D& canvas)
{
    // Orio: the bar spans the height of the object, in a framed panel at the right of the scene (PaintAxisBar,
    // as the variable speed tool), whose background goes under it.
    m_axis_bar.set_height(m_object_max_z);
    render_variable_layer_height_dialog(canvas);
    if (! m_enabled)
        return;
    m_axis_bar.render_panel_background(canvas);
    render_background_texture(canvas);
    const Rect& bar_rect = get_bar_rect_viewport(canvas);
    render_curve(bar_rect);
    m_axis_bar.render_panel(canvas, _u8L("Variable layer height") + " \xC2\xB7 Z", false, false);
    // The layer height under the mouse goes beside the panel, where it hides nothing (see get_tooltip()).
    m_axis_bar.render_value_tip(canvas, cursor_value_text(canvas));
    // Orio: the bounds, over the bar and its graduations.
    render_bounds(canvas);
}

float GLCanvas3D::LayersEditing::cursor_coordinate(const GLCanvas3D& canvas) const
{
    const float relative = m_axis_bar.cursor_relative(canvas);
    if (relative != -1000.0f)
        return m_object_max_z * relative;
    // Orio: over the part, where a click would paint.
    const Vec2d mouse   = canvas.get_local_mouse_position();
    const float on_part = part_coordinate_at(canvas, (float)mouse.x(), (float)mouse.y());
    return std::isnan(on_part) ? -1000.0f : on_part;
}

bool GLCanvas3D::LayersEditing::bar_rect_contains(const GLCanvas3D& canvas, float x, float y) const
{
    // The bar, or beside its ends in its panel: a stroke may start just past them.
    return m_axis_bar.contains(canvas, x, y);
}

Rect GLCanvas3D::LayersEditing::get_bar_rect_screen(const GLCanvas3D& canvas) const
{
    return m_axis_bar.rect_screen(canvas);
}

Rect GLCanvas3D::LayersEditing::get_bar_rect_viewport(const GLCanvas3D& canvas) const
{
    return m_axis_bar.rect_viewport(canvas);
}

float GLCanvas3D::LayersEditing::edit_coordinate_at(const GLCanvas3D& canvas, float x, float y) const
{
    // Orio: a stroke started on the part follows the part, and pauses while the mouse is off it (NaN).
    if (this->part_stroke)
        return part_coordinate_at(canvas, x, y);
    // Brought onto the bar: a stroke beside its ends, or beyond them, paints its ends.
    return m_object_max_z * m_axis_bar.paintable(m_axis_bar.relative_along(canvas, x, y));
}

void GLCanvas3D::LayersEditing::update_part_raycasters() const
{
    size_t i    = 0;
    bool   same = true;
    if (m_model_object != nullptr)
        for (const ModelVolume* volume : m_model_object->volumes)
            if (volume->is_model_part()) {
                same = same && i < m_part_raycasters.size() && m_part_raycasters[i].volume == volume &&
                       m_part_raycasters[i].mesh == volume->get_mesh_shared_ptr();
                ++i;
            }
    if (same && i == m_part_raycasters.size())
        return;
    m_part_raycasters.clear();
    if (m_model_object != nullptr)
        for (const ModelVolume* volume : m_model_object->volumes)
            if (volume->is_model_part())
                m_part_raycasters.push_back({ volume, volume->get_mesh_shared_ptr(), std::make_shared<MeshRaycaster>(volume->mesh()) });
}

float GLCanvas3D::LayersEditing::part_coordinate_at(const GLCanvas3D& canvas, float x, float y) const
{
    float ret = std::numeric_limits<float>::quiet_NaN();
    update_part_raycasters();
    const int instance_idx = canvas.get_selection().get_instance_idx();
    if (m_model_object == nullptr || instance_idx < 0 || instance_idx >= (int)m_model_object->instances.size() || m_object_max_z <= 0.0f)
        return ret;
    const Camera&     camera   = canvas.get_active_camera();
    const Transform3d instance = m_model_object->instances[instance_idx]->get_matrix();
    // The nearest of the hits on the solid parts; its height in the scene, as the object is colored.
    double nearest = std::numeric_limits<double>::max();
    for (const PartRaycaster& part : m_part_raycasters) {
        const Transform3d matrix = instance * part.volume->get_matrix();
        Vec3f position, normal;
        if (! part.raycaster->unproject_on_mesh(Vec2d(x, y), matrix, camera, position, normal))
            continue;
        const Vec3d  world = matrix * position.cast<double>();
        const double depth = (world - camera.get_position()).dot(camera.get_dir_forward());
        if (depth >= nearest)
            continue;
        nearest = depth;
        ret     = std::clamp((float)world.z(), 0.0f, m_object_max_z);
    }
    return ret;
}

float GLCanvas3D::LayersEditing::right_bar_width(const GLCanvas3D& canvas) const
{
    return (m_enabled && this->last_object_id >= 0) ? m_axis_bar.reserved_width(canvas) : 0.0f;
}

bool GLCanvas3D::LayersEditing::hides(const GLVolume& volume) const
{
    return m_enabled && m_hide_other_objects && this->last_object_id >= 0 && volume.composite_id.object_id != this->last_object_id;
}

// Orio: bounds the painting does not cross (11/10), at fixed heights. Between two bounds, and from the ends of
// the bar, mm.
static constexpr double LAYER_BOUND_MIN_GAP = 0.05;

std::vector<double> GLCanvas3D::LayersEditing::active_bounds() const
{
    if (m_bound_drag >= 0)
        return m_drag_bounds;
    std::vector<double> ret;
    if (m_model_object == nullptr)
        return ret;
    for (const double bound : m_model_object->layer_height_bounds)
        if (bound > LAYER_BOUND_MIN_GAP && bound < (double)m_object_max_z - LAYER_BOUND_MIN_GAP)
            ret.push_back(bound);
    std::sort(ret.begin(), ret.end());
    return ret;
}

void GLCanvas3D::LayersEditing::set_bounds(std::vector<double> bounds, const std::string& snapshot_name)
{
    std::sort(bounds.begin(), bounds.end());
    if (m_model_object == nullptr || m_model_object->layer_height_bounds == bounds)
        return;
    // Only the editing reads them: nothing to slice again.
    wxGetApp().plater()->take_snapshot(snapshot_name);
    const_cast<ModelObject*>(m_model_object)->layer_height_bounds = std::move(bounds);
}

void GLCanvas3D::LayersEditing::render_bounds(const GLCanvas3D& canvas)
{
    const double length = (double)m_object_max_z;
    if (! m_enabled || length <= 0. || this->last_object_id < 0 || m_model_object == nullptr)
        return;
    const Rect   bar   = m_axis_bar.rect_screen(canvas);
    const float  size  = PaintAxisBar::thickness(canvas); // the width of the bar
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // Over the graduations, which an ImGui window of the panel covers: the scene does not get these clicks. Not while the
    // menu is open: a click on it is not for a bound under it.
    const bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && ! ImGui::IsPopupOpen("layer_height_bounds_menu") &&
        bar.get_left() - 1.2f * size <= mouse.x && mouse.x < bar.get_left() && bar.get_top() <= mouse.y && mouse.y <= bar.get_bottom();
    // A height on the screen, and the height under the mouse, to 0.01 mm (a bound stays at a fixed height).
    auto screen_of = [&](double z) { return bar.get_bottom() - float(z / length) * (bar.get_bottom() - bar.get_top()); };
    auto height_at_mouse = [&]() {
        return std::round(100. * length * std::clamp(m_axis_bar.relative_along(canvas, mouse.x, mouse.y), 0.0f, 1.0f)) / 100.;
    };

    std::vector<double> bounds = active_bounds();
    // The bound under the mouse.
    int hovered = -1;
    if (over && m_bound_drag < 0) {
        float best = 0.15f * size;
        for (size_t i = 0; i < bounds.size(); ++i)
            if (const float distance = std::abs(screen_of(bounds[i]) - mouse.y); distance <= best) {
                best    = distance;
                hovered = (int)i;
            }
    }
    // Dragging one, between its neighbours; one undo step when it is released.
    if (m_bound_drag >= (int)bounds.size())
        m_bound_drag = -1;
    if (m_bound_drag >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const double low  = (m_bound_drag > 0 ? bounds[m_bound_drag - 1] : 0.) + LAYER_BOUND_MIN_GAP;
            const double high = (m_bound_drag + 1 < (int)bounds.size() ? bounds[m_bound_drag + 1] : length) - LAYER_BOUND_MIN_GAP;
            const double z    = height_at_mouse();
            if (z >= low && z <= high)
                m_drag_bounds[m_bound_drag] = z;
        } else {
            m_bound_drag = -1;
            set_bounds(m_drag_bounds, "Variable layer height - Move bound");
        }
    } else if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_drag_bounds = bounds;
        m_bound_drag  = hovered;
    }
    // The menu, by a right click over the graduations.
    if (over && m_bound_drag < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_bounds_menu_coordinate = height_at_mouse();
        m_bounds_menu_bound      = hovered;
        ImGui::OpenPopup("layer_height_bounds_menu");
    }
    if (ImGui::BeginPopup("layer_height_bounds_menu")) {
        const double z = m_bounds_menu_coordinate;
        // Not on another one, nor at an end of the bar.
        const bool room = z > LAYER_BOUND_MIN_GAP && z < length - LAYER_BOUND_MIN_GAP &&
            std::none_of(bounds.begin(), bounds.end(), [z](double bound) { return std::abs(bound - z) < LAYER_BOUND_MIN_GAP; });
        if (ImGui::MenuItem(_u8L("Place a bound").c_str(), nullptr, false, room)) {
            bounds.push_back(z);
            set_bounds(bounds, "Variable layer height - Add bound");
        } else if (m_bounds_menu_bound >= 0 && m_bounds_menu_bound < (int)bounds.size() && ImGui::MenuItem(_u8L("Delete the bound").c_str())) {
            bounds.erase(bounds.begin() + m_bounds_menu_bound);
            set_bounds(bounds, "Variable layer height - Delete bound");
        } else if (! bounds.empty() && ImGui::MenuItem(_u8L("Delete all the bounds").c_str()))
            set_bounds({}, "Variable layer height - Delete bounds");
        ImGui::EndPopup();
    }

    // Drawn over the bar drawn in GL, under the ImGui windows: a line across the bar, and a tab over the graduations.
    bounds = active_bounds();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const ImU32 color     = ImGui::GetColorU32(ImGuiWrapper::COL_ORANGE_LIGHT);
    const float tab_long  = std::round(0.3f * size);
    const float tab_half  = std::max(2.0f, std::round(0.09f * size));
    for (size_t i = 0; i < bounds.size(); ++i) {
        const float p     = std::round(screen_of(bounds[i]));
        const float width = ((int)i == hovered || (int)i == m_bound_drag) ? std::max(2.0f, std::round(0.07f * size)) : std::max(1.0f, std::round(0.035f * size));
        draw_list->AddLine(ImVec2(bar.get_left(), p), ImVec2(bar.get_right(), p), color, width);
        draw_list->AddRectFilled(ImVec2(bar.get_left() - tab_long, p - tab_half), ImVec2(bar.get_left(), p + tab_half), color, 0.5f * tab_half);
    }
    // The bound under the mouse or dragged: its height and the layer there now (the profile moves the layers, not the
    // bound). Over everything, as the graduations are under it.
    const int labelled = (m_bound_drag >= 0) ? m_bound_drag : hovered;
    if (labelled >= 0 && labelled < (int)bounds.size()) {
        const double z = bounds[labelled];
        char         mm[64];
        snprintf(mm, sizeof(mm), "%.2f mm", z);
        std::string text = mm;
        if (m_slicing_parameters != nullptr && m_layer_height_profile.size() >= 4) {
            const std::vector<coordf_t> layers = generate_object_layers(*m_slicing_parameters, m_layer_height_profile, false);
            for (size_t i = 1; i < layers.size(); i += 2)
                if (layers[i] > z - EPSILON) {
                    text = _u8L("Layer") + " " + std::to_string(i / 2 + 1) + " \xC2\xB7 " + text;
                    break;
                }
        }
        const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
        const float  padding   = std::max(2.0f, std::round(0.08f * size));
        const float  p         = screen_of(z);
        const ImVec2 min(bar.get_left() - tab_long - 2.0f * padding - (text_size.x + 2.0f * padding), p - 0.5f * text_size.y - padding);
        const ImVec2 max(min.x + text_size.x + 2.0f * padding, min.y + text_size.y + 2.0f * padding);
        ImDrawList*  top_list = ImGui::GetForegroundDrawList();
        top_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_PopupBg), padding);
        top_list->AddRect(min, max, color, padding);
        top_list->AddText(ImVec2(min.x + padding, min.y + padding), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    }
}

bool GLCanvas3D::LayersEditing::is_initialized() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr;
}

std::string GLCanvas3D::LayersEditing::get_tooltip(const GLCanvas3D&) const
{
    // Orio: a tooltip next to the mouse would hide the bar: the panel shows the layer height beside itself
    // instead (render_overlay()).
    return std::string();
}

std::string GLCanvas3D::LayersEditing::cursor_value_text(const GLCanvas3D& canvas) const
{
    std::string ret;
    if (m_enabled && m_layer_height_profile.size() >= 4) {
        float z = cursor_coordinate(canvas);
        if (z != -1000.0f) {
            float h = 0.0f;
            for (size_t i = m_layer_height_profile.size() - 2; i >= 2; i -= 2) {
                const float zi = static_cast<float>(m_layer_height_profile[i]);
                const float zi_1 = static_cast<float>(m_layer_height_profile[i - 2]);
                if (zi_1 <= z && z <= zi) {
                    float dz = zi - zi_1;
                    h = (dz != 0.0f) ? static_cast<float>(lerp(m_layer_height_profile[i - 1], m_layer_height_profile[i + 1], (z - zi_1) / dz)) :
                        static_cast<float>(m_layer_height_profile[i + 1]);
                    break;
                }
            }
            if (h > 0.0f) {
                char text[32];
                snprintf(text, sizeof(text), "%.2f mm", h);
                ret = text;
            }
        }
    }
    return ret;
}

void GLCanvas3D::LayersEditing::render_background_texture(const GLCanvas3D& canvas)
{
    if (!m_enabled)
        return;

    const Size cnv_size = canvas.get_canvas_size();
    const int cnv_width = cnv_size.get_width();
    const int cnv_height = cnv_size.get_height();
    if (cnv_width == 0 || cnv_height == 0)
        return;

    const auto& shader = wxGetApp().get_shader("variable_layer_height");
    if (shader == nullptr)
        return;

    wxGetApp().bind_shader(shader);

    shader->set_uniform("z_to_texture_row", float(m_layers_texture.cells - 1) / (float(m_layers_texture.width) * m_object_max_z));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / (float)m_layers_texture.height);
    shader->set_uniform("z_cursor", this->cursor_coordinate(canvas));
    shader->set_uniform("z_cursor_band_width", band_width);
    shader->set_uniform("object_max_z", m_object_max_z);

    glsafe(::glActiveTexture(GL_TEXTURE0 + 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));

    // Orio: the bar in its panel at the right of the scene.
    m_axis_bar.render_background(canvas, *shader);

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    wxGetApp().unbind_shader();
}

void GLCanvas3D::LayersEditing::render_curve(const Rect & bar_rect)
{
    if (!m_enabled)
        return;

    //FIXME show some kind of legend.
    if (!m_slicing_parameters)
        return;

    const auto& shader = wxGetApp().get_shader("flat");
    if (!shader) {
        return;
    }

    wxGetApp().bind_shader(shader);
    // Make the vertical bar a bit wider so the layer height curve does not touch the edge of the bar region.
    const float scale_x = bar_rect.get_width() / float(1.12 * m_slicing_parameters->max_layer_height);
    const float scale_y = bar_rect.get_height() / m_object_max_z;
    const float x = bar_rect.get_left() + float(m_slicing_parameters->layer_height) * scale_x;

    const Camera &camera    = m_canvas.get_active_camera();
    Transform3d view_matrix = camera.get_view_matrix_for_billboard();

    Transform3d model_matrix{ Transform3d::Identity() };
    model_matrix.data()[3 * 4 + 0] = x;
    model_matrix.data()[3 * 4 + 1] = (bar_rect.get_top() + bar_rect.get_bottom()) * 0.5f;
    model_matrix.data()[0 * 4 + 0] = 0.0f;
    model_matrix.data()[1 * 4 + 1] = (bar_rect.get_top() - bar_rect.get_bottom());

    const auto& projection_matrix = camera.get_projection_matrix();
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    shader->set_uniform("projection_matrix", projection_matrix);

    if (!m_baseline.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, -0.5f, 0.0f));
        init_data.add_vertex(Vec3f(0.0f, 0.5f, 0.0f));

        // indices
        init_data.add_line(0, 1);

        m_baseline.init_from(std::move(init_data));
    }
    m_baseline.set_color({ 0.0f, 0.0f, 0.0f , 1.0f});
    m_baseline.render_geometry();

    // Curve

    Transform3d model_matrix_curve{ Transform3d::Identity() };
    model_matrix_curve.data()[3 * 4 + 0] = bar_rect.get_left();
    model_matrix_curve.data()[3 * 4 + 1] = bar_rect.get_bottom();
    model_matrix_curve.data()[0 * 4 + 0] = scale_x;
    model_matrix_curve.data()[1 * 4 + 1] = scale_y;
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix_curve);

    if (!m_profile_curve.is_initialized() || m_profile_dirty) {
        m_profile_curve.reset();
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(m_layer_height_profile.size() / 2);
        init_data.reserve_indices(m_layer_height_profile.size() / 2);

        // vertices
        for (unsigned int i = 0; i < m_layer_height_profile.size(); i += 2) {
            init_data.add_vertex(Vec3f((float)m_layer_height_profile[i + 1], (float)m_layer_height_profile[i], 0.0f));
            init_data.add_index(i / 2);
        }

        m_profile_curve.init_from(std::move(init_data));

        m_profile_dirty = false;
    }
    m_profile_curve.set_color({ 0.0f, 0.0f, 1.0f , 1.0f });
    m_profile_curve.render_geometry();

    wxGetApp().unbind_shader();
}

void GLCanvas3D::LayersEditing::render_volumes(const GLCanvas3D & canvas, const GLVolumeCollection & volumes)//render volume and layer height texture (has mapping relation with each other)
{
    assert(this->is_allowed());
    assert(this->last_object_id != -1);

    const auto& current_shader = wxGetApp().get_current_shader();
    ScopeGuard guard([current_shader]() { if (current_shader != nullptr) wxGetApp().bind_shader(current_shader); });
    if (current_shader != nullptr)
        wxGetApp().unbind_shader();

    const auto& shader = wxGetApp().get_shader("variable_layer_height");
    if (shader == nullptr)
        return;

    wxGetApp().bind_shader(shader);

    generate_layer_height_texture();

    // Uniforms were resolved, go ahead using the layer editing shader.
    shader->set_uniform("z_to_texture_row", float(m_layers_texture.cells - 1) / (float(m_layers_texture.width) * float(m_object_max_z)));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / float(m_layers_texture.height));
    shader->set_uniform("z_cursor", this->cursor_coordinate(canvas));
    shader->set_uniform("z_cursor_band_width", float(this->band_width));

    const Camera &camera    = m_canvas.get_active_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();
    shader->set_uniform("projection_matrix", projection_matrix);

    // Initialize the layer height texture mapping.
    const GLsizei w = (GLsizei)m_layers_texture.width;
    const GLsizei h = (GLsizei)m_layers_texture.height;
    const GLsizei half_w = w / 2;
    const GLsizei half_h = h / 2;
    glsafe(::glActiveTexture(GL_TEXTURE0 + 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, half_w, half_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data()));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, half_w, half_h, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data() + m_layers_texture.width * m_layers_texture.height * 4));
    std::vector<std::array<float, 4>> colors = m_canvas.get_active_colors();
    for (GLVolume* glvolume : volumes.volumes) {
        // Render the object using the layer editing shader and texture.
        if (!glvolume->is_active || glvolume->composite_id.object_id != this->last_object_id || glvolume->is_modifier)
            continue;

        shader->set_uniform("volume_world_matrix", glvolume->world_matrix());
        shader->set_uniform("object_max_z", 0.0f);

        const Transform3d view_model_matrix = view_matrix * glvolume->world_matrix();
        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());

        glvolume->render(camera, colors, m_canvas.get_ref_model());
    }
    // Revert back to the previous shader.
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLCanvas3D::LayersEditing::adjust_layer_height_profile()
{
    this->update_slicing_parameters();
    bool nozzle_range_reset = false;
    //Orio: [256] every reset of the profile warns, not only the nozzle range one.
    std::string reset_message;
    PrintObject::update_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile, nozzle_range_reset, nullptr, &reset_message);
    if (! reset_message.empty())
        wxGetApp().plater()->get_notification_manager()->push_plater_warning_notification(reset_message);
    const std::vector<double> before = m_layer_height_profile;
    Slic3r::adjust_layer_height_profile(*m_slicing_parameters, m_layer_height_profile, this->last_z, this->strength, this->band_width, this->last_action);
    // Orio: what the stroke changed stops at the bounds around it; a bound under the mouse goes with its lower side.
    if (const std::vector<double> bounds = active_bounds(); ! bounds.empty() && before.size() >= 4 && m_layer_height_profile != before) {
        double lo = 0., hi = before[before.size() - 2];
        for (const double bound : bounds) {
            if (bound < (double)this->last_z)
                lo = bound;
            else {
                hi = bound;
                break;
            }
        }
        m_layer_height_profile = Slic3r::layer_height_clip_stroke(before, m_layer_height_profile, lo, hi);
    }
    m_layers_texture.valid = false;
    m_profile_dirty = true;
}

void GLCanvas3D::LayersEditing::reset_layer_height_profile(GLCanvas3D & canvas)
{
    const_cast<ModelObject*>(m_model_object)->layer_height_profile.clear();
    m_layer_height_profile.clear();
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(last_object_id);
    m_profile_dirty = true;
}

void GLCanvas3D::LayersEditing::adaptive_layer_height_profile(GLCanvas3D & canvas, float quality_factor)
{
    this->update_slicing_parameters();
    m_layer_height_profile = layer_height_profile_adaptive(*m_slicing_parameters, *m_model_object, quality_factor);
    const_cast<ModelObject*>(m_model_object)->layer_height_profile.set(m_layer_height_profile);
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(last_object_id);
    m_profile_dirty = true;
}

void GLCanvas3D::LayersEditing::smooth_layer_height_profile(GLCanvas3D & canvas, const HeightProfileSmoothingParams & smoothing_params)
{
    this->update_slicing_parameters();
    // Orio: each part between the bounds on its own: the steps at the bounds stay.
    m_layer_height_profile = smooth_height_profile_bounded(m_layer_height_profile, active_bounds(), *m_slicing_parameters, smoothing_params);
    const_cast<ModelObject*>(m_model_object)->layer_height_profile.set(m_layer_height_profile);
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(last_object_id);
    m_profile_dirty = true;
}

void GLCanvas3D::LayersEditing::generate_layer_height_texture()
{
    this->update_slicing_parameters();
    // Always try to update the layer height profile.
    bool update = !m_layers_texture.valid;
    bool nozzle_range_reset = false;
    //Orio: [256] every reset of the profile warns, not only the nozzle range one.
    std::string reset_message;
    if (PrintObject::update_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile, nozzle_range_reset, nullptr, &reset_message)) {
        // Initialized to the default value.
        update = true;
        m_profile_dirty = true;
    }
    if (! reset_message.empty())
        wxGetApp().plater()->get_notification_manager()->push_plater_warning_notification(reset_message);
    // Update if the layer height profile was changed, or when the texture is not valid.
    if (!update && !m_layers_texture.data.empty() && m_layers_texture.cells > 0)
        // Texture is valid, don't update.
        return;

    if (m_layers_texture.data.empty()) {
        m_layers_texture.width = 1024;
        m_layers_texture.height = 1024;
        m_layers_texture.levels = 2;
        m_layers_texture.data.assign(m_layers_texture.width * m_layers_texture.height * 5, 0);
    }

    bool level_of_detail_2nd_level = true;
    m_layers_texture.cells = Slic3r::generate_layer_height_texture(
        *m_slicing_parameters,
        Slic3r::generate_object_layers(*m_slicing_parameters, m_layer_height_profile, false),
        m_layers_texture.data.data(), m_layers_texture.height, m_layers_texture.width, level_of_detail_2nd_level);
    m_layers_texture.valid = true;
}

void GLCanvas3D::LayersEditing::accept_changes(GLCanvas3D & canvas)
{
    if (last_object_id >= 0) {
        wxGetApp().plater()->take_snapshot("Variable layer height - Manual edit");
        const_cast<ModelObject*>(m_model_object)->layer_height_profile.set(m_layer_height_profile);
        canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        wxGetApp().obj_list()->update_info_items(last_object_id);
    }
}

void GLCanvas3D::LayersEditing::update_slicing_parameters()
{
    if (m_slicing_parameters == nullptr) {
        m_slicing_parameters = new SlicingParameters();
        *m_slicing_parameters = PrintObject::slicing_parameters(*m_config, *m_model_object, m_object_max_z);
    }
}

Size::Size()
    : m_width(0)
    , m_height(0)
{
}

Size::Size(int width, int height, float scale_factor)
    : m_width(width)
    , m_height(height)
    , m_scale_factor(scale_factor)
{
}

int Size::get_width() const
{
    return m_width;
}

void Size::set_width(int width)
{
    m_width = width;
}

int Size::get_height() const
{
    return m_height;
}

void Size::set_height(int height)
{
    m_height = height;
}

int Size::get_scale_factor() const
{
    return m_scale_factor;
}

void Size::set_scale_factor(int scale_factor)
{
    m_scale_factor = scale_factor;
}

const Point GLCanvas3D::Mouse::Drag::Invalid_2D_Point(INT_MAX, INT_MAX);
const Vec3d GLCanvas3D::Mouse::Drag::Invalid_3D_Point(DBL_MAX, DBL_MAX, DBL_MAX);
const int GLCanvas3D::Mouse::Drag::MoveThresholdPx = 5;

GLCanvas3D::Mouse::Drag::Drag()
    : start_position_2D(Invalid_2D_Point)
    , start_position_3D(Invalid_3D_Point)
    , move_volume_idx(-1)
    , move_requires_threshold(false)
    , move_start_threshold_position_2D(Invalid_2D_Point)
{
}

GLCanvas3D::Mouse::Mouse()
    : dragging(false)
    , position(DBL_MAX, DBL_MAX)
    , scene_position(DBL_MAX, DBL_MAX, DBL_MAX)
    , ignore_left_up(false)
    , ignore_right_up(false)
{
}

void GLCanvas3D::Labels::render(const std::vector<const ModelInstance*>& sorted_instances) const
{
    if (!m_enabled || m_canvas.get_gizmos_manager().is_running()) {
        return;
    }
    if ((!m_show_layer_labels && !m_show_object_labels) )
        return;

    // Lambda to check print sequence of current plate
    auto get_plate_print_sequence = [&]() -> PrintSequence {
        PartPlate* cur_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        if (cur_plate) {
            PrintSequence plate_seq = cur_plate->get_print_seq();
            if (plate_seq != PrintSequence::ByDefault) {
                return plate_seq;
            }
            // If plate uses default, check global print sequence
            return wxGetApp().global_print_sequence();
        }
        return PrintSequence::ByLayer; // Default fallback
    };
    PrintSequence seq = get_plate_print_sequence();
    // Check if current plate follows global/layer sequence
    bool is_layer_seq = (seq == PrintSequence::ByLayer || seq == PrintSequence::ByDefault);
    if (is_layer_seq && !m_show_layer_labels) {
        return;
    }
    bool is_object_seq = (seq == PrintSequence::ByObject || seq == PrintSequence::ByDefault);
    if (is_object_seq && !m_show_object_labels) {
        return;
    }

    const Camera &camera = m_canvas.get_active_camera();
    const Model* model = m_canvas.get_model();
    if (model == nullptr)
        return;

    Transform3d world_to_eye = camera.get_view_matrix();
    const std::array<int, 4>& viewport = camera.get_viewport();
    struct Owner
    {
        int obj_idx;
        int inst_idx;
        size_t model_instance_id;
        BoundingBoxf3 world_box;
        double eye_center_z;
        std::string title;
        std::string label;
        std::string print_order;
        bool selected;
    };

    // collect owners world bounding boxes and data from volumes
    std::vector<Owner> owners;
    const GLVolumeCollection& volumes = m_canvas.get_volumes();
    PartPlate* cur_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    for (const GLVolume* volume : volumes.volumes) {
        int obj_idx = volume->object_idx();
        if (0 <= obj_idx && obj_idx < (int)model->objects.size()) {
            int inst_idx = volume->instance_idx();
            //only show current plate's label
            if (!cur_plate->contain_instance(obj_idx, inst_idx))
                continue;
            std::vector<Owner>::iterator it = std::find_if(owners.begin(), owners.end(), [obj_idx, inst_idx](const Owner& owner) {
                return (owner.obj_idx == obj_idx) && (owner.inst_idx == inst_idx);
                });
            if (it != owners.end()) {
                it->world_box.merge(volume->transformed_bounding_box());
                it->selected &= volume->selected;
            } else {
                const ModelObject* model_object = model->objects[obj_idx];
                Owner owner;
                owner.obj_idx = obj_idx;
                owner.inst_idx = inst_idx;
                owner.model_instance_id = model_object->instances[inst_idx]->id().id;
                owner.world_box = volume->transformed_bounding_box();
                owner.title = "object" + std::to_string(obj_idx) + "_inst##" + std::to_string(inst_idx);
                owner.label = model_object->name;
                if (model_object->instances.size() > 1)
                    owner.label += " (" + std::to_string(inst_idx + 1) + ")";
                owner.selected = volume->selected;
                owners.emplace_back(owner);
            }
        }
    }

    // updates print order strings
    if (sorted_instances.size() > 0) {
        for (size_t i = 0; i < sorted_instances.size(); ++i) {
            size_t id = sorted_instances[i]->id().id;
            std::vector<Owner>::iterator it = std::find_if(owners.begin(), owners.end(), [id](const Owner& owner) {
                return owner.model_instance_id == id;
                });
            if (it != owners.end())
                //it->print_order = std::string((_(L("Sequence"))).ToUTF8()) + "#: " + std::to_string(i + 1);
                it->print_order = std::string((_(L("Sequence"))).ToUTF8()) + "#: " + std::to_string(sorted_instances[i]->arrange_order);
        }
    }

    // calculate eye bounding boxes center zs
    for (Owner& owner : owners) {
        owner.eye_center_z = (world_to_eye * owner.world_box.center())(2);
    }

    // sort owners by center eye zs and selection
    std::sort(owners.begin(), owners.end(), [](const Owner& owner1, const Owner& owner2) {
        if (!owner1.selected && owner2.selected)
            return true;
        else if (owner1.selected && !owner2.selected)
            return false;
        else
            return (owner1.eye_center_z < owner2.eye_center_z);
        });

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    // render info windows
    Matrix4d world_to_screen = camera.get_projection_matrix().matrix() * world_to_eye.matrix();
    for (const Owner& owner : owners) {
        Vec4d temp_center(owner.world_box.center().x(), owner.world_box.center().y(), owner.world_box.center().z(), 1.0);
        Vec4d temp_ndc = world_to_screen * temp_center;
        Vec3d screen_box_center = Vec3d(temp_ndc.x(), temp_ndc.y(), temp_ndc.z()) / temp_ndc.w();

        float x = 0.5f * (1 + screen_box_center(0)) * viewport[2];
        float y  = 0.5f * (1 - screen_box_center(1)) * viewport[3];

        if (x < 0.0f || viewport[2] < x || y < 0.0f || viewport[3] < y)
            continue;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, owner.selected ? 3.0f : 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, owner.selected ? ImVec4(1/255.f, 174/255.f, 66/255.f, 1.0f) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
        imgui.set_next_window_pos(x, y, ImGuiCond_Always, 0.5f, 0.5f);
        imgui.begin(owner.title, ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::AlignTextToFramePadding();

        bool show_object_label = m_show_object_labels && !owner.print_order.empty();

        imgui.text(owner.label);

        if (show_object_label) {
            ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.3f));
            ImGui::Separator();
            ImGui::PopStyleColor();
            ImGui::AlignTextToFramePadding();
            imgui.text(owner.print_order);
        }

        // force re-render while the windows gets to its final size (it takes several frames)
        if (ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
            imgui.set_requires_extra_frame();

        imgui.end();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

void GLCanvas3D::Tooltip::set_text(const std::string& text)
{
    // If the mouse is inside an ImGUI dialog, then the tooltip is suppressed.
    m_text = m_in_imgui ? std::string() : text;
}

void GLCanvas3D::Tooltip::render(const Vec2d& mouse_position, GLCanvas3D& canvas)
{
    static ImVec2 size(0.0f, 0.0f);

    auto validate_position = [](const Vec2d& position, const GLCanvas3D& canvas, const ImVec2& wnd_size) {
        const Size cnv_size = canvas.get_canvas_size();
        const float x = std::clamp((float)position.x(), 0.0f, (float)cnv_size.get_width() - wnd_size.x);
        const float y = std::clamp((float)position.y() + 16.0f, 0.0f, (float)cnv_size.get_height() - wnd_size.y);
        return Vec2f(x, y);
    };

    if (m_text.empty()) {
        m_start_time = std::chrono::steady_clock::now();
        return;
    }

    // draw the tooltip as hidden until the delay is expired
    // use a value of alpha slightly different from 0.0f because newer imgui does not calculate properly the window size if alpha == 0.0f
    const float alpha = (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start_time).count() < 500) ? 0.01f : 1.0f;

    const Vec2f position = validate_position(mouse_position, canvas, size);

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    imgui.set_next_window_pos(position.x(), position.y(), ImGuiCond_Always, 0.0f, 0.0f);

    imgui.begin(wxString("canvas_tooltip"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::TextUnformatted(m_text.c_str());

    // force re-render while the windows gets to its final size (it may take several frames) or while hidden
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    if (alpha < 1.0f || ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
        imgui.set_requires_extra_frame();
#else
    if (alpha < 1.0f || ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
        canvas.request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

    size = ImGui::GetWindowSize();

    imgui.end();
    ImGui::PopStyleVar(2);
}

//BBS: add height limit logic

GLCanvas3D::SequentialPrintClearance::~SequentialPrintClearance()
{
    reset();
}

void GLCanvas3D::SequentialPrintClearance::set_polygons(const Polygons& polygons, const std::vector<std::pair<Polygon, float>>& height_polygons)
{
    //BBS: add height limit logic
    m_height_limit.reset();
    m_perimeter.reset();
    m_fill.reset();
    if (!polygons.empty()) {
        size_t triangles_count = 0;
        for (const Polygon &poly : polygons) { triangles_count += poly.points.size() - 2; }
        const size_t vertices_count = 3 * triangles_count;

        if (m_render_fill) {
            GLModel::Geometry fill_data;
            fill_data.format = {GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3};
            fill_data.color  = {0.8f, 0.8f, 1.0f, 0.5f};

            // vertices + indices
            const ExPolygons polygons_union   = union_ex(polygons);
            unsigned int     vertices_counter = 0;
            for (const ExPolygon &poly : polygons_union) {
                const std::vector<Vec3d> triangulation = triangulate_expolygon_3d(poly);
                fill_data.reserve_vertices(fill_data.vertices_count() + triangulation.size());
                fill_data.reserve_indices(fill_data.indices_count() + triangulation.size());
                for (const Vec3d &v : triangulation) {
                    fill_data.add_vertex((Vec3f) (v.cast<float>() + 0.0125f * Vec3f::UnitZ())); // add a small positive z to avoid z-fighting
                    ++vertices_counter;
                    if (vertices_counter % 3 == 0)
                        fill_data.add_triangle(vertices_counter - 3, vertices_counter - 2, vertices_counter - 1);
                }
            }
            m_fill.init_from(std::move(fill_data));

        }

        m_perimeter.init_model_from_polygon(polygons, 0.025f);
    }

    //BBS: add the height limit compute logic
    if (!height_polygons.empty()) {
        size_t height_triangles_count = 0;
        for (const auto &poly : height_polygons) { height_triangles_count += poly.first.points.size() - 2; }
        const size_t height_vertices_count = 3 * height_triangles_count;

        GLModel::InitializationData         height_fill_data;
        GLModel::InitializationData::Entity height_entity;
        height_entity.type  = GLModel::PrimitiveType::Triangles;
        height_entity.color = {0.8f, 0.8f, 1.0f, 0.5f};
        height_entity.positions.reserve(height_vertices_count);
        height_entity.normals.reserve(height_vertices_count);
        height_entity.indices.reserve(height_vertices_count);

        for (const auto &poly : height_polygons) {
            ExPolygon                ex_poly(poly.first);
            const std::vector<Vec3d> height_triangulation = triangulate_expolygon_3d(ex_poly);
            for (const Vec3d &v : height_triangulation) {
                Vec3d point{v.x(), v.y(), poly.second};
                height_entity.positions.emplace_back(point.cast<float>());
                height_entity.normals.emplace_back(Vec3f::UnitZ());
                const size_t height_positions_count = height_entity.positions.size();
                if (height_positions_count % 3 == 0) {
                    height_entity.indices.emplace_back(height_positions_count - 3);
                    height_entity.indices.emplace_back(height_positions_count - 2);
                    height_entity.indices.emplace_back(height_positions_count - 1);
                }
            }
        }

        height_fill_data.entities.emplace_back(height_entity);
        m_height_limit.init_from(height_fill_data);
    }
}

void GLCanvas3D::SequentialPrintClearance::render()
{
    std::array<float, 4> FILL_COLOR = { 0.7f, 0.7f, 1.0f, 0.5f };
    std::array<float, 4> NO_FILL_COLOR = { 0.75f, 0.75f, 0.75f, 0.75f };

    const auto& shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    wxGetApp().bind_shader(shader);

    const Camera &camera = m_canvas.get_active_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    m_perimeter.set_color(-1, m_render_fill ? FILL_COLOR : NO_FILL_COLOR);
    m_perimeter.render_geometry();
    m_fill.render_geometry();
    //BBS: add height limit
    m_height_limit.set_color(-1, m_render_fill ? FILL_COLOR : NO_FILL_COLOR);
    m_height_limit.render_geometry();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_DEPTH_TEST));

    wxGetApp().unbind_shader();
}

void GLCanvas3D::SequentialPrintClearance::reset()
{
    m_height_limit.reset();
    m_fill.reset();
    m_perimeter.reset();
}

wxDEFINE_EVENT(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_OBJECT_SELECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_PLATE_NAME_CHANGE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_MOVE_PLATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_PLATE_SELECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RIGHT_CLICK, RBtnEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_PLATE_RIGHT_CLICK, RBtnPlateEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_REMOVE_OBJECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ARRANGE, SimpleEvent);
//BBS: add arrange and orient event
wxDEFINE_EVENT(EVT_GLCANVAS_ARRANGE_PARTPLATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ARRANGE_OUTPLATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ORIENT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ORIENT_PARTPLATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SELECT_CURR_PLATE_ALL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SELECT_ALL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_QUESTION_MARK, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INCREASE_INSTANCES, Event<int>);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_MOVED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_ROTATED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_SCALED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_FORCE_UPDATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, Event<bool>);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE_GEOMETRY, Vec3dsEvent<2>);
wxDEFINE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_STARTED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE_BED_SHAPE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_TAB, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RESETGIZMOS, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_MOVE_SLIDERS, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_EDIT_COLOR_CHANGE, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_JUMP_TO, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UNDO, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_REDO, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SWITCH_TO_OBJECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SWITCH_TO_GLOBAL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_COLLAPSE_SIDEBAR, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RELOAD_FROM_DISK, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RENDER_TIMER, wxTimerEvent/*RenderTimerEvent*/);
wxDEFINE_EVENT(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE, SimpleEvent);
wxDEFINE_EVENT(EVT_CUSTOMEVT_TICKSCHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, Event<float>);
wxDEFINE_EVENT(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, HeightProfileSmoothEvent);
//BBS: variable print speed along Z.
wxDEFINE_EVENT(EVT_GLCANVAS_RESET_SPEED_PROFILE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SMOOTH_SPEED_PROFILE, HeightProfileSmoothEvent);
//Orio: variable sparse infill density / wall count along Z.
wxDEFINE_EVENT(EVT_GLCANVAS_RESET_INFILL_WALLS_PROFILE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SMOOTH_INFILL_WALLS_PROFILE, HeightProfileSmoothEvent);

const double GLCanvas3D::DefaultCameraZoomToBoxMarginFactor = 1.25;
const double GLCanvas3D::DefaultCameraZoomToBedMarginFactor = 2.00;
const double GLCanvas3D::DefaultCameraZoomToPlateMarginFactor = 1.25;

//Orio: the arrange menu saves its options in the "arrange" section of the application config: spacing
// and auto rotation per mode (suffix "_fff", "_fff_seq_print" or "_sla"), the other options shared by all the modes.
// Booleans are saved as "true" / "false" (older configs may contain "1" / "0").
static void load_arrange_bool(const std::string &key, bool &value)
{
    const std::string str = wxGetApp().app_config->get("arrange", key);
    if (!str.empty())
        value = (str == "1" || str == "true");
}

//Orio: "Align to Y axis" depends on the printer kinematics, so it is saved per printer structure
// (i3, CoreXY...). As long as the user did not change it, it follows the printer: enabled on i3 printers.
static std::string arrange_align_to_y_axis_key()
{
    auto printer_structure_opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure");
    return "align_to_y_axis_" + (printer_structure_opt ? printer_structure_opt->serialize() : std::string("undefine"));
}

static bool arrange_align_to_y_axis_default()
{
    auto printer_structure_opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure");
    return printer_structure_opt && printer_structure_opt->value == PrinterStructure::psI3;
}

void GLCanvas3D::load_arrange_settings()
{
    //Orio: read the keys written by the arrange menu (_render_arrange_menu()). Upstream read other keys
    // ("min_object_distance", "enable_rotation"...) and not the shared options, so they were all lost at restart.
    auto load_mode_settings = [](ArrangeSettings &settings, const std::string &postfix) {
        settings.postfix = postfix;
        const std::string dist_str = wxGetApp().app_config->get("arrange", "min_object_distance" + postfix);
        if (!dist_str.empty()) {
            // Saved with float_to_string_decimal_point(): independent of the locale, unlike std::stof().
            size_t       pos  = 0;
            const double dist = string_to_double_decimal_point(dist_str, &pos);
            if (pos > 0 && dist >= 0.)
                settings.distance = float(dist);
        }
        load_arrange_bool("enable_rotation" + postfix, settings.enable_rotation);
    };
    load_mode_settings(m_arrange_settings_fff, "_fff");
    load_mode_settings(m_arrange_settings_fff_seq_print, "_fff_seq_print");
    load_mode_settings(m_arrange_settings_sla, "_sla");

    //BBS: add specific arrange settings
    m_arrange_settings_fff_seq_print.is_seq_print = true;

    load_shared_arrange_settings();
}

void GLCanvas3D::load_shared_arrange_settings()
{
    bool allow_multi_materials = true, avoid_cali_region = true;
    load_arrange_bool("allow_multi_materials_on_same_plate", allow_multi_materials);
    load_arrange_bool("avoid_extrusion_cali_region", avoid_cali_region);

    bool align_to_y_axis = false;
    if (wxGetApp().preset_bundle != nullptr) { // no printer preset in the G-code viewer
        align_to_y_axis = arrange_align_to_y_axis_default();
        load_arrange_bool(arrange_align_to_y_axis_key(), align_to_y_axis);
    }

    for (ArrangeSettings *settings : { &m_arrange_settings_fff, &m_arrange_settings_fff_seq_print, &m_arrange_settings_sla }) {
        settings->allow_multi_materials_on_same_plate = allow_multi_materials;
        settings->avoid_extrusion_cali_region         = avoid_cali_region;
        // As in the arrange menu: no alignment to the Y axis with auto rotation.
        settings->align_to_y_axis                     = align_to_y_axis && !settings->enable_rotation;
    }
}

GLCanvas3D::ArrangeSettings& GLCanvas3D::get_arrange_settings()
{
    PrinterTechnology ptech = current_printer_technology();

    auto* ptr = &m_arrange_settings_fff;

    if (ptech == ptSLA) {
        ptr = &m_arrange_settings_sla;
        ptr->postfix = "_sla";
    }
    else if (ptech == ptFFF) {
        if (wxGetApp().global_print_sequence() == PrintSequence::ByObject) {
            ptr     = &m_arrange_settings_fff_seq_print;
            ptr->postfix = "_fff_seq_print";
        } else {
            ptr     = &m_arrange_settings_fff;
            ptr->postfix = "_fff";
        }
    }

    return *ptr;
}

int GLCanvas3D::GetHoverId()
{
    if (m_hover_plate_idxs.size() == 0) {
        return -1; }
    return m_hover_plate_idxs.front();

}

Vec2i GLCanvas3D::convert_world_pt_to_screen(Vec3d pos)
{
    Vec2i                     result(-1,-1);

    auto camera = get_active_camera();
    Transform3d world_to_eye    = camera.get_view_matrix();
    Matrix4d world_to_screen = camera.get_projection_matrix().matrix() * world_to_eye.matrix();
    const std::array<int, 4> &viewport        = camera.get_viewport();

    Vec4d       temp_center(pos.x(), pos.y(), pos.z(), 1.0);
    Vec4d       temp_ndc          = world_to_screen * temp_center;
    Vec3d       screen_box_center = Vec3d(temp_ndc.x(), temp_ndc.y(), temp_ndc.z()) / temp_ndc.w();

    float x = 0.5f * (1 + screen_box_center(0)) * viewport[2];
    float y = 0.5f * (1 - screen_box_center(1)) * viewport[3];

    if (x < 0.0f || viewport[2] < x || y < 0.0f || viewport[3] < y)
        return result;
    result[0] = x;
    result[1] = y;
    return result;
}

bool GLCanvas3D::is_mouse_in_screen_convex_hull(Vec2i mouse, const BoundingBoxf3 &box) {
    //convert_world_pt_to_screen
    auto &               _min = box.min;
    auto &               _max = box.max;
    std::array<Vec3d, 8> src_vertices;
    src_vertices[0] = _min;
    src_vertices[1] = Vec3d(_max.x(), _min.y(), _min.z());
    src_vertices[2] = Vec3d(_max.x(), _max.y(), _min.z());
    src_vertices[3] = Vec3d(_min.x(), _max.y(), _min.z());
    src_vertices[4] = Vec3d(_min.x(), _min.y(), _max.z());
    src_vertices[5] = Vec3d(_max.x(), _min.y(), _max.z());
    src_vertices[6] = _max;
    src_vertices[7] = Vec3d(_min.x(), _max.y(), _max.z());
    Points pts;
    for (int i = 0; i < src_vertices.size(); i++) {
        auto temp = convert_world_pt_to_screen(src_vertices[i]);
        if (temp[0] >= 0 && temp[1] >= 0) {
            pts.emplace_back(coord_t(temp.x()), coord_t(temp.y()));
        }
    }
    Polygon pg = Geometry::convex_hull(std::move(pts));
    Point   temp_mouse(mouse[0], mouse[1]);
    if (pg.contains(temp_mouse)) {
        return true;
    }
    return false;
}

PrinterTechnology GLCanvas3D::current_printer_technology() const {
    return m_process->current_printer_technology();
}

GLCanvas3D::GLCanvas3D(wxGLCanvas* canvas, Bed3D &bed)
    : m_canvas(canvas)
    , m_context(nullptr)
    , m_bed(bed)
#if ENABLE_RETINA_GL
    , m_retina_helper(nullptr)
#endif
    , m_in_render(false)
    , m_return_toolbar()
    , m_canvas_type(ECanvasType::CanvasView3D)
    , m_gizmos(*this)
    , m_use_clipping_planes(false)
    , m_sidebar_field("")
    , m_extra_frame_requested(false)
    , m_config(nullptr)
    , m_process(nullptr)
    , m_model(nullptr)
    , m_dirty(true)
    , m_initialized(false)
    , m_apply_zoom_to_volumes_filter(false)
    , m_picking_enabled(false)
    , m_moving_enabled(false)
    , m_dynamic_background_enabled(false)
    , m_multisample_allowed(false)
    , m_moving(false)
    , m_tab_down(false)
    , m_cursor_type(Standard)
    , m_color_by("volume")
    , m_reload_delayed(false)
#if ENABLE_RENDER_PICKING_PASS
    , m_show_picking_texture(false)
#endif // ENABLE_RENDER_PICKING_PASS
    , m_render_sla_auxiliaries(true)
    , m_layers_editing(*this)
    , m_speed_editing(*this)
    , m_infill_walls_editing(*this)
    , m_labels(*this)
    , m_slope(m_volumes)
    , m_sequential_print_clearance(*this)
{
    if (m_canvas != nullptr) {
        m_timer.SetOwner(m_canvas);
        m_render_timer.SetOwner(m_canvas);
#if ENABLE_RETINA_GL
        m_retina_helper.reset(new RetinaHelper(canvas));
#endif // ENABLE_RETINA_GL
    }
    m_timer_set_color.Bind(wxEVT_TIMER, &GLCanvas3D::on_set_color_timer, this);
    load_arrange_settings();

    m_selection.set_volumes(&m_volumes.volumes);

    const wxString alt                               = GUI::shortkey_alt_prefix();
    m_assembly_view_desc["object_selection_caption"] = _L("Left mouse button");
    m_assembly_view_desc["object_selection"]         = _L("object selection");
    m_assembly_view_desc["part_selection_caption"]   = alt  + _L("Left mouse button");
    m_assembly_view_desc["part_selection"]         = _L("part selection");
    m_assembly_view_desc["number_key_caption"]       = "1~16 " + _L("number keys");
    m_assembly_view_desc["number_key"]       = _L("number keys can quickly change the color of objects");

    m_render_pipeline_stage_stack.push(ERenderPipelineStage::Normal);
}

GLCanvas3D::~GLCanvas3D()
{
    reset_volumes(false);

    m_sel_plate_toolbar.del_all_item();
    m_sel_plate_toolbar.del_stats_item();

    s_full_screen_mesh.reset();
    m_unit_cube.reset();
#if ENABLE_SHOW_CAMERA_TARGET
    m_camera_target_mark.reset();
#endif // ENABLE_SHOW_CAMERA_TARGET

    m_sequential_print_clearance.reset();
}

void GLCanvas3D::set_context(wxGLContext* context)
{
    if (context != m_context) {
        m_context = context;
        m_dirty_context = true;
    }
}

void GLCanvas3D::set_type(ECanvasType type)
{
    if (type != m_canvas_type) {
        m_canvas_type = type;
    }
}

void GLCanvas3D::post_event(wxEvent &&event)
{
    event.SetEventObject(m_canvas);
    wxPostEvent(m_canvas, event);
}

bool GLCanvas3D::init()
{
    if (m_initialized)
        return true;

    if (m_canvas == nullptr || m_context == nullptr)
        return false;

    // init dark mode status
    on_change_color_mode(wxGetApp().app_config->get("dark_color_mode") == "1", false);

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< " enter";
    glsafe(::glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
    glsafe(::glClearDepth(1.0f));

    glsafe(::glDepthFunc(GL_LESS));

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    // Set antialiasing / multisampling
    glsafe(::glDisable(GL_LINE_SMOOTH));
    glsafe(::glDisable(GL_POLYGON_SMOOTH));

    if (m_multisample_allowed)
        glsafe(::glEnable(GL_MULTISAMPLE));

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": before m_layers_editing init";
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        m_layers_editing.init();
        //BBS: variable print speed along Z.
        m_speed_editing.init();
        //Orio: variable sparse infill density / wall count along Z.
        m_infill_walls_editing.init();
    }

    // on linux the gl context is not valid until the canvas is not shown on screen
    // we defer the geometry finalization of volumes until the first call to render()
    m_volumes.finalize_geometry(true);

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": before gizmo init";
    if (m_gizmos.is_enabled() && !m_gizmos.init())
        std::cout << "Unable to initialize gizmos: please, check that all the required textures are available" << std::endl;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": before _init_toolbars";
    if (!_init_toolbars())
        return false;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": finish _init_toolbars";
    if (m_selection.is_enabled() && !m_selection.init())
        return false;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": finish m_selection";

#if ENABLE_IMGUI_STYLE_EDITOR
    //BBS load render color for style editor
    GLVolume::load_render_colors();
    PartPlate::load_render_colors();
    GLGizmoBase::load_render_colors();
    GLCanvas3D::load_render_colors();
    Bed3D::load_render_colors();
#endif
    //if (!wxGetApp().is_gl_version_greater_or_equal_to(3, 0))
    //    wxGetApp().plater()->enable_wireframe(false);
    m_initialized = true;

    return true;
}

void GLCanvas3D::on_change_color_mode(bool is_dark, bool reinit) {
    m_is_dark = is_dark;
    // Bed color
    m_bed.on_change_color_mode(is_dark);
    // GcodeViewer color
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.on_change_color_mode(is_dark);
    // ImGui Style
    wxGetApp().imgui()->on_change_color_mode(is_dark);
    // Notification
    wxGetApp().plater()->get_notification_manager()->on_change_color_mode(is_dark);
    // DailyTips Window
    wxGetApp().plater()->get_dailytips()->on_change_color_mode(is_dark);
    // Preview Slider
    IMSlider* m_layers_slider = get_gcode_viewer().get_layers_slider();
    IMSlider* m_moves_slider = get_gcode_viewer().get_moves_slider();
    m_layers_slider->on_change_color_mode(is_dark);
    m_moves_slider->on_change_color_mode(is_dark);
    // Orio : curseurs de coupe X/Y
    if (IMSlider* x_slider = get_gcode_viewer().get_x_slider())
        x_slider->on_change_color_mode(is_dark);
    if (IMSlider* y_slider = get_gcode_viewer().get_y_slider())
        y_slider->on_change_color_mode(is_dark);
    // Partplate
    wxGetApp().plater()->get_partplate_list().on_change_color_mode(is_dark);

    // Toolbar
    if (m_canvas_type == CanvasView3D) {
        m_gizmos.on_change_color_mode(is_dark);
        if (reinit) {
            // reset svg
            _switch_toolbars_icon_filename();
            const auto& p_main_toolbar = get_main_toolbar();
            if (p_main_toolbar) {
                p_main_toolbar->set_icon_dirty();
            }
            wxGetApp().plater()->get_collapse_toolbar().set_icon_dirty();
        }
    }
    if (m_canvas_type == CanvasAssembleView) {
        m_gizmos.on_change_color_mode(is_dark);
    }
}

void GLCanvas3D::set_as_dirty()
{
    m_dirty = true;
}

const float GLCanvas3D::get_scale() const
{
#if ENABLE_RETINA_GL
    return m_retina_helper->get_scale_factor();
#else
    return 1.0f;
#endif
}

gcode::GCodeViewer& GLCanvas3D::get_gcode_viewer() const
{
    if (!m_p_gcode_viewer) {
        m_p_gcode_viewer = std::make_shared<gcode::GCodeViewer>();
    }
    return *m_p_gcode_viewer;
}

void GLCanvas3D::init_gcode_viewer(ConfigOptionMode mode, Slic3r::PresetBundle* preset_bundle)
{
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.init(mode, preset_bundle);
}

void GLCanvas3D::reset_gcode_toolpaths()
{
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.reset();
}

void GLCanvas3D::update_gcode_sequential_view_current(unsigned int first, unsigned int last)
{
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.update_sequential_view_current(first, last);
}

bool GLCanvas3D::is_in_same_model_object(const std::vector<int> volume_ids) {
    if (volume_ids.size() >= 2) {
        auto gl_volume        = m_selection.get_volume(volume_ids[0]);
        if (gl_volume) {
            std::set<int> object_sets;
            object_sets.insert(gl_volume->object_idx());
            for (int i = 1; i < volume_ids.size(); i++) {
                auto temp_volume = m_selection.get_volume(volume_ids[i]);
                if (temp_volume) {
                    auto temp_object_idx = temp_volume->object_idx();
                    if (object_sets.find(temp_object_idx) == object_sets.end()) { return false; }
                } else {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "check code";
                }
            }
            if (object_sets.size() == 1) {
                return true;
            }
        }
    }
    return false;
}

unsigned int GLCanvas3D::get_volumes_count() const {
    return (unsigned int)m_volumes.volumes.size();
}

void GLCanvas3D::reset_volumes(bool set_notice)
{
    if (!m_initialized)
        return;

    if (m_volumes.empty())
        return;

    _set_current(true);

    m_selection.clear();
    m_volumes.clear();
    m_dirty = true;
    if (set_notice) { _set_warning_notification(EWarning::ObjectOutside, false); }
}

//BBS: get current plater's bounding box
BoundingBoxf3 GLCanvas3D::_get_current_partplate_print_volume()
{
    BoundingBoxf3 test_volume;
    if (m_process && m_config)
    {
        BoundingBoxf3 plate_bb = m_process->get_current_plate()->get_bounding_box(false);
        BoundingBoxf3 print_volume({ plate_bb.min(0), plate_bb.min(1), 0.0 }, { plate_bb.max(0), plate_bb.max(1), m_config->opt_float("printable_height") });
        // Allow the objects to protrude below the print bed
        print_volume.min(2) = -1e10;
        print_volume.min(0) -= Slic3r::BuildVolume::BedEpsilon;
        print_volume.min(1) -= Slic3r::BuildVolume::BedEpsilon;
        print_volume.max(0) += Slic3r::BuildVolume::BedEpsilon;
        print_volume.max(1) += Slic3r::BuildVolume::BedEpsilon;
        test_volume = print_volume;
    }
    else
        test_volume = BoundingBoxf3();

    return test_volume;
}

static bool construct_error_string(ObjectFilamentResults& object_result, std::string& error_string)
{
    error_string.clear();
    if (!object_result.partly_outside_objects.empty()) {
        error_string += _u8L("Following objects are laid over the boundary of plate or exceeds the height limit:\n");
        for(auto& object: object_result.partly_outside_objects)
        {
            error_string += object->name;
            error_string += "\n";
        }
        error_string += _u8L("Please solve the problem by moving it totally on or off the plate, and confirming that the height is within the build volume.\n");
        return true;
    }
    return false;
}

static bool construct_assembly_warning_string(std::vector<std::string>& object_result, std::string& error_string)
{
    error_string.clear();
    if (!object_result.size()) {
        return false;
    }
    bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    double koef = imperial_units ? GizmoObjectManipulation::mm_to_in : 1.0f;
    float distance_limit = 10000.0f;
    if (imperial_units) {
        distance_limit *= koef;
    }
    if (imperial_units) {
        error_string += (boost::format(_utf8(L("Assembly's bounding box is too large ( max size >= %1% in ) which may cause rendering issues.\n"))) % distance_limit).str();
    }
    else {
        error_string += (boost::format(_utf8(L("Assembly's bounding box is too large ( max size >= %1% mm ) which may cause rendering issues.\n"))) % distance_limit).str();
    }
    if (!object_result.empty()) {
        if (imperial_units) {
            error_string += (boost::format(_utf8(L("Following objects are too far ( distance >= %1% in ) from the original of the world coordinate system:\n"))) % distance_limit).str();
        }
        else {
            error_string += (boost::format(_utf8(L("Following objects are too far ( distance >= %1% mm ) from the original of the world coordinate system:\n"))) % distance_limit).str();
        }
        for (const auto& t_name : object_result)
        {
            error_string += t_name;
            error_string += "\n";
        }
    }
    return true;
}

static std::pair<bool, bool> construct_extruder_unprintable_error(ObjectFilamentResults& object_result, std::string& left_extruder_unprintable_text, std::string& right_extruder_unprintable_text)
{
    left_extruder_unprintable_text.clear();
    right_extruder_unprintable_text.clear();
    if (object_result.filaments.empty())
        return {false,false};

    std::string gc_pt = wxGetApp().preset_bundle->printers.get_edited_preset().get_printer_type(wxGetApp().preset_bundle);
    std::string gc_dep_nz = DevPrinterConfigUtil::get_toolhead_display_name(gc_pt, DEPUTY_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::LowerCase);
    std::string gc_main_nz = DevPrinterConfigUtil::get_toolhead_display_name(gc_pt, MAIN_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::LowerCase);
    const std::vector<std::string> nozzle_name_list = { _u8L(gc_dep_nz.c_str()), _u8L(gc_main_nz.c_str()) };

    std::vector<ObjectFilamentInfo> left_unprintable_objects;
    std::vector<ObjectFilamentInfo> right_unprintable_objects;

    std::vector<int> conflicted_filaments = object_result.filaments;

    auto mode = object_result.mode;

    for (auto& obj_filament : object_result.object_filaments) {
        if (mode == FilamentMapMode::fmmManual) {
            for (auto& elem : obj_filament.manual_filaments) {
                bool found_left = false, found_right = false;
                int filamnet_id = elem.first;
                int extruder_id = elem.second;
                if (extruder_id == 1 && !found_left) {
                    found_left = true;
                    left_unprintable_objects.emplace_back(obj_filament);
                }
                if (extruder_id == 2 && !found_right) {
                    found_right = true;
                    right_unprintable_objects.emplace_back(obj_filament);
                }
            }
        }
        else {
            if (!obj_filament.auto_filaments.empty()) {
                left_unprintable_objects.emplace_back(obj_filament);
                right_unprintable_objects.emplace_back(obj_filament);
            }
        }
    }

    Preset &preset = GUI::wxGetApp().preset_bundle->printers.get_edited_preset();
    float   left_x_min = 0, left_x_max = 0, left_y_min = 0, left_y_max = 0, left_z_min = 0, left_z_max = 0;
    float   right_x_min = 0, right_x_max = 0, right_y_min = 0, right_y_max = 0, right_z_min = 0, right_z_max = 0;
    auto printable_height_option = preset.config.option<ConfigOptionFloatsNullable>("extruder_printable_height");
    if (printable_height_option && printable_height_option->values.size() == 2) {
        left_z_max = (float) printable_height_option->values[0];
        right_z_max = (float) printable_height_option->values[1];
    }
    std::vector<Pointfs> printable_areas = preset.config.option<ConfigOptionPointsGroups>("extruder_printable_area")->values;
    if (printable_areas.size() == 2 && printable_areas[0].size() == 4) {
        left_x_min = printable_areas[0][0][0];
        left_y_min = printable_areas[0][0][1];
        left_x_max = printable_areas[0][2][0];
        left_y_max = printable_areas[0][2][1];

        right_x_min = printable_areas[1][0][0];
        right_y_min = printable_areas[1][0][1];
        right_x_max = printable_areas[1][2][0];
        right_y_max = printable_areas[1][2][1];
    }

    std::vector<std::string> tips(2);
    for (size_t idx = 0; idx < tips.size(); ++idx) {
        const auto& unprintable_objs = idx == 0 ? left_unprintable_objects : right_unprintable_objects;
        auto& output_text = idx == 0 ? left_extruder_unprintable_text : right_extruder_unprintable_text;
        if (unprintable_objs.empty())
            continue;
        std::string nozzle_name = nozzle_name_list[idx];
        std::string model_prefix;
        if ((idx == 0 && left_unprintable_objects.size() > 1) || (idx == 1 && right_unprintable_objects.size() > 1))
            model_prefix = (boost::format(_u8L("The position or size of some models exceeds the %s's printable range.")) % nozzle_name).str();
        else
            model_prefix = (boost::format(_u8L("The position or size of the model %s exceeds the %s's printable range."))
                           %object_result.object_filaments.front().object->name % nozzle_name).str();
        tips[idx] += model_prefix;

        tips[idx] += (boost::format(_u8L(" Please check and adjust the part's position or size to fit the printable range:\n"))).str();
        if (idx == 0) {
            std::string dep_nz_sc = DevPrinterConfigUtil::get_toolhead_display_name(gc_pt, DEPUTY_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::SentenceCase);
            tips[idx] += (boost::format(_u8L(dep_nz_sc.c_str()) + std::string(": X:%1%-%2%, Y:%3%-%4%, Z:%5%-%6%\n"))
                         % format_number(left_x_min) % format_number(left_x_max) % format_number(left_y_min)
                         % format_number(left_y_max) % format_number(left_z_min) % format_number(left_z_max)).str();
        } else {
            std::string main_nz_sc = DevPrinterConfigUtil::get_toolhead_display_name(gc_pt, MAIN_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::SentenceCase);
            tips[idx] += (boost::format(_u8L(main_nz_sc.c_str()) + std::string(": X:%1%-%2%, Y:%3%-%4%, Z:%5%-%6%"))
                         %format_number(right_x_min) %format_number(right_x_max) %format_number(right_y_min)
                         %format_number(right_y_max) %format_number(right_z_min) %format_number(right_z_max)).str();
        }
        output_text = tips[idx];
    }

    return { !left_unprintable_objects.empty(),!right_unprintable_objects.empty() };
}

ModelInstanceEPrintVolumeState GLCanvas3D::check_volumes_outside_state(ObjectFilamentResults* object_results) const
{
    //BBS: if not initialized, return inside directly insteadof assert
    if (!m_initialized) {
        return ModelInstancePVS_Inside;
    }
    //assert(m_initialized);

    ModelInstanceEPrintVolumeState state;
    m_volumes.check_outside_state(m_bed.build_volume(), &state, object_results,*m_model);

    construct_error_string(*object_results, get_object_clashed_text());
    construct_extruder_unprintable_error(*object_results, get_left_extruder_unprintable_text(), get_right_extruder_unprintable_text());
    return state;
}

bool GLCanvas3D::is_volumes_selected_and_sinking() const {
    for (GLVolume *volume : m_volumes.volumes) {
        if (volume->selected && volume->is_sinking() && !volume->is_below_printbed()) {
            return true;
        }
    }
    return false;
}

void GLCanvas3D::toggle_selected_volume_visibility(bool selected_visible)
{
    m_render_sla_auxiliaries = !selected_visible;
    if (selected_visible) {
        const Selection::IndicesList &idxs = m_selection.get_volume_idxs();
        if (idxs.size() > 0) {
            for (GLVolume *vol : m_volumes.volumes) {
                if (vol->composite_id.object_id >= 1000 && vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count())
                    continue; // the wipe tower
                if (vol->composite_id.volume_id >= 0) {
                    vol->is_active = false;
                }
            }
            for (unsigned int idx : idxs) {
                GLVolume *v  = const_cast<GLVolume *>(m_selection.get_volume(idx));
                v->is_active = true;
            }
        }
    } else { // show all
        for (GLVolume *vol : m_volumes.volumes) {
            if (vol->composite_id.object_id >= 1000 && vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count())
                continue; // the wipe tower
            if (vol->composite_id.volume_id >= 0) {
                vol->is_active = true;
            }
        }
    }
}

void GLCanvas3D::toggle_sla_auxiliaries_visibility(bool visible, const ModelObject *mo, int instance_idx)
{
    m_render_sla_auxiliaries = visible;

    for (GLVolume* vol : m_volumes.volumes) {
        if (vol->composite_id.object_id >= 1000 &&
            vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count())
            continue; // the wipe tower
        if ((mo == nullptr || m_model->objects[vol->composite_id.object_id] == mo)
        && (instance_idx == -1 || vol->composite_id.instance_id == instance_idx)
        && vol->composite_id.volume_id < 0)
            vol->is_active = visible;
    }
}

void GLCanvas3D::toggle_model_objects_visibility(bool visible, const ModelObject* mo, int instance_idx, const ModelVolume* mv)
{
    for (GLVolume* vol : m_volumes.volumes) {
        // BBS: add partplate logic
        if (vol->composite_id.object_id >= 1000 &&
            vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count()) { // wipe tower
            vol->is_active = (visible && mo == nullptr);
        }
        else {
            if ((mo == nullptr || m_model->objects[vol->composite_id.object_id] == mo)
            && (instance_idx == -1 || vol->composite_id.instance_id == instance_idx)
            && (mv == nullptr || m_model->objects[vol->composite_id.object_id]->volumes[vol->composite_id.volume_id] == mv)) {
                vol->is_active = visible;

                if (instance_idx == -1) {
                    vol->force_native_color = false;
                    vol->force_neutral_color = false;
                } else {
                    auto  gizmo_type = m_gizmos.get_current_type();
                    if (m_gizmos.is_paint_gizmo()
                        && ! vol->is_modifier)
                        vol->force_neutral_color = true;
                    else if (gizmo_type == GLGizmosManager::BrimEars)
                        vol->force_neutral_color = false;
                    else if (gizmo_type == GLGizmosManager::MmuSegmentation)
                        vol->is_active = false;
                    else if (gizmo_type == GLGizmosManager::Text || gizmo_type == GLGizmosManager::MeshBoolean) {
                        vol->force_native_color  = false;
                        vol->force_neutral_color = false;
                    }
                    else
                        vol->force_native_color = true;
                }
            }
        }
    }
    if (visible && !mo)
        toggle_sla_auxiliaries_visibility(true, mo, instance_idx);

    if (!mo && !visible && !m_model->objects.empty() && (m_model->objects.size() > 1 || m_model->objects.front()->instances.size() > 1))
        _set_warning_notification(EWarning::SomethingNotShown, true);

    if (!mo && visible)
        _set_warning_notification(EWarning::SomethingNotShown, false);

    //Orio: the hidden « Hors impression » area stays hidden when a gizmo shows the objects again.
    update_parked_volumes_visibility();
}

void GLCanvas3D::update_instance_printable_state_for_object(const size_t obj_idx)
{
    ModelObject* model_object = m_model->objects[obj_idx];
    for (int inst_idx = 0; inst_idx < (int)model_object->instances.size(); ++inst_idx) {
        ModelInstance* instance = model_object->instances[inst_idx];

        for (GLVolume* volume : m_volumes.volumes) {
            if ((volume->object_idx() == (int)obj_idx) && (volume->instance_idx() == inst_idx))
                volume->printable = instance->printable;
                if (!volume->printable) {
                    volume->render_color = GLVolume::UNPRINTABLE_COLOR;
                }
        }
    }
}

void GLCanvas3D::update_instance_printable_state_for_objects(const std::vector<size_t>& object_idxs)
{
    for (size_t obj_idx : object_idxs)
        update_instance_printable_state_for_object(obj_idx);
    update_parked_volumes_visibility();
}

//Orio: the instances of the hidden « Hors impression » area are inactive: neither drawn nor pickable. Only the
// volumes deactivated here are activated again, the other inactive ones belong to the gizmos.
void GLCanvas3D::update_parked_volumes_visibility()
{
    if (m_canvas_type != ECanvasType::CanvasView3D || m_model == nullptr)
        return;
    const PartPlateList          &plate_list = wxGetApp().plater()->get_partplate_list();
    const bool                    hidden     = plate_list.is_parking_zone_hidden();
    std::set<std::pair<int, int>> now_hidden;
    for (GLVolume *volume : m_volumes.volumes) {
        if (volume->is_wipe_tower || volume->composite_id.volume_id < 0)
            continue;
        const std::pair<int, int> instance(volume->object_idx(), volume->instance_idx());
        if (hidden && plate_list.is_parked_instance(instance.first, instance.second)) {
            volume->is_active = false;
            now_hidden.insert(instance);
        } else if (m_parked_hidden.count(instance))
            volume->is_active = true;
    }
    m_parked_hidden = std::move(now_hidden);
}

void GLCanvas3D::set_parking_zone_hidden(bool hidden)
{
    PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
    if (plate_list.is_parking_zone_hidden() == hidden)
        return;
    plate_list.set_parking_zone_hidden(hidden);
    if (hidden) {
        // Its selected instances are no more editable.
        for (unsigned int idx : m_selection.get_volume_idxs()) {
            const GLVolume *volume = m_volumes.volumes[idx];
            if (plate_list.is_parked_instance(volume->object_idx(), volume->instance_idx())) {
                wxGetApp().plater()->deselect_all();
                break;
            }
        }
    }
    update_parked_volumes_visibility();
    m_dirty = true;
}

void GLCanvas3D::zoom_to_parking_zone()
{
    PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
    if (!plate_list.has_parked_instances())
        return;
    BoundingBoxf3 box = plate_list.get_parking_zone_box();
    if (!box.defined)
        return;
    for (const GLVolume *volume : m_volumes.volumes)
        if (!volume->is_wipe_tower && volume->composite_id.volume_id >= 0 && plate_list.is_parked_instance(volume->object_idx(), volume->instance_idx()))
            box.merge(volume->transformed_bounding_box());
    _zoom_to_box(box);
}

void GLCanvas3D::set_config(const DynamicPrintConfig* config)
{
    m_config = config;
    m_layers_editing.set_config(config);
    //BBS: variable print speed along Z.
    m_speed_editing.set_config(config);
    //Orio: variable sparse infill density / wall count along Z.
    m_infill_walls_editing.set_config(config);
}

void GLCanvas3D::set_process(BackgroundSlicingProcess *process)
{
    m_process = process;
}

void GLCanvas3D::set_model(Model* model)
{
    m_model = model;
    m_selection.set_model(m_model);
}

const Selection& GLCanvas3D::get_selection() const
{
    return m_selection;
}

Selection& GLCanvas3D::get_selection()
{
    return m_selection;
}

void GLCanvas3D::bed_shape_changed()
{
    refresh_camera_scene_box();
    get_active_camera().requires_zoom_to_bed = true;
    m_dirty = true;
}

void GLCanvas3D::plates_count_changed()
{
    refresh_camera_scene_box();
    m_dirty = true;
}

void GLCanvas3D::set_use_clipping_planes(bool use)
{
    if (m_gizmos.get_current_type() == GLGizmosManager::EType::Text) {
        m_use_clipping_planes = false;
    }
    else{
        m_use_clipping_planes = use;
    }
}

void GLCanvas3D::set_color_by(const std::string &value) {
    m_color_by = value;
}

void GLCanvas3D::refresh_camera_scene_box()
{
    get_active_camera().set_scene_box(scene_bounding_box());
}

BoundingBoxf3 GLCanvas3D::assembly_view_cur_bounding_box() const {
    return m_model->bounding_box_in_assembly_view();
}

BoundingBoxf3 GLCanvas3D::volumes_bounding_box(bool limit_to_expand_plate) const
{
    BoundingBoxf3 bb;
    BoundingBoxf3 expand_part_plate_list_box;
    bool          is_limit = limit_to_expand_plate;
    if (is_limit) {
        auto        plate_list_box    = wxGetApp().plater()->get_partplate_list().get_bounding_box();
        auto        horizontal_radius = 0.5 * sqrt(std::pow(plate_list_box.min[0] - plate_list_box.max[0], 2) + std::pow(plate_list_box.min[1] - plate_list_box.max[1], 2));
        const float scale             = 2;
        expand_part_plate_list_box.merge(plate_list_box.min - scale * Vec3d(horizontal_radius, horizontal_radius, 0));
        expand_part_plate_list_box.merge(plate_list_box.max + scale * Vec3d(horizontal_radius, horizontal_radius, 0));
    }
    for (const GLVolume *volume : m_volumes.volumes) {
        if (!m_apply_zoom_to_volumes_filter || ((volume != nullptr) && volume->zoom_to_volumes)) {
            const auto v_bb     = volume->transformed_bounding_box();
            if (is_limit && !expand_part_plate_list_box.overlap(v_bb))
                continue;
            if (v_bb.max_size() > 100000) {//unit::mm more than 100m
                continue;
            }
            bb.merge(v_bb);
        }
    }
    return bb;
}

bool GLCanvas3D::is_volumes_limit_to_expand_plate() const {
    return m_canvas_type != ECanvasType::CanvasAssembleView;
}

BoundingBoxf3 GLCanvas3D::scene_bounding_box() const
{
    BoundingBoxf3 bb = volumes_bounding_box(is_volumes_limit_to_expand_plate());
    bb.merge(m_bed.extended_bounding_box());
    double h = m_bed.build_volume().printable_height();
    //FIXME why -h?
    bb.min.z() = std::min(bb.min.z(), -h);
    bb.max.z() = std::max(bb.max.z(), h);

    //BBS merge plate scene bounding box
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        PartPlateList& plate = wxGetApp().plater()->get_partplate_list();
        bb.merge(plate.get_bounding_box());
    }

    return bb;
}

BoundingBoxf3 GLCanvas3D::plate_scene_bounding_box(int plate_idx) const
{
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);

    BoundingBoxf3 bb = plate->get_bounding_box(true);
    if (m_config != nullptr) {
        double h = m_config->opt_float("printable_height");
        bb.min(2) = std::min(bb.min(2), -h);
        bb.max(2) = std::max(bb.max(2), h);
    }

    return bb;
}

bool GLCanvas3D::is_layers_editing_enabled() const
{
    return m_layers_editing.is_enabled();
}

bool GLCanvas3D::is_layers_editing_allowed() const
{
    return m_layers_editing.is_allowed();
}

void GLCanvas3D::reset_layer_height_profile()
{
    wxGetApp().plater()->take_snapshot("Variable layer height - Reset");
    m_layers_editing.reset_layer_height_profile(*this);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::adaptive_layer_height_profile(float quality_factor)
{
    wxGetApp().plater()->take_snapshot("Variable layer height - Adaptive");
    m_layers_editing.adaptive_layer_height_profile(*this, quality_factor);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::smooth_layer_height_profile(const HeightProfileSmoothingParams& smoothing_params)
{
    wxGetApp().plater()->take_snapshot("Variable layer height - Smooth all");
    m_layers_editing.smooth_layer_height_profile(*this, smoothing_params);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

bool GLCanvas3D::is_reload_delayed() const
{
    return m_reload_delayed;
}

void GLCanvas3D::enable_layers_editing(bool enable)
{
    //BBS: both editors own the same vertical bar on the right hand side, only one may be active.
    if (enable && m_speed_editing.is_enabled())
        enable_speed_editing(false);
    if (enable && m_infill_walls_editing.is_enabled())
        enable_infill_walls_editing(false);
    m_layers_editing.set_enabled(enable);
    set_as_dirty();
}

//BBS: variable print speed along Z.
bool GLCanvas3D::is_speed_editing_enabled() const
{
    return m_speed_editing.is_enabled();
}

bool GLCanvas3D::is_speed_editing_allowed() const
{
    return m_speed_editing.is_allowed();
}

void GLCanvas3D::enable_speed_editing(bool enable)
{
    if (enable && m_layers_editing.is_enabled())
        enable_layers_editing(false);
    if (enable && m_infill_walls_editing.is_enabled())
        enable_infill_walls_editing(false);
    m_speed_editing.set_enabled(enable);
    set_as_dirty();
}

void GLCanvas3D::reset_speed_profile()
{
    //Orio: no undo snapshot here, SpeedEditing::commit_to_model() takes it, and only when the model really
    // changes. A second one made the first Ctrl+Z after a reset jump between two identical states (no effect).
    m_speed_editing.reset_speed_profile(*this);
    m_speed_editing.state = SpeedEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::smooth_speed_profile(const HeightProfileSmoothingParams& smoothing_params)
{
    m_speed_editing.smooth_speed_profile(*this, smoothing_params);
    m_speed_editing.state = SpeedEditing::Completed;
    m_dirty = true;
}

//Orio: same as detach_infill_walls_editing(). The next render attaches the tool to the restored object,
// SpeedEditing::select_object() then reloads the restored curves.
void GLCanvas3D::detach_speed_editing()
{
    if (m_model != nullptr)
        m_speed_editing.select_object(*m_model, -1);
    m_dirty = true;
}

//Orio: variable sparse infill density / wall count along Z.
bool GLCanvas3D::is_infill_walls_editing_enabled() const
{
    return m_infill_walls_editing.is_enabled();
}

bool GLCanvas3D::is_infill_walls_editing_allowed() const
{
    return m_infill_walls_editing.is_allowed();
}

void GLCanvas3D::enable_infill_walls_editing(bool enable)
{
    // The three editors share the same vertical bar on the right hand side, only one may be active.
    if (enable && m_layers_editing.is_enabled())
        enable_layers_editing(false);
    if (enable && m_speed_editing.is_enabled())
        enable_speed_editing(false);
    m_infill_walls_editing.set_enabled(enable);
    set_as_dirty();
}

void GLCanvas3D::reset_infill_walls_profile()
{
    // The undo snapshot is taken by the editor, only when the model really changes.
    m_infill_walls_editing.reset_profile(*this);
    m_infill_walls_editing.state = InfillWallsEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::smooth_infill_walls_profile(const HeightProfileSmoothingParams& smoothing_params)
{
    m_infill_walls_editing.smooth_profile(*this, smoothing_params);
    m_infill_walls_editing.state = InfillWallsEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::detach_infill_walls_editing()
{
    // Nothing may dereference the edited ModelObject once the jump has deleted it: detach now. The curves are
    // committed to the model, the next render reloads the restored ones.
    if (m_model != nullptr)
        m_infill_walls_editing.select_object(*m_model, -1);
    m_dirty = true;
}

void GLCanvas3D::enable_legend_texture(bool enable)
{
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.enable_legend(enable);
}

void GLCanvas3D::enable_picking(bool enable)
{
    m_picking_enabled = enable;
    m_selection.set_mode(Selection::Instance);
}

void GLCanvas3D::enable_moving(bool enable)
{
    m_moving_enabled = enable;
}

void GLCanvas3D::enable_gizmos(bool enable)
{
    m_gizmos.set_enabled(enable);
}

void GLCanvas3D::enable_selection(bool enable)
{
    m_selection.set_enabled(enable);
}

void GLCanvas3D::enable_main_toolbar(bool enable)
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return;
    }
    p_main_toolbar->set_enabled(enable);
}

void GLCanvas3D::reset_select_plate_toolbar_selection() {
    if (m_sel_plate_toolbar.m_all_plates_stats_item)
        m_sel_plate_toolbar.m_all_plates_stats_item->selected = false;
    if (wxGetApp().mainframe)
        wxGetApp().mainframe->update_slice_print_status(MainFrame::eEventSliceUpdate, true, true);
}

void GLCanvas3D::enable_select_plate_toolbar(bool enable)
{
    m_sel_plate_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_return_toolbar(bool enable)
{
    m_return_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_dynamic_background(bool enable)
{
    m_dynamic_background_enabled = enable;
}

void GLCanvas3D::allow_multisample(bool allow)
{
    m_multisample_allowed = allow;
}

void GLCanvas3D::zoom_to_bed()
{
    BoundingBoxf3 box = m_bed.build_volume().bounding_volume();
    box.min.z() = 0.0;
    box.max.z() = 0.0;
    _zoom_to_box(box, DefaultCameraZoomToBedMarginFactor);
}

void GLCanvas3D::zoom_to_volumes()
{
    m_apply_zoom_to_volumes_filter = true;
    _zoom_to_box(volumes_bounding_box(is_volumes_limit_to_expand_plate()));
    m_apply_zoom_to_volumes_filter = false;
}

void GLCanvas3D::zoom_to_selection()
{
    if (!m_selection.is_empty())
        _zoom_to_box(m_selection.get_bounding_box());
}

void GLCanvas3D::zoom_to_gcode()
{
    const auto& t_gcode_viewer = get_gcode_viewer();
    _zoom_to_box(t_gcode_viewer.get_paths_bounding_box(), 1.05);
}

void GLCanvas3D::zoom_to_plate(int plate_idx)
{
    BoundingBoxf3 box;
    if (plate_idx == REQUIRES_ZOOM_TO_ALL_PLATE) {
        box = wxGetApp().plater()->get_partplate_list().get_bounding_box();
        box.min.z() = 0.0;
        box.max.z() = 0.0;
        _zoom_to_box(box, DefaultCameraZoomToPlateMarginFactor);
    } else {
        PartPlate* plate = nullptr;
        if (plate_idx == REQUIRES_ZOOM_TO_CUR_PLATE) {
            plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        }else {
            assert(plate_idx >= 0 && plate_idx < wxGetApp().plater()->get_partplate_list().get_plate_count());
            plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
        }
        box = plate->get_bounding_box(true);
        box.min.z() = 0.0;
        box.max.z() = 0.0;
        _zoom_to_box(box, DefaultCameraZoomToPlateMarginFactor);
    }
}

void GLCanvas3D::zoom_to_fit()
{
    if (!can_show_3d_navigator())
        return;

    select_view("plate");
    if (m_selection.is_empty()) {
        if (m_canvas_type == ECanvasType::CanvasAssembleView)
            zoom_to_volumes();
        else
            zoom_to_bed();
    }
    else {
        zoom_to_selection();
    }
}

void GLCanvas3D::select_view(const std::string& direction)
{
    get_active_camera().select_view(direction);
    if (m_canvas != nullptr)
        m_canvas->Refresh();
}

void GLCanvas3D::select_plate()
{
    wxGetApp().plater()->get_partplate_list().select_plate_view();
    if (m_canvas != nullptr)
        m_canvas->Refresh();
}

void GLCanvas3D::update_volumes_colors_by_extruder()
{
    if (m_config != nullptr)
        m_volumes.update_colors_by_extruder(m_config);
}

//Orio: place of the main toolbar of the 3D view chosen by the user, by dragging its grip (render_main_toolbar_grip()),
// remembered in the app config ("main_toolbar_place"). Not custom: at the top, in the middle (default). Vertical: against the
// left (edge -1) or right (edge 1) side of the view, its top at y; horizontal: its top left corner at (x, y). Pixels of the
// view. One 3D view (Prepare): a static.
struct MainToolbarPlace
{
    bool  loaded{false};
    bool  custom{false};
    bool  vertical{false};
    int   edge{-1};
    float x{0.f};
    float y{0.f};
    // size of its tools chosen by dragging its sides (render_main_toolbar_sides()), 0.5 to 2 (app config "main_toolbar_size", %)
    float factor{1.f};
    // its rectangle at the last frame, valid if x1 > x0; laid out vertically then or not
    float x0{0.f};
    float y0{0.f};
    float x1{0.f};
    float y1{0.f};
    bool  rect_vertical{false};
    // dragged by its grip, held at this point of the grip
    bool   dragging{false};
    ImVec2 grab{0.f, 0.f};
    // what is drawn over it (ImGui), that it keeps off: the view cube and the "Fit camera" button, the cut slider (pixels,
    // GLCanvas3D::_render_main_toolbar(), at each frame)
    std::vector<ImRect> obstacles;
};
static MainToolbarPlace s_main_toolbar_place;

static void main_toolbar_place_load()
{
    MainToolbarPlace &p = s_main_toolbar_place;
    p.loaded = true;
    const int size = std::atoi(wxGetApp().app_config->get("main_toolbar_size").c_str());
    p.factor       = size > 0 ? std::clamp(0.01f * float(size), 0.5f, 2.f) : 1.f;
    const std::string s = wxGetApp().app_config->get("main_toolbar_place");
    float a = 0.f, b = 0.f;
    if (std::sscanf(s.c_str(), "h %f %f", &a, &b) == 2) {
        p.custom   = true;
        p.vertical = false;
        p.x        = a;
        p.y        = b;
    } else if (std::sscanf(s.c_str(), "v %f %f", &a, &b) == 2) {
        p.custom   = true;
        p.vertical = true;
        p.edge     = a < 0.f ? -1 : 1;
        p.y        = b;
    }
}

static void main_toolbar_place_save()
{
    const MainToolbarPlace &p = s_main_toolbar_place;
    wxGetApp().app_config->set("main_toolbar_size", std::to_string(int(std::lround(100.f * p.factor))));
    wxGetApp().app_config->set("main_toolbar_place", !p.custom ? std::string() :
                               p.vertical ? "v " + std::to_string(p.edge) + " " + std::to_string(int(p.y)) :
                                            "h " + std::to_string(int(p.x)) + " " + std::to_string(int(p.y)));
}

// Thickness of the main toolbar (height of a horizontal one, width of a column of a vertical one) and of its grip, pixels.
static float main_toolbar_thickness(const GLToolbar &toolbar)
{
    const ToolbarLayout &layout = toolbar.get_layout();
    return (layout.icons_size + 2.f * layout.border) * layout.scale;
}

static float main_toolbar_grip(const GLToolbar &toolbar) { return std::round(0.35f * main_toolbar_thickness(toolbar)); }

// Lays the main toolbar of the 3D view out at the place chosen by the user, kept in the view (a vertical one in the band
// [band_top, band_bottom] of its side, GLCanvas3D::main_toolbar_vertical_band()); false: default place (at the top, in the
// middle, set by the caller). A horizontal one keeps off the obstacles (MainToolbarPlace::obstacles: the view cube and the
// "Fit camera" button at the bottom left, the cut slider on a side): beside them on the side they leave free, else above.
static bool main_toolbar_apply_place(GLToolbar &toolbar, const Camera &camera, float band_top, float band_bottom)
{
    MainToolbarPlace &p = s_main_toolbar_place;
    if (!p.loaded)
        main_toolbar_place_load();
    toolbar.set_layout_type(p.custom && p.vertical ? ToolbarLayout::EType::Vertical : ToolbarLayout::EType::Horizontal);
    if (!p.custom) {
        toolbar.set_position_mode(ToolbarLayout::EPositionMode::TopMiddle);
        return false;
    }
    toolbar.set_position_mode(ToolbarLayout::EPositionMode::Custom);
    toolbar.set_offset(0.f);
    const std::array<int, 4> &viewport = camera.get_viewport();
    const float cnv_w = (float)viewport[2];
    const float cnv_h = (float)viewport[3];
    const float grip  = main_toolbar_grip(toolbar);
    // its size laid out at the last frame, else its natural one
    const bool  last = p.x1 > p.x0 && p.rect_vertical == p.vertical;
    const float w    = last ? p.x1 - p.x0 : toolbar.get_width();
    const float h    = last ? p.y1 - p.y0 : toolbar.get_height();
    float       x    = p.x;
    float       y    = p.y;
    if (p.vertical) {
        x = p.edge < 0 ? 0.f : std::max(0.f, cnv_w - w);
        y = std::clamp(y, band_top + grip, std::max(band_top + grip, band_bottom - h));
    } else {
        x = std::clamp(x, grip, std::max(grip, cnv_w - w));
        y = std::clamp(y, 0.f, std::max(0.f, cnv_h - h));
        const auto over = [&](float bx, float by, const ImRect &o) { return o.Overlaps(ImRect(bx - grip, by, bx + w, by + h)); };
        for (const ImRect &o : p.obstacles) {
            if (!over(x, y, o))
                continue;
            // an obstacle against the left side of the view: on its right; against the right side: on its left
            const float bx   = o.Min.x <= 0.f ? o.Max.x + grip : o.Min.x - w;
            bool        free = bx >= grip && bx + w <= cnv_w;
            for (const ImRect &q : p.obstacles)
                free = free && !over(bx, y, q);
            if (free)
                x = bx;
            else
                y = std::max(0.f, o.Min.y - h);
        }
    }
    const float inv_zoom = (float)camera.get_inv_zoom();
    toolbar.set_position((0.5f * cnv_h - y) * inv_zoom, (x - 0.5f * cnv_w) * inv_zoom);
    return true;
}

// The rectangle of the main toolbar in the view (pixels), from its buttons as drawn; false: none drawn.
static bool main_toolbar_rect(const GLToolbar &toolbar, const Camera &camera, ImVec2 &min, ImVec2 &max)
{
    const std::array<int, 4> &viewport = camera.get_viewport();
    const float half_w = 0.5f * (float)viewport[2];
    const float half_h = 0.5f * (float)viewport[3];
    const float zoom   = (float)camera.get_zoom();
    bool        any    = false;
    for (const std::shared_ptr<GLToolbarItem> &item : toolbar.get_items()) {
        if (!item || !item->is_visible() || item->is_collapsed() || item->render_rect[1] <= item->render_rect[0])
            continue;
        const ImVec2 a(half_w + item->render_rect[0] * zoom, half_h - item->render_rect[3] * zoom);
        const ImVec2 b(half_w + item->render_rect[1] * zoom, half_h - item->render_rect[2] * zoom);
        min = any ? ImMin(min, a) : a;
        max = any ? ImMax(max, b) : b;
        any = true;
    }
    if (any) {
        const float border = toolbar.get_layout().border * toolbar.get_layout().scale;
        min.x -= border;
        min.y -= border;
        max.x += border;
        max.y += border;
    }
    return any;
}

// The icons size of the main toolbar laid out vertically, at most size: its column and its grip above it fit in a band of
// band_height pixels, whatever the size chosen for the tools (MainToolbarPlace::factor): never behind the "Fit camera" button
// and the view cube under the band.
static float main_toolbar_vertical_icons_size(const GLToolbar &toolbar, float size, float band_height)
{
    // the buttons, and the separators between two of them, as laid out in a column
    int buttons = 0, separators = 0, pending = 0;
    for (const std::shared_ptr<GLToolbarItem> &item : toolbar.get_items()) {
        if (!item || !item->is_visible())
            continue;
        if (item->is_separator() || item->get_type() == GLToolbarItem::EType::SeparatorLine || item->is_collapse_button()) {
            if (buttons > 0 && !item->is_collapse_button())
                ++pending;
            continue;
        }
        separators += pending;
        pending = 0;
        ++buttons;
    }
    if (buttons == 0)
        return size;
    // column: scale (2 border + n icon + (n - 1) gap + separators (separator + gap)); grip: 0.35 scale (icon + 2 border)
    const ToolbarLayout &layout = toolbar.get_layout();
    const float          scale  = std::max(layout.scale, 0.01f);
    const float          room   = band_height / scale - 2.7f * layout.border - float(buttons - 1) * layout.gap_size -
                       float(separators) * (layout.separator_size + layout.gap_size) - 2.f;
    return std::clamp(room / (float(buttons) + 0.35f), std::min(8.f, size), size);
}

// The cut slider and the notifications go to the side of the view opposite to the settings column docked on its right and,
// in the 3D view of Prepare, opposite to the main toolbar laid out against a side (which wins): true: left.
static bool overlays_on_left(GLCanvas3D::ECanvasType type)
{
    const MainToolbarPlace &p = s_main_toolbar_place;
    if (type == GLCanvas3D::ECanvasType::CanvasView3D && p.custom && p.vertical)
        return p.edge > 0;
    return wxGetApp().plater()->get_sidebar_docking_state() == Sidebar::Right;
}

// Side of the main toolbar moved by the user where the windows of the tools open (ImGuiWrapper::set_tool_windows_beside):
// beside a vertical one, under a horizontal one in the upper half of the view, above it in the lower half.
static int main_toolbar_beside_side(float cnv_h)
{
    const MainToolbarPlace &p = s_main_toolbar_place;
    if (!p.custom || !(p.x1 > p.x0))
        return 0;
    if (p.vertical)
        return p.edge < 0 ? 1 : 2;
    return 0.5f * (p.y0 + p.y1) < 0.5f * cnv_h ? 3 : 4;
}

// The largest ratio the rectangle [min, max] of the main toolbar can be scaled by around anchor (on its side that stays),
// staying in the view and off the obstacles it is not over yet (MainToolbarPlace::obstacles); at least 1.
static float main_toolbar_max_ratio(const ImVec2 &min, const ImVec2 &max, const ImVec2 &anchor, float cnv_w, float cnv_h)
{
    // the ratio from which [lo, hi] scaled around c reaches [olo, ohi] (1: already over it)
    const auto reach = [](float lo, float hi, float c, float olo, float ohi) {
        if (lo < ohi && olo < hi)
            return 1.f;
        if (olo >= hi)
            return hi > c ? (olo - c) / (hi - c) : FLT_MAX;
        return lo < c ? (c - ohi) / (c - lo) : FLT_MAX;
    };
    float r = std::min({reach(min.x, max.x, anchor.x, cnv_w, FLT_MAX), reach(min.x, max.x, anchor.x, -FLT_MAX, 0.f),
                        reach(min.y, max.y, anchor.y, cnv_h, FLT_MAX), reach(min.y, max.y, anchor.y, -FLT_MAX, 0.f)});
    for (const ImRect &o : s_main_toolbar_place.obstacles)
        if (!o.Overlaps(ImRect(min, max)))
            r = std::min(r, std::max(reach(min.x, max.x, anchor.x, o.Min.x, o.Max.x), reach(min.y, max.y, anchor.y, o.Min.y, o.Max.y)));
    return std::max(r, 1.f);
}

// The place of a tooltip of size `size` shown for the main toolbar at the mouse, off the toolbar (it hid its buttons): under
// a horizontal toolbar in the upper half of the view, above it in the lower half, beside a vertical one; false: no toolbar.
static bool main_toolbar_tooltip_pos(const ImVec2 &mouse, const ImVec2 &size, float cnv_w, float cnv_h, ImVec2 &pos)
{
    const MainToolbarPlace &p = s_main_toolbar_place;
    if (!(p.x1 > p.x0))
        return false;
    const float gap = 4.f;
    if (p.rect_vertical)
        pos = ImVec2(p.edge < 0 ? p.x1 + gap : p.x0 - gap - size.x, mouse.y - 0.5f * size.y);
    else
        pos = ImVec2(mouse.x, 0.5f * (p.y0 + p.y1) < 0.5f * cnv_h ? p.y1 + gap : p.y0 - gap - size.y);
    pos.x = std::clamp(pos.x, 0.f, std::max(0.f, cnv_w - size.x));
    pos.y = std::clamp(pos.y, 0.f, std::max(0.f, cnv_h - size.y));
    return true;
}

// An ImGui tooltip of the main toolbar (its grip, its sides), off it like the tooltips of its buttons.
static void main_toolbar_imgui_tooltip(const wxString &tip, float cnv_w, float cnv_h)
{
    ImGuiWrapper     &imgui = *wxGetApp().imgui();
    const ImGuiStyle &style = ImGui::GetStyle();
    const ImVec2      text  = imgui.calc_text_size(tip);
    ImVec2            pos;
    if (main_toolbar_tooltip_pos(ImGui::GetIO().MousePos, ImVec2(text.x + 2.f * style.WindowPadding.x, text.y + 2.f * style.WindowPadding.y),
                                 cnv_w, cnv_h, pos))
        ImGui::SetNextWindowPos(pos);
    imgui.tooltip(tip, text.x + 2.f * style.WindowPadding.x);
}

// The grip dragged to the mouse: closer than 1.5 thickness to the left or right side of the view, the toolbar turns vertical
// against it.
static void main_toolbar_drag(const ImVec2 &mouse, float cnv_w, float thick, float grip)
{
    MainToolbarPlace &p        = s_main_toolbar_place;
    const float       snap     = 1.5f * thick;
    const bool        vertical = mouse.x < snap || mouse.x > cnv_w - snap;
    if (vertical != (p.custom && p.vertical))
        // the grip changes its shape: held by its middle
        p.grab = vertical ? ImVec2(0.5f * thick, 0.5f * grip) : ImVec2(0.5f * grip, 0.5f * thick);
    p.custom   = true;
    p.vertical = vertical;
    if (vertical) {
        p.edge = mouse.x < 0.5f * cnv_w ? -1 : 1;
        p.y    = mouse.y - p.grab.y + grip;
    } else {
        p.x = mouse.x - p.grab.x + grip;
        p.y = mouse.y - p.grab.y;
    }
}

// The grip of the main toolbar of the 3D view, left of a horizontal toolbar, above a vertical one: dragged, it moves the
// toolbar (main_toolbar_drag()); double-click: back to its default place, at the top in the middle.
static void render_main_toolbar_grip(GLCanvas3D &canvas, const GLToolbar &toolbar)
{
    MainToolbarPlace &p = s_main_toolbar_place;
    if (!(p.x1 > p.x0))
        return;
    const float cnv_w    = (float)canvas.get_canvas_size().get_width();
    const float thick    = main_toolbar_thickness(toolbar);
    const float grip     = main_toolbar_grip(toolbar);
    const bool  vertical = p.custom && p.vertical;
    ImVec2      gmin, gmax;
    if (vertical) {
        gmin = ImVec2(p.x0, p.y0 - grip);
        gmax = ImVec2(p.x1, p.y0);
        if (gmin.y < 0.f) { // no room above it: under it
            gmin.y = p.y1;
            gmax.y = p.y1 + grip;
        }
    } else {
        gmin = ImVec2(p.x0 - grip, p.y0);
        gmax = ImVec2(p.x0, p.y1);
        if (gmin.x < 0.f) { // no room on its left: on its right
            gmin.x = p.x1;
            gmax.x = p.x1 + grip;
        }
    }
    const ImVec2  size(gmax.x - gmin.x, gmax.y - gmin.y);
    ImGuiWrapper &imgui = *wxGetApp().imgui();
    ImGui::SetNextWindowPos(gmin, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##orio_main_toolbar_grip", nullptr, flags)) {
        const ImGuiIO &io = ImGui::GetIO();
        ImGui::InvisibleButton("##grip", size);
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();
        // on the background of the toolbar, six dots, green when hovered or held
        ImDrawList *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(gmin, gmax, ImGui::GetColorU32(wxGetApp().dark_mode() ? ImGuiWrapper::COL_WINDOW_BG_DARK : ImGuiWrapper::COL_WINDOW_BG),
                            0.3f * grip);
        const ImU32  dot_col = hovered || active ? ImGui::GetColorU32(ImGuiWrapper::COL_BAMBU) : IM_COL32(150, 150, 150, 255);
        const float  radius  = std::max(1.f, 0.09f * grip);
        const float  step    = 0.28f * grip;
        const ImVec2 center(0.5f * (gmin.x + gmax.x), 0.5f * (gmin.y + gmax.y));
        for (int i = -1; i <= 1; ++i)
            for (int j = 0; j < 2; ++j) {
                const float along  = float(i) * 1.6f * step;
                const float across = (float(j) - 0.5f) * step;
                draw->AddCircleFilled(vertical ? ImVec2(center.x + along, center.y + across) : ImVec2(center.x + across, center.y + along), radius, dot_col);
            }
        if (ImGui::IsItemActivated()) {
            p.grab     = ImVec2(io.MousePos.x - gmin.x, io.MousePos.y - gmin.y);
            p.dragging = false;
        }
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            main_toolbar_drag(io.MousePos, cnv_w, thick, grip);
            p.dragging = true;
            canvas.set_as_dirty();
            imgui.set_requires_extra_frame();
        }
        if (ImGui::IsItemDeactivated() && p.dragging) {
            p.dragging = false;
            main_toolbar_place_save();
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            p.custom   = false;
            p.dragging = false;
            main_toolbar_place_save();
            canvas.set_as_dirty();
            imgui.set_requires_extra_frame();
        }
        if (hovered && !active) {
            const wxString tip = _L("Drag to move the toolbar, double-click to put it back in place.\n"
                                    "Hold a tool, then drag it to change the order of the tools.\n"
                                    "Drag a side of the toolbar to make the tools bigger or smaller.");
            main_toolbar_imgui_tooltip(tip, cnv_w, (float)canvas.get_canvas_size().get_height());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

//Orio: the sides of the main toolbar of the 3D view, dragged like the sides of a window: its tools get bigger or
// smaller (MainToolbarPlace::factor, 50 to 200 %), the opposite side staying in place (both ends move for the toolbar at
// its default place, in the middle); a green line on the side under the mouse. Not the sides against the edges of the view
// nor, for a vertical toolbar, its top (its grip). It grows up to the edges of the view and to what is drawn over it
// (main_toolbar_max_ratio()).
static void render_main_toolbar_sides(GLCanvas3D &canvas, const GLToolbar &toolbar)
{
    MainToolbarPlace &p = s_main_toolbar_place;
    // the side being dragged (axis -1: none), and the toolbar when it was taken
    static int    s_axis   = -1;
    static float  s_sign   = 0.f;
    static float  s_factor = 1.f;
    static float  s_grip   = 0.f;
    static float  s_max_factor = 2.f;
    static ImVec2 s_mouse, s_min, s_max;
    if (s_axis >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_axis = -1;
        main_toolbar_place_save();
    }
    if (!(p.x1 > p.x0))
        return;
    const Size  cnv_size = canvas.get_canvas_size();
    const float cnv_w    = (float)cnv_size.get_width();
    const float cnv_h    = (float)cnv_size.get_height();
    const bool  vertical = p.custom && p.vertical;
    const float grip     = main_toolbar_grip(toolbar);
    const float t        = std::max(4.f, std::round(0.25f * grip));
    // the toolbar with its grip (left of a horizontal one, above a vertical one)
    const ImVec2 min(vertical ? p.x0 : p.x0 - grip, vertical ? p.y0 - grip : p.y0);
    const ImVec2 max(p.x1, p.y1);
    struct Side
    {
        const char *name;
        ImVec2      a, b; // its strip, outside the toolbar
        int         axis; // 0: a left or right side, 1: a top or bottom one
        float       sign; // -1: left or top, 1: right or bottom
    };
    std::vector<Side> sides;
    if (min.x >= t)
        sides.push_back({"##orio_main_toolbar_side_l", ImVec2(min.x - t, min.y), ImVec2(min.x, max.y), 0, -1.f});
    if (max.x + t <= cnv_w)
        sides.push_back({"##orio_main_toolbar_side_r", ImVec2(max.x, min.y), ImVec2(max.x + t, max.y), 0, 1.f});
    if (min.y >= t && !vertical)
        sides.push_back({"##orio_main_toolbar_side_t", ImVec2(min.x, min.y - t), ImVec2(max.x, min.y), 1, -1.f});
    if (max.y + t <= cnv_h)
        sides.push_back({"##orio_main_toolbar_side_b", ImVec2(min.x, max.y), ImVec2(max.x, max.y + t), 1, 1.f});

    ImGuiWrapper          &imgui = *wxGetApp().imgui();
    const ImGuiIO         &io    = ImGui::GetIO();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    for (const Side &side : sides) {
        const ImVec2 size(side.b.x - side.a.x, side.b.y - side.a.y);
        ImGui::SetNextWindowPos(side.a, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
        if (ImGui::Begin(side.name, nullptr, flags)) {
            ImGui::InvisibleButton("##side", size);
            const bool hovered = ImGui::IsItemHovered();
            const bool active  = ImGui::IsItemActive();
            if (hovered || active) {
                // a green line along the side of the toolbar
                const float  w  = std::max(2.f, std::round(0.5f * t));
                const ImVec2 la = side.axis == 0 ? ImVec2(side.sign < 0.f ? side.b.x - w : side.a.x, side.a.y) :
                                                   ImVec2(side.a.x, side.sign < 0.f ? side.b.y - w : side.a.y);
                const ImVec2 lb = side.axis == 0 ? ImVec2(la.x + w, side.b.y) : ImVec2(side.b.x, la.y + w);
                ImGui::GetWindowDrawList()->AddRectFilled(la, lb, ImGui::GetColorU32(ImGuiWrapper::COL_BAMBU));
            }
            if (ImGui::IsItemActivated()) {
                s_axis   = side.axis;
                s_sign   = side.sign;
                s_factor = p.factor;
                s_grip   = grip;
                s_mouse  = io.MousePos;
                s_min    = min;
                s_max    = max;
                // the side that stays: the opposite one (the middle for the toolbar at its default place, both ends moving)
                const ImVec2 anchor(!p.custom ? 0.5f * (min.x + max.x) :
                                    vertical  ? (p.edge < 0 ? min.x : max.x) :
                                                (side.axis == 0 && side.sign < 0.f ? max.x : min.x),
                                    side.axis == 1 && side.sign < 0.f ? max.y : min.y);
                s_max_factor = s_factor * main_toolbar_max_ratio(min, max, anchor, cnv_w, cnv_h);
            }
            if (active && s_axis == side.axis && s_sign == side.sign && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const float extent = std::max(1.f, side.axis == 0 ? s_max.x - s_min.x : s_max.y - s_min.y);
                float       delta  = side.sign * (side.axis == 0 ? io.MousePos.x - s_mouse.x : io.MousePos.y - s_mouse.y);
                if (!p.custom && side.axis == 0)
                    delta *= 2.f; // in the middle of the view: both ends move
                const float factor = std::clamp(s_factor * (extent + delta) / extent, 0.5f, std::clamp(s_max_factor, s_factor, 2.f));
                if (std::abs(factor - p.factor) > 0.001f) {
                    p.factor = factor;
                    // a horizontal toolbar moved by the user: its opposite side stays in place
                    if (p.custom && !p.vertical && side.sign < 0.f) {
                        const float ratio = factor / s_factor;
                        if (side.axis == 0)
                            p.x = s_max.x - (s_max.x - (s_min.x + s_grip)) * ratio;
                        else
                            p.y = s_max.y - (s_max.y - s_min.y) * ratio;
                    }
                    canvas.set_as_dirty();
                    imgui.set_requires_extra_frame();
                }
            }
            if (hovered && !active) {
                const wxString tip = _L("Drag to make the tools bigger or smaller");
                main_toolbar_imgui_tooltip(tip, cnv_w, cnv_h);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }
}

int GLCanvas3D::get_main_toolbar_offset() const
{
    const float cnv_width              = get_canvas_size().get_width();
    const float collapse_toolbar_width = get_collapse_toolbar_width() * 2;

    float main_toolbar_width = 0.0f;
    const auto& p_main_toolbar = get_main_toolbar();
    if (p_main_toolbar) {
        main_toolbar_width = p_main_toolbar->get_width();
    }
    const float toolbar_total_width = main_toolbar_width + collapse_toolbar_width;

    if (cnv_width < toolbar_total_width) {
        return is_collapse_toolbar_on_left() ? collapse_toolbar_width : 0;
    } else {
        const float offset = (cnv_width - toolbar_total_width) / 2.f;
        return is_collapse_toolbar_on_left() ? offset + collapse_toolbar_width : offset;
    }
}

float GLCanvas3D::get_main_toolbar_left(int cnv_width,float inv_zoom) const {
    return (-0.5f * cnv_width + get_main_toolbar_offset()) * inv_zoom;
}

int GLCanvas3D::get_main_toolbar_height() const
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return 0;
    }
    //Orio: moved by the user, the toolbar no longer takes a band at the top of the view, unless it was left there.
    const MainToolbarPlace &place = s_main_toolbar_place;
    if (m_canvas_type == ECanvasType::CanvasView3D && place.custom)
        return !place.vertical && place.x1 > place.x0 && place.y0 < 1.f ? int(place.y1) : 0;
    return p_main_toolbar->get_height();
}

float GLCanvas3D::main_toolbar_window_top(float item_top) const
{
    const auto &p_main_toolbar = get_main_toolbar();
    const float height         = p_main_toolbar ? p_main_toolbar->get_height() : 0.f;
    const MainToolbarPlace &place = s_main_toolbar_place;
    if (m_canvas_type != ECanvasType::CanvasView3D || !place.custom || !(place.x1 > place.x0))
        return height;
    if (place.vertical)
        return 0.5f * float(get_canvas_size().get_height()) - item_top * float(get_active_camera().get_zoom());
    return place.y1;
}

int GLCanvas3D::get_main_toolbar_width() const
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return 0;
    }
    return p_main_toolbar->get_width();
}

float GLCanvas3D::get_main_toolbar_scale() const
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return 1.0f;
    }
    return p_main_toolbar->get_scale();
}

bool GLCanvas3D::is_collapse_toolbar_on_left() const
{
    auto state = wxGetApp().plater()->get_sidebar_docking_state();
    return state == Sidebar::Left;
}

float GLCanvas3D::get_collapse_toolbar_width() const
{
    GLToolbar &collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    const auto state            = wxGetApp().plater()->get_sidebar_docking_state();

    return state != Sidebar::None ? collapse_toolbar.get_width() : 0;
}

float GLCanvas3D::get_collapse_toolbar_height() const
{
    GLToolbar &collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    const auto state            = wxGetApp().plater()->get_sidebar_docking_state();

    return state != Sidebar::None ? collapse_toolbar.get_height() : 0;
}

bool GLCanvas3D::make_current_for_postinit() {
    return _set_current(true);
}

void GLCanvas3D::mark_context_dirty()
{
    m_dirty_context = true;
}

void GLCanvas3D::render(bool only_init)
{
    if (m_in_render) {
        // if called recursively, return
        m_dirty = true;
        return;
    }

    m_in_render = true;
    Slic3r::ScopeGuard in_render_guard([this]() { m_in_render = false; });
    (void)in_render_guard;

    if (m_canvas == nullptr)
        return;

    //BBS: add enable_render
    if (!m_enable_render)
        return;

    // ensures this canvas is current and initialized
    if (!_is_shown_on_screen() || !_set_current(true) || !wxGetApp().init_opengl())
        return;

    if (!is_initialized() && !init())
        return;
    if (m_canvas_type == ECanvasType::CanvasView3D  && m_gizmos.get_current_type() == GLGizmosManager::Undefined) {
        enable_return_toolbar(false);
    }
    if (m_canvas_type == ECanvasType::CanvasPreview) {
        auto& t_gcode_viewer = get_gcode_viewer();
        t_gcode_viewer.enable_legend(true);
        t_gcode_viewer.init(wxGetApp().get_mode(), wxGetApp().preset_bundle);
    }

    if (! m_bed.build_volume().valid()) {
        // this happens at startup when no data is still saved under <>\AppData\Roaming\Slic3rPE
        post_event(SimpleEvent(EVT_GLCANVAS_UPDATE_BED_SHAPE));
        return;
    }

    wxGetApp().set_picking_effect(EPickingEffect::Silhouette);
    ColorRGB picking_color;
    if (m_canvas_type == ECanvasType::CanvasAssembleView) {
        picking_color.set(0, 1.0f);
        picking_color.set(1, 1.0f);
        picking_color.set(2, 0.0f);
    }
    else {
        picking_color.set(0, 1.0f);
        picking_color.set(1, 1.0f);
        picking_color.set(2, 1.0f);
    }
    wxGetApp().set_picking_color(picking_color);

    if (only_init)
        return;

#if ENABLE_ENVIRONMENT_MAP
    if (wxGetApp().is_editor())
        wxGetApp().plater()->init_environment_texture();
#endif // ENABLE_ENVIRONMENT_MAP

    const Size& cnv_size = get_canvas_size();
    // Probably due to different order of events on Linux/GTK2, when one switched from 3D scene
    // to preview, this was called before canvas had its final size. It reported zero width
    // and the viewport was set incorrectly, leading to tripping glAsserts further down
    // the road (in apply_projection). That's why the minimum size is forced to 10.
    Camera& camera = get_active_camera();
    camera.apply_viewport(0, 0, std::max(10u, (unsigned int)cnv_size.get_width()), std::max(10u, (unsigned int)cnv_size.get_height()));

    if (camera.requires_zoom_to_bed) {
        zoom_to_bed();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_bed = false;
    }

    if (camera.requires_zoom_to_plate > REQUIRES_ZOOM_TO_PLATE_IDLE) {
        zoom_to_plate(camera.requires_zoom_to_plate);
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_plate = REQUIRES_ZOOM_TO_PLATE_IDLE;
    }

    if (camera.requires_zoom_to_volumes) {
        zoom_to_volumes();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_volumes = false;
    }

    camera.apply_projection(_max_bounding_box(true, true, true,is_volumes_limit_to_expand_plate()));
    camera.update_frustum();

    m_frame_callback_list.clear();

    const std::array<int, 4>& viewport = camera.get_viewport();
    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    if (!p_ogl_manager) {
        return;
    }

    if (m_main_toolbar) {
        if (m_canvas_type == ECanvasType::CanvasView3D) {
            const auto toolbar_style = p_ogl_manager->get_toolbar_rendering_style();
            m_main_toolbar->set_rendering_mode(static_cast<GLToolbar::EToolbarRenderingMode>(toolbar_style));
        }
    }

    auto& ogl_manager = *p_ogl_manager;
    ogl_manager.set_viewport_size(viewport[2], viewport[3]);

    ogl_manager.bind_vao();

    wxGetApp().imgui()->new_frame();

    //Orio: Print of the tab bar centred on the title line of the Slicing Result legend of the Preview (its width and
    // right margin as in BaseRenderer::render_legend and _render_gcode), in both tabs, before any slicing.
    if ((m_canvas_type == ECanvasType::CanvasView3D || m_canvas_type == ECanvasType::CanvasPreview) && wxGetApp().mainframe != nullptr) {
        ImGuiWrapper     &imgui  = *wxGetApp().imgui();
        const float       s      = get_scale();
        const std::string title  = _u8L("Slicing Result");
        const float       normal = imgui.calc_text_size(title).x;
        float             bold   = normal;
        if (imgui.push_bold_font()) {
            bold = imgui.calc_text_size(title).x;
            imgui.pop_bold_font();
        }
        const float line  = 4.0f * s + ImGui::GetStyle().ItemSpacing.x + bold + (imgui.calc_text_size("ABCD").x + 18.0f * s) * 8.0f - normal +
                            ImGui::CalcTextSize(into_u8(std::wstring(1, ImGui::FoldButtonIcon)).c_str()).x;
        const int   right = int(overlays_on_left(ECanvasType::CanvasPreview) ? SLIDER_DEFAULT_RIGHT_MARGIN + get_collapse_toolbar_width() / s : SLIDER_RIGHT_MARGIN);
        wxGetApp().mainframe->center_print_button_on(
            m_canvas->ClientToScreen(wxPoint(int((cnv_size.get_width() - right * s - 0.5f * line) / cnv_size.get_scale_factor()), 0)).x);
    }

    //Orio: the windows of the tools open beside the main toolbar moved by the user (its place at the last frame).
    {
        const MainToolbarPlace &place = s_main_toolbar_place;
        ImGuiWrapper::set_tool_windows_beside(m_canvas_type == ECanvasType::CanvasView3D ? main_toolbar_beside_side(float(cnv_size.get_height())) : 0,
                                              place.x0, place.y0, place.x1, place.y1);
    }

    //Orio: cut X/Y/Z of the Prepare tab, updated before any rendering of the objects (picking included).
    m_prepare_cut.update(m_volumes, m_canvas_type == ECanvasType::CanvasView3D ? wxGetApp().plater()->get_partplate_list().get_curr_plate() : nullptr,
                         _is_prepare_cut_allowed());
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        _update_modifiers_colors();
        _update_support_enforcers_colors();
        _update_sinking_contours_colors();
    }

    if (m_picking_enabled) {
        if (m_rectangle_selection.is_dragging())
            // picking pass using rectangle selection
            _rectangular_selection_picking_pass();
        //BBS: enable picking when no volumes for partplate logic
        //else if (!m_volumes.empty())
        else
            // regular picking pass
            _picking_pass();
    }
    glsafe(::glViewport(viewport[0], viewport[1], viewport[2], viewport[3]));
    EPickingEffect picking_effect = wxGetApp().get_picking_effect();
    if (EPickingEffect::Disabled != picking_effect) {
        if (!ogl_manager.are_framebuffers_supported()) {
            picking_effect = EPickingEffect::StencilOutline; // use stencil outline as framebuffer not supported yet.
        }
        if (!m_gizmos.is_allow_show_volume_highlight_outline()) {
            picking_effect = EPickingEffect::Disabled;
        }
    }

    const bool off_screen_rendering_enabled = ogl_manager.is_fxaa_enabled();

    if (m_picking_enabled && EPickingEffect::Silhouette == picking_effect) {
        _render_silhouette_effect();
    }

    std::string write_to_framebuffer_name{};
    if (off_screen_rendering_enabled) {
        write_to_framebuffer_name = "mainframe";
        OpenGLManager::FrameBufferModifier main_frame(ogl_manager, write_to_framebuffer_name, ogl_manager.get_msaa_type());
    }
    else {
        write_to_framebuffer_name = OpenGLManager::s_back_frame;
        OpenGLManager::FrameBufferModifier main_frame(ogl_manager, write_to_framebuffer_name);
    }

#if ENABLE_RENDER_PICKING_PASS
    if (!m_picking_enabled || !m_show_picking_texture) {
#endif // ENABLE_RENDER_PICKING_PASS
    // draw scene
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    _render_background();

    //BBS add partplater rendering logic
    bool only_current = false, only_body = false, show_axes = true, no_partplate = false;
    bool show_grid = true;
    GLGizmosManager::EType gizmo_type = m_gizmos.get_current_type();
    if (m_canvas_type != CanvasView3D || m_gizmos.is_show_only_active_plate()) {
        //only_body = true;
        if (m_gizmos.get_object_located_outside_plate()) {
            no_partplate = true;
        }
        else {
            only_current = true;
        }
    } else if (m_gizmos.get_current_type() == GLGizmosManager::EType::Text) {
        if (m_gizmos.is_only_text_volume()) {
            only_current = false;
        } else {
            only_current = true;
        }
    } else if (m_gizmos.is_paint_gizmo())
        no_partplate = true;
    else if (gizmo_type == GLGizmosManager::BrimEars && !camera.is_looking_downward())
        show_grid = false;

    /* view3D render*/
    int hover_id = (m_hover_plate_idxs.size() > 0) ? m_hover_plate_idxs.front() : -1;
    bool b_with_stencil_outline = !m_gizmos.is_running() && (EPickingEffect::StencilOutline == picking_effect);
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        //BBS: add outline logic
        _render_objects(m_volumes,GLVolumeCollection::ERenderType::Opaque, b_with_stencil_outline);
        if (!m_paint_outline_volumes.empty()) {
            _render_objects(m_paint_outline_volumes, GLVolumeCollection::ERenderType::Opaque, b_with_stencil_outline,true);
        }
        _render_sla_slices();
        _render_selection();
        if (!no_partplate)
            _render_bed(!camera.is_looking_downward(), show_axes);
        if (!no_partplate) //BBS: add outline logic
            _render_platelist(!camera.is_looking_downward(), only_current, only_body, hover_id, true, show_grid);
        //Orio: supports of the last slicing, before the (transparent) modifiers to see them inside.
        _render_sliced_supports();
        _render_objects(m_volumes, GLVolumeCollection::ERenderType::Transparent, b_with_stencil_outline);
        _render_objects(m_paint_outline_volumes, GLVolumeCollection::ERenderType::Transparent, b_with_stencil_outline, true);
    }
    /* preview render */
    else if (m_canvas_type == ECanvasType::CanvasPreview && m_render_preview) {
        _render_objects(m_volumes, GLVolumeCollection::ERenderType::Opaque, b_with_stencil_outline);
        _render_sla_slices();
        _render_selection();
        _render_bed(!camera.is_looking_downward(), show_axes);
        _render_platelist(!camera.is_looking_downward(), only_current, true, hover_id);
        // BBS: GUI refactor: add canvas size as parameters
        _render_gcode(cnv_size.get_width(), cnv_size.get_height());
        _render_picked_object_frame();
    }
    /* assemble render*/
    else if (m_canvas_type == ECanvasType::CanvasAssembleView) {
        //BBS: add outline logic
        if (m_show_world_axes) {
            m_axes.render();
        }
        _render_objects(m_volumes, GLVolumeCollection::ERenderType::Opaque, b_with_stencil_outline);
        //_render_bed(!camera.is_looking_downward(), show_axes);
        _render_plane();
        _render_bvh_primary_bounds();
        //BBS: add outline logic insteadof selection under assemble view
        //_render_selection();
        // BBS: add outline logic
        _render_objects(m_volumes, GLVolumeCollection::ERenderType::Transparent, b_with_stencil_outline);
    }

     if (m_picking_enabled && EPickingEffect::Silhouette == picking_effect) {
         _composite_silhouette_effect();
     }

    _render_sequential_clearance();
#if ENABLE_RENDER_SELECTION_CENTER
    _render_selection_center();
#endif // ENABLE_RENDER_SELECTION_CENTER

    // sidebar hints need to be rendered before the gizmos because the depth buffer
    // could be invalidated by the following gizmo render methods
    _render_selection_sidebar_hints();
    _render_current_gizmo();

    _rebuild_postprocessing_pipeline(p_ogl_manager, write_to_framebuffer_name, OpenGLManager::s_back_frame, viewport[2], viewport[3]);

#if ENABLE_RENDER_PICKING_PASS
    }
#endif // ENABLE_RENDER_PICKING_PASS

#if ENABLE_SHOW_CAMERA_TARGET
    _render_camera_target();
#endif // ENABLE_SHOW_CAMERA_TARGET
    if (m_picking_enabled && m_rectangle_selection.is_dragging())
        m_rectangle_selection.render(*this);

    // draw overlays
    _render_overlays();

    if (wxGetApp().plater()->is_render_statistic_dialog_visible()) {
        ImGui::ShowMetricsWindow();

        ImGuiWrapper& imgui = *wxGetApp().imgui();
        imgui.begin(std::string("Render statistics"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        imgui.text("FPS (SwapBuffers() calls per second):");
        ImGui::SameLine();
        imgui.text(std::to_string(m_render_stats.get_fps_and_reset_if_needed()));
        ImGui::Separator();
        imgui.text("Compressed textures:");
        ImGui::SameLine();
        imgui.text(OpenGLManager::are_compressed_textures_supported() ? "supported" : "not supported");
        imgui.text("Max texture size:");
        ImGui::SameLine();
        imgui.text(std::to_string(OpenGLManager::get_gl_info().get_max_tex_size()));
        imgui.end();
    }

#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
    if (wxGetApp().is_editor() && wxGetApp().plater()->is_view3D_shown())
        wxGetApp().plater()->render_project_state_debug_window();
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

    if (wxGetApp().plater()->is_render_statistic_dialog_visible()) {
        camera.debug_render();
        camera.debug_frustum();
    }

#if ENABLE_IMGUI_STYLE_EDITOR
    if (wxGetApp().get_mode() == ConfigOptionMode::comDevelop)
        _render_style_editor();
#endif


    std::string tooltip;
    bool        main_toolbar_tooltip = false; //Orio: shown off the main toolbar of the 3D view

	// Negative coordinate means out of the window, likely because the window was deactivated.
	// In that case the tooltip should be hidden.
    if (m_mouse.position.x() >= 0. && m_mouse.position.y() >= 0.) {
        if (tooltip.empty())
            tooltip = m_layers_editing.get_tooltip(*this);

        //BBS: variable print speed along Z.
        if (tooltip.empty())
            tooltip = m_speed_editing.get_tooltip(*this);

        //Orio: variable sparse infill density / wall count along Z.
        if (tooltip.empty())
            tooltip = m_infill_walls_editing.get_tooltip(*this);

	    if (tooltip.empty())
	        tooltip = m_gizmos.get_tooltip();

        if (tooltip.empty()) {
            const auto& p_main_toolbar = get_main_toolbar();
            if (p_main_toolbar) {
                tooltip = p_main_toolbar->get_tooltip();
                main_toolbar_tooltip = !tooltip.empty();
            }
        }

	    if (tooltip.empty())
            tooltip = wxGetApp().plater()->get_collapse_toolbar().get_tooltip();

        // BBS
#if 0
	    if (tooltip.empty())
            tooltip = wxGetApp().plater()->get_view_toolbar().get_tooltip();
#endif
    }

    set_tooltip(tooltip);

    if (m_tooltip_enabled) {
        float offset_x = std::atof(wxGetApp().app_config->get("3d_middle_tooltip_offset_x").c_str());
        float offset_y = std::atof(wxGetApp().app_config->get("3d_middle_tooltip_offset_y").c_str());
        Vec2d position = m_mouse.position + Vec2d(offset_x,offset_y);
        //Orio: the tooltip of a button of the main toolbar of the 3D view off the toolbar (it hid its other buttons);
        // Tooltip::render() shows it 16 pixels lower than asked.
        if (main_toolbar_tooltip && m_canvas_type == ECanvasType::CanvasView3D) {
            const ImGuiStyle &style = ImGui::GetStyle();
            const ImVec2      text  = ImGui::CalcTextSize(tooltip.c_str());
            const Size        cnv   = get_canvas_size();
            ImVec2            pos;
            if (main_toolbar_tooltip_pos(ImVec2((float)m_mouse.position.x(), (float)m_mouse.position.y()),
                                         ImVec2(text.x + 2.f * style.WindowPadding.x, text.y + 2.f * style.WindowPadding.y),
                                         (float)cnv.get_width(), (float)cnv.get_height(), pos))
                position = Vec2d(pos.x, pos.y - 16.0);
        }
        m_tooltip.render(position, *this);
    }

    wxGetApp().plater()->get_mouse3d_controller().render_settings_dialog(*this);

    //Orio: the notifications of a print being sent, at the bottom center: above the horizontal slider of the
    // preview, and above the "Sliced supports" check box of the prepare view (and the bars X / Y of the tools).
    wxGetApp().plater()->get_notification_manager()->set_center_bottom_margin(
        SLIDER_BOTTOM_MARGIN + (m_infill_walls_editing.bottom_bar_height(*this) + m_speed_editing.bottom_bar_height(*this)) / get_scale());

    float right_margin = SLIDER_DEFAULT_RIGHT_MARGIN;
    float bottom_margin = SLIDER_DEFAULT_BOTTOM_MARGIN;
    if (m_canvas_type == ECanvasType::CanvasPreview) {
        right_margin = SLIDER_RIGHT_MARGIN;
        bottom_margin = SLIDER_BOTTOM_MARGIN;
    }
    // Orio : dans Préparation, les notifications s'écartent du réglet de coupe X/Y/Z (bord droit, sur toute la
    // hauteur) comme elles s'écartent du curseur des couches dans l'Aperçu : sinon leur croix passe derrière.
    else if (_is_prepare_cut_allowed())
        right_margin = SLIDER_RIGHT_MARGIN;
    //Orio: above the bar of the variable infill / walls tool painting along X / Y, at the bottom of the scene
    // (the margin is scaled by the notification manager). The same for the variable speed tool; only one tool is open.
    bottom_margin += (m_infill_walls_editing.bottom_bar_height(*this) + m_speed_editing.bottom_bar_height(*this)) / get_scale();
    //Orio: on the left of the panel of the variable infill / walls tool painting along Z, at the right of the scene.
    // The same for the variable speed and the variable layer height tools.
    right_margin += (m_infill_walls_editing.right_bar_width(*this) + m_speed_editing.right_bar_width(*this) + m_layers_editing.right_bar_width(*this)) / get_scale();
    //Orio: with the cut slider on the left of the view (overlays_on_left()), the notifications too: right of the slider,
    // of the plates of the preview and of the view cube.
    float left_margin = -1.f;
    if ((m_canvas_type == CanvasView3D || m_canvas_type == CanvasPreview) && overlays_on_left(m_canvas_type)) {
        left_margin = m_canvas_type == CanvasPreview || _is_prepare_cut_allowed() ? SLIDER_RIGHT_MARGIN : SLIDER_DEFAULT_RIGHT_MARGIN;
        if (m_canvas_type == CanvasPreview)
            left_margin += m_sel_plate_toolbar_width / get_scale();
        if (wxGetApp().show_3d_navigator() && can_show_3d_navigator())
            left_margin = std::max(left_margin, 128.f * m_sc / get_scale() + SLIDER_DEFAULT_RIGHT_MARGIN);
    }
    wxGetApp().plater()->get_notification_manager()->set_left_margin(left_margin);
    //Orio: main toolbar moved to the lower half of the 3D view: the notifications at the top, below the "Return"
    // button of the tools when it is shown.
    float top_margin = -1.f;
    if (m_canvas_type == CanvasView3D && main_toolbar_beside_side(float(get_canvas_size().get_height())) == 4)
        top_margin = SLIDER_DEFAULT_BOTTOM_MARGIN + m_return_button_bottom / get_scale();
    wxGetApp().plater()->get_notification_manager()->set_top_margin(top_margin);
    wxGetApp().plater()->get_notification_manager()->render_notifications(*this, get_overlay_window_width(), bottom_margin, right_margin);
    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        wxGetApp().plater()->get_dailytips()->render();
    }

    wxGetApp().imgui()->render();
    ogl_manager.unbind_vao();
    ogl_manager.clear_dirty();
    m_canvas->SwapBuffers();

    for (const auto& cb : m_frame_callback_list) {
        cb();
    }

    m_render_stats.increment_fps_counter();
}

void GLCanvas3D::render_thumbnail(ThumbnailData &         thumbnail_data,
                                  unsigned int            w,
                                  unsigned int            h,
                                  const ThumbnailsParams &thumbnail_params,
                                  Camera::EType           camera_type,
                                  Camera::ViewAngleType   camera_view_angle_type,
                                  bool                    for_picking,
                                  bool                    ban_light,
                                  const ExtraThumbData&   extra_thumb_data)
{
    ModelObjectPtrs &model_objects = GUI::wxGetApp().model().objects;
    std::vector<std::array<float, 4>> colors = wxGetApp().plater()->get_extruders_colors();
    render_thumbnail(thumbnail_data, colors, w, h, thumbnail_params, model_objects, m_volumes, camera_type, camera_view_angle_type, for_picking, ban_light, extra_thumb_data);
}

void GLCanvas3D::render_thumbnail(ThumbnailData &                    thumbnail_data,
                                  std::vector<std::array<float, 4>> &extruder_colors,
                                  unsigned int                       w,
                                  unsigned int                       h,
                                  const ThumbnailsParams &           thumbnail_params,
                                  ModelObjectPtrs &                  model_objects,
                                  const GLVolumeCollection &         volumes,
                                  Camera::EType                      camera_type,
                                  Camera::ViewAngleType              camera_view_angle_type,
                                  bool                               for_picking,
                                  bool                               ban_light,
                                  const ExtraThumbData&              extra_thumb_data)
{
    const auto& shader = wxGetApp().get_shader("thumbnail");
    if (!shader) {
        return;
    }

    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    p_ogl_manager->bind_vao();
    p_ogl_manager->bind_shader(shader);

    render_thumbnail_framebuffer(p_ogl_manager, thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, extruder_colors,
                                 shader, camera_type, camera_view_angle_type, for_picking, ban_light, extra_thumb_data);


    p_ogl_manager->unbind_shader();
    p_ogl_manager->unbind_vao();
}

void GLCanvas3D::render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params)
{
    //load current plate gcode
    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    if (!p_ogl_manager) {
        return;
    }
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.render_calibration_thumbnail(thumbnail_data, w, h, thumbnail_params,
        wxGetApp().plater()->get_partplate_list(), *p_ogl_manager);
}

//BBS
void GLCanvas3D::select_curr_plate_all()
{
    m_selection.add_curr_plate();
    m_dirty = true;
}

void GLCanvas3D::select_object_from_idx(std::vector<int>& object_idxs) {
    m_selection.add_object_from_idx(object_idxs);
    m_dirty = true;
}

//BBS
void GLCanvas3D::remove_curr_plate_all()
{
    m_selection.remove_curr_plate();
    m_dirty = true;
}

void GLCanvas3D::select_all()
{
    if (!m_gizmos.is_allow_select_all()) {
        return;
    }
    m_selection.add_all();
    m_dirty = true;
}

void GLCanvas3D::deselect_all()
{
    m_selection.remove_all();
    // BBS
    //wxGetApp().obj_manipul()->set_dirty();
    m_gizmos.reset_all_states();
    m_gizmos.update_data();
    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
}

void GLCanvas3D::exit_gizmo() {
    if (m_gizmos.get_current_type() != GLGizmosManager::Undefined) {
        m_gizmos.reset_all_states();
        m_gizmos.update_data();
    }
}

void GLCanvas3D::set_selected_visible(bool visible)
{
    for (unsigned int i : m_selection.get_volume_idxs()) {
        GLVolume* volume = const_cast<GLVolume*>(m_selection.get_volume(i));
        volume->visible = visible;
        volume->color[3] = visible ? 1.f : GLVolume::MODEL_HIDDEN_COL[3];
        volume->render_color[3] = volume->color[3];
        volume->force_native_color = !visible;
    }
    m_dirty = true;
}

void GLCanvas3D::delete_selected()
{
    m_selection.erase();
}

void GLCanvas3D::ensure_on_bed(unsigned int object_idx, bool allow_negative_z)
{
    //BBS if asseble view canvas
    if (m_canvas_type == ECanvasType::CanvasAssembleView) {
        return;
    }

    if (allow_negative_z)
        return;

    typedef std::map<std::pair<int, int>, double> InstancesToZMap;
    InstancesToZMap instances_min_z;

    for (GLVolume* volume : m_volumes.volumes) {
        if (volume->object_idx() == (int)object_idx && !volume->is_modifier) {
            double min_z = volume->transformed_convex_hull_bounding_box().min.z();
            std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::iterator it = instances_min_z.find(instance);
            if (it == instances_min_z.end())
                it = instances_min_z.insert(InstancesToZMap::value_type(instance, DBL_MAX)).first;

            it->second = std::min(it->second, min_z);
        }
    }

    for (GLVolume* volume : m_volumes.volumes) {
        std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
        InstancesToZMap::iterator it = instances_min_z.find(instance);
        if (it != instances_min_z.end())
            volume->set_instance_offset(Z, volume->get_instance_offset(Z) - it->second);
    }
}

bool GLCanvas3D::is_gcode_legend_enabled() const
{
    auto& t_gcode_viewer = get_gcode_viewer();
    return t_gcode_viewer.is_legend_enabled();
}

std::vector<double> GLCanvas3D::get_gcode_layers_zs() const
{
    const auto& t_gcode_viewer = get_gcode_viewer();
    const auto& t_layers = t_gcode_viewer.get_layers_zs();
    std::vector<double> layer_z_list;
    layer_z_list.reserve(t_layers.size());
    for (int i = 0; i < t_layers.size(); ++i) {
        layer_z_list.emplace_back(t_layers[i]);
    }
    return layer_z_list;
}

int GLCanvas3D::get_gcode_layers_count() const
{
    const auto& t_gcode_viewer = get_gcode_viewer();
    return t_gcode_viewer.get_layers_zs().size();
}

std::vector<double> GLCanvas3D::get_volumes_print_zs(bool active_only) const
{
    return m_volumes.get_current_print_zs(active_only);
}

void GLCanvas3D::set_gcode_options_visibility_from_flags(unsigned int flags)
{
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.set_options_visibility_from_flags(flags);
}

unsigned int GLCanvas3D::get_gcode_options_visibility_flags() const
{
    const auto& t_gcode_viewer = get_gcode_viewer();
    return t_gcode_viewer.get_options_visibility_flags();
}

void GLCanvas3D::set_volumes_z_range(const std::array<double, 2>& range)
{
    m_volumes.set_range(range[0] - 1e-6, range[1] + 1e-6);
}

std::vector<CustomGCode::Item>& GLCanvas3D::get_custom_gcode_per_print_z() const
{
    const auto& t_gcode_viewer = get_gcode_viewer();
    return t_gcode_viewer.get_custom_gcode_per_print_z();
}

size_t GLCanvas3D::get_gcode_extruders_count() const
{
    const auto& t_gcode_viewer = get_gcode_viewer();
    return t_gcode_viewer.get_extruders_count();
}

std::vector<int> GLCanvas3D::load_object(const ModelObject& model_object, int obj_idx, std::vector<int> instance_idxs, bool lod_enabled)
{
    if (instance_idxs.empty()) {
        for (unsigned int i = 0; i < model_object.instances.size(); ++i) {
            instance_idxs.emplace_back(i);
        }
    }
    return m_volumes.load_object(&model_object, obj_idx, instance_idxs, m_color_by, m_initialized, lod_enabled);
}

std::vector<int> GLCanvas3D::load_object(const Model& model, int obj_idx, bool lod_enabled)
{
    if (0 <= obj_idx && obj_idx < (int)model.objects.size()) {
        const ModelObject* model_object = model.objects[obj_idx];
        if (model_object != nullptr)
            return load_object(*model_object, obj_idx, std::vector<int>(), lod_enabled);
    }

    return std::vector<int>();
}

void GLCanvas3D::mirror_selection(Axis axis)
{
    TransformationType transformation_type;
    //transformation_type.set_world();
    transformation_type.set_relative();
    m_selection.setup_cache();
    m_selection.mirror(axis, transformation_type);
    do_mirror(L("Mirror Object"));
    // BBS
    //wxGetApp().obj_manipul()->set_dirty();
}

// Reload the 3D scene of
// 1) Model / ModelObjects / ModelInstances / ModelVolumes
// 2) Print bed
// 3) SLA support meshes for their respective ModelObjects / ModelInstances
// 4) Wipe tower preview
// 5) Out of bed collision status & message overlay (texture)
void GLCanvas3D::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas == nullptr || m_config == nullptr || m_model == nullptr)
        return;

    if (!m_initialized)
        return;

    _set_current(true);

    m_hover_volume_idxs.clear();

    struct ModelVolumeState {
        ModelVolumeState(const GLVolume* volume) :
            model_volume(nullptr), geometry_id(volume->geometry_id), volume_idx(-1) {}
        ModelVolumeState(const ModelVolume* model_volume, const ObjectID& instance_id, const GLVolume::CompositeID& composite_id) :
            model_volume(model_volume), geometry_id(std::make_pair(model_volume->id().id, instance_id.id)), composite_id(composite_id), volume_idx(-1) {}
        ModelVolumeState(const ObjectID& volume_id, const ObjectID& instance_id) :
            model_volume(nullptr), geometry_id(std::make_pair(volume_id.id, instance_id.id)), volume_idx(-1) {}
        bool new_geometry() const { return this->volume_idx == size_t(-1); }
        const ModelVolume* model_volume;
        // ObjectID of ModelVolume + ObjectID of ModelInstance
        // or timestamp of an SLAPrintObjectStep + ObjectID of ModelInstance
        std::pair<size_t, size_t>   geometry_id;
        GLVolume::CompositeID       composite_id;
        // Volume index in the new GLVolume vector.
        size_t                      volume_idx;
    };
    std::vector<ModelVolumeState> model_volume_state;
    std::vector<ModelVolumeState> aux_volume_state;

    struct GLVolumeState {
        GLVolumeState() :
            volume_idx(size_t(-1)) {}
        GLVolumeState(const GLVolume* volume, unsigned int volume_idx) :
            composite_id(volume->composite_id), volume_idx(volume_idx) {}
        GLVolumeState(const GLVolume::CompositeID &composite_id) :
            composite_id(composite_id), volume_idx(size_t(-1)) {}

        GLVolume::CompositeID       composite_id;
        // Volume index in the old GLVolume vector.
        size_t                      volume_idx;
    };

    // SLA steps to pull the preview meshes for.
	typedef std::array<SLAPrintObjectStep, 3> SLASteps;
    SLASteps sla_steps = { slaposDrillHoles, slaposSupportTree, slaposPad };
    struct SLASupportState {
        std::array<PrintStateBase::StateWithTimeStamp, std::tuple_size<SLASteps>::value> step;
    };
    // State of the sla_steps for all SLAPrintObjects.
    std::vector<SLASupportState>   sla_support_state;

    std::vector<size_t> instance_ids_selected;
    std::vector<size_t> map_glvolume_old_to_new(m_volumes.volumes.size(), size_t(-1));
    std::vector<GLVolumeState> deleted_volumes;
    // BBS
    std::vector<GLVolumeState> deleted_wipe_towers;
    std::vector<GLVolume*> glvolumes_new;
    glvolumes_new.reserve(m_volumes.volumes.size());
    auto model_volume_state_lower = [](const ModelVolumeState& m1, const ModelVolumeState& m2) { return m1.geometry_id < m2.geometry_id; };

    m_reload_delayed = !m_canvas->IsShown() && !refresh_immediately && !force_full_scene_refresh;

    PrinterTechnology printer_technology = current_printer_technology();

    // BBS: support wipe tower for multi-plates
    PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
    int n_plates = ppl.get_plate_count();
    std::vector<int> volume_idxs_wipe_tower_old(n_plates, -1);

    // Release invalidated volumes to conserve GPU memory in case of delayed refresh (see m_reload_delayed).
    // First initialize model_volumes_new_sorted & model_instances_new_sorted.
    for (int object_idx = 0; object_idx < (int)m_model->objects.size(); ++object_idx) {
        const ModelObject* model_object = m_model->objects[object_idx];
        for (int instance_idx = 0; instance_idx < (int)model_object->instances.size(); ++instance_idx) {
            const ModelInstance* model_instance = model_object->instances[instance_idx];
            for (int volume_idx = 0; volume_idx < (int)model_object->volumes.size(); ++volume_idx) {
                const ModelVolume* model_volume = model_object->volumes[volume_idx];
                if (m_canvas_type == ECanvasType::CanvasAssembleView) {
                    if (model_volume->is_model_part())
                        model_volume_state.emplace_back(model_volume, model_instance->id(), GLVolume::CompositeID(object_idx, volume_idx, instance_idx));
                }
                else {
                    model_volume_state.emplace_back(model_volume, model_instance->id(), GLVolume::CompositeID(object_idx, volume_idx, instance_idx));
                }
            }
        }
    }
    if (printer_technology == ptSLA) {
        const SLAPrint* sla_print = this->sla_print();
#ifndef NDEBUG
        // Verify that the SLAPrint object is synchronized with m_model.
        check_model_ids_equal(*m_model, sla_print->model());
#endif /* NDEBUG */
        sla_support_state.reserve(sla_print->objects().size());
        for (const SLAPrintObject* print_object : sla_print->objects()) {
            SLASupportState state;
            for (size_t istep = 0; istep < sla_steps.size(); ++istep) {
                state.step[istep] = print_object->step_state_with_timestamp(sla_steps[istep]);
                if (state.step[istep].state == PrintStateBase::DONE) {
                    if (!print_object->has_mesh(sla_steps[istep]))
                        // Consider the DONE step without a valid mesh as invalid for the purpose
                        // of mesh visualization.
                        state.step[istep].state = PrintStateBase::INVALID;
                    else if (sla_steps[istep] != slaposDrillHoles)
                        for (const ModelInstance* model_instance : print_object->model_object()->instances)
                            // Only the instances, which are currently printable, will have the SLA support structures kept.
                            // The instances outside the print bed will have the GLVolumes of their support structures released.
                            if (model_instance->is_printable())
                                aux_volume_state.emplace_back(state.step[istep].timestamp, model_instance->id());
                }
            }
            sla_support_state.emplace_back(state);
        }
    }
    std::sort(model_volume_state.begin(), model_volume_state.end(), model_volume_state_lower);
    std::sort(aux_volume_state.begin(), aux_volume_state.end(), model_volume_state_lower);

    // BBS: normalize painting data with current filament count
    for (unsigned int obj_idx = 0; obj_idx < (unsigned int)m_model->objects.size(); ++obj_idx) {
        const ModelObject& model_object = *m_model->objects[obj_idx];
        for (int volume_idx = 0; volume_idx < (int)model_object.volumes.size(); ++volume_idx) {
            ModelVolume& model_volume = *model_object.volumes[volume_idx];
            if (!model_volume.is_model_part())
                continue;

            // Use project_config as the source of truth for filament count,
            // because m_config->filament_colour may be truncated by full_config()
            // which only merges filament_presets (physical filaments).
            // project_config includes mixed filament slots.
            unsigned int filaments_count_m_config = (unsigned int)dynamic_cast<const ConfigOptionStrings*>(m_config->option("filament_colour"))->values.size();
            auto* proj_colour_opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            unsigned int filaments_count = proj_colour_opt
                ? std::max(filaments_count_m_config, (unsigned int)proj_colour_opt->values.size())
                : filaments_count_m_config;
            model_volume.update_extruder_count(filaments_count);
        }
    }

    // Release all ModelVolume based GLVolumes not found in the current Model. Find the GLVolume of a hollowed mesh.
    for (size_t volume_id = 0; volume_id < m_volumes.volumes.size(); ++volume_id) {
        GLVolume* volume = m_volumes.volumes[volume_id];
        ModelVolumeState  key(volume);
        ModelVolumeState* mvs = nullptr;
        if (volume->volume_idx() < 0) {
            auto it = std::lower_bound(aux_volume_state.begin(), aux_volume_state.end(), key, model_volume_state_lower);
            if (it != aux_volume_state.end() && it->geometry_id == key.geometry_id)
                // This can be an SLA support structure that should not be rendered (in case someone used undo
                // to revert to before it was generated). We only reuse the volume if that's not the case.
                if (m_model->objects[volume->composite_id.object_id]->sla_points_status != sla::PointsStatus::NoPoints)
                    mvs = &(*it);
        }
        else {
            auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
            if (it != model_volume_state.end() && it->geometry_id == key.geometry_id)
                mvs = &(*it);
        }
        // Emplace instance ID of the volume. Both the aux volumes and model volumes share the same instance ID.
        // The wipe tower has its own wipe_tower_instance_id().
        if (m_selection.contains_volume(volume_id)) {
            if (m_canvas_type == ECanvasType::CanvasAssembleView) {
                if (!volume->is_modifier)
                    instance_ids_selected.emplace_back(volume->geometry_id.second);
            }
            else {
                instance_ids_selected.emplace_back(volume->geometry_id.second);
            }
        }
        if (mvs == nullptr || force_full_scene_refresh) {
            // This GLVolume will be released.
            if (volume->is_wipe_tower) {
                // There is only one wipe tower.
                //assert(volume_idx_wipe_tower_old == -1);
                int plate_id = volume->composite_id.object_id - 1000;
                if (plate_id < n_plates)
                    volume_idxs_wipe_tower_old[plate_id] = (int)volume_id;
            }
            if (!m_reload_delayed) {
                deleted_volumes.emplace_back(volume, volume_id);
                // BBS
                if (volume->is_wipe_tower)
                    deleted_wipe_towers.emplace_back(volume, volume_id);
                m_volumes.release_volume(volume);
                delete volume;
            }
        }
        else {
            // This GLVolume will be reused.
            volume->set_sla_shift_z(0.0);
            map_glvolume_old_to_new[volume_id] = glvolumes_new.size();
            mvs->volume_idx = glvolumes_new.size();
            glvolumes_new.emplace_back(volume);
            // Update color of the volume based on the current extruder.
            if (mvs->model_volume != nullptr) {
                int extruder_id = mvs->model_volume->extruder_id();
                if (extruder_id != -1)
                    volume->extruder_id = extruder_id;

                volume->is_modifier = !mvs->model_volume->is_model_part();
                volume->is_interval = mvs->model_volume->is_interval();
                volume->set_color(color_from_model_volume(*mvs->model_volume));

                // updates volumes transformations
                if (m_canvas_type != ECanvasType::CanvasAssembleView) {
                    volume->set_instance_transformation(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_transformation());
                    volume->set_volume_transformation(mvs->model_volume->get_transformation());
                    // updates volumes convex hull
                    if (mvs->model_volume->is_model_part() && ! volume->convex_hull())
                        // Model volume was likely changed from modifier or support blocker / enforcer to a model part.
                        // Only model parts require convex hulls.
                        volume->set_convex_hull(mvs->model_volume->get_convex_hull_shared_ptr());
                    volume->set_offset_to_assembly(Vec3d(0, 0, 0));
                }
                else {
                    volume->set_instance_transformation(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_assemble_transformation());
                    volume->set_volume_transformation(mvs->model_volume->get_transformation());
                    // updates volumes convex hull
                    if (mvs->model_volume->is_model_part() && ! volume->convex_hull())
                        // Model volume was likely changed from modifier or support blocker / enforcer to a model part.
                        // Only model parts require convex hulls.
                        volume->set_convex_hull(mvs->model_volume->get_convex_hull_shared_ptr());
                    volume->set_offset_to_assembly(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_offset_to_assembly());
                }
            }
        }
    }
    sort_remove_duplicates(instance_ids_selected);
    auto deleted_volumes_lower = [](const GLVolumeState &v1, const GLVolumeState &v2) { return v1.composite_id < v2.composite_id; };
    std::sort(deleted_volumes.begin(), deleted_volumes.end(), deleted_volumes_lower);

    //BBS clean hover_volume_idxs
    m_hover_volume_idxs.clear();

    if (m_reload_delayed)
        return;

    // BBS: do not check wipe tower changes
    bool update_object_list = false;
    if (deleted_volumes.size() != deleted_wipe_towers.size())
        update_object_list = true;

    if (m_volumes.volumes != glvolumes_new && !update_object_list) {
        int vol_idx = 0;
        for (; vol_idx < std::min(m_volumes.volumes.size(), glvolumes_new.size()); vol_idx++) {
            if (m_volumes.volumes[vol_idx] != glvolumes_new[vol_idx]) {
                update_object_list = true;
                break;
            }
        }
        for (int temp_idx = vol_idx; temp_idx < m_volumes.volumes.size() && !update_object_list; temp_idx++) {
            // Volumes in m_volumes might not exist anymore, so we cannot
            // directly check if they are is_wipe_towers, for which we do
            // not want to update the object list.  Instead, we do a kind of
            // slow thing of seeing if they were in the deleted list, and if
            // so, if they were a wipe tower.
            bool was_deleted_wipe_tower = false;
            for (int del_idx = 0; del_idx < deleted_wipe_towers.size(); del_idx++) {
                if (deleted_wipe_towers[del_idx].volume_idx == temp_idx) {
                    was_deleted_wipe_tower = true;
                    break;
                }
            }
            if (!was_deleted_wipe_tower) {
                update_object_list = true;
            }
        }
        for (int temp_idx = vol_idx; temp_idx < glvolumes_new.size() && !update_object_list; temp_idx++) {
            if (!glvolumes_new[temp_idx]->is_wipe_tower)
                update_object_list = true;
        }
    }
    m_volumes.volumes = std::move(glvolumes_new);
    bool enable_lod   = GUI::wxGetApp().app_config->get_bool("enable_lod") ;
    for (unsigned int obj_idx = 0; obj_idx < (unsigned int)m_model->objects.size(); ++ obj_idx) {
        const ModelObject &model_object = *m_model->objects[obj_idx];
        for (int volume_idx = 0; volume_idx < (int)model_object.volumes.size(); ++ volume_idx) {
			const ModelVolume &model_volume = *model_object.volumes[volume_idx];
            if (m_canvas_type == ECanvasType::CanvasAssembleView && !model_volume.is_model_part())
                continue;
            for (int instance_idx = 0; instance_idx < (int)model_object.instances.size(); ++ instance_idx) {
				const ModelInstance &model_instance = *model_object.instances[instance_idx];
				ModelVolumeState key(model_volume.id(), model_instance.id());
				auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
				assert(it != model_volume_state.end() && it->geometry_id == key.geometry_id);
                if (it->new_geometry()) {
                    // New volume.
                    auto it_old_volume = std::lower_bound(deleted_volumes.begin(), deleted_volumes.end(), GLVolumeState(it->composite_id), deleted_volumes_lower);
                    if (it_old_volume != deleted_volumes.end() && it_old_volume->composite_id == it->composite_id)
                        // If a volume changed its ObjectID, but it reuses a GLVolume's CompositeID, maintain its selection.
                        map_glvolume_old_to_new[it_old_volume->volume_idx] = m_volumes.volumes.size();
                    // Note the index of the loaded volume, so that we can reload the main model GLVolume with the hollowed mesh
                    // later in this function.
                    it->volume_idx = m_volumes.volumes.size();
                    if (enable_lod && CpuMemory::cur_free_memory_less_than_specify_size_gb(LOD_FREE_MEMORY_SIZE)) {
                        enable_lod = false;
                    }
#ifdef __APPLE__
                    if (Slic3r::is_mac_version_15()) {
                        enable_lod = false;
                    }
#endif
                    m_volumes.load_object_volume(&model_object, obj_idx, volume_idx, instance_idx, m_color_by, m_initialized, m_canvas_type == ECanvasType::CanvasAssembleView, false, enable_lod);
                    m_volumes.volumes.back()->geometry_id = key.geometry_id;
                    update_object_list = true;
                } else {
					// Recycling an old GLVolume.
					GLVolume &existing_volume = *m_volumes.volumes[it->volume_idx];
                    assert(existing_volume.geometry_id == key.geometry_id);
					// Update the Object/Volume/Instance indices into the current Model.
					if (existing_volume.composite_id != it->composite_id) {
						existing_volume.composite_id = it->composite_id;
						update_object_list = true;
					}
                }
            }
        }
    }
    if (printer_technology == ptSLA) {
        size_t idx = 0;
        const SLAPrint *sla_print = this->sla_print();
		std::vector<double> shift_zs(m_model->objects.size(), 0);
        double relative_correction_z = sla_print->relative_correction().z();
        if (relative_correction_z <= EPSILON)
            relative_correction_z = 1.;
		for (const SLAPrintObject *print_object : sla_print->objects()) {
            SLASupportState   &state        = sla_support_state[idx ++];
            const ModelObject *model_object = print_object->model_object();
            // Find an index of the ModelObject
            int object_idx;
            // There may be new SLA volumes added to the scene for this print_object.
            // Find the object index of this print_object in the Model::objects list.
            auto it = std::find(sla_print->model().objects.begin(), sla_print->model().objects.end(), model_object);
            assert(it != sla_print->model().objects.end());
			object_idx = it - sla_print->model().objects.begin();
			// Cache the Z offset to be applied to all volumes with this object_idx.
			shift_zs[object_idx] = print_object->get_current_elevation() / relative_correction_z;
            // Collect indices of this print_object's instances, for which the SLA support meshes are to be added to the scene.
            // pairs of <instance_idx, print_instance_idx>
			std::vector<std::pair<size_t, size_t>> instances[std::tuple_size<SLASteps>::value];
            for (size_t print_instance_idx = 0; print_instance_idx < print_object->instances().size(); ++ print_instance_idx) {
                const SLAPrintObject::Instance &instance = print_object->instances()[print_instance_idx];
                // Find index of ModelInstance corresponding to this SLAPrintObject::Instance.
				auto it = std::find_if(model_object->instances.begin(), model_object->instances.end(),
                    [&instance](const ModelInstance *mi) { return mi->id() == instance.instance_id; });
                assert(it != model_object->instances.end());
                int instance_idx = it - model_object->instances.begin();
                for (size_t istep = 0; istep < sla_steps.size(); ++ istep)
                    if (sla_steps[istep] == slaposDrillHoles) {
                    	// Hollowing is a special case, where the mesh from the backend is being loaded into the 1st volume of an instance,
                    	// not into its own GLVolume.
                        // There shall always be such a GLVolume allocated.
                        ModelVolumeState key(model_object->volumes.front()->id(), instance.instance_id);
                        auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
                        assert(it != model_volume_state.end() && it->geometry_id == key.geometry_id);
                        assert(!it->new_geometry());
                        GLVolume &volume = *m_volumes.volumes[it->volume_idx];
                        if (! volume.offsets.empty() && state.step[istep].timestamp != volume.offsets.front()) {
                        	// The backend either produced a new hollowed mesh, or it invalidated the one that the front end has seen.
                            volume.indexed_vertex_array->release_geometry();
                        	if (state.step[istep].state == PrintStateBase::DONE) {
                                TriangleMesh mesh = print_object->get_mesh(slaposDrillHoles);
	                            assert(! mesh.empty());
                                mesh.transform(sla_print->sla_trafo(*m_model->objects[volume.object_idx()]).inverse());
#if ENABLE_SMOOTH_NORMALS
                                volume.indexed_vertex_array->load_mesh(mesh, true);
#else
                                volume.indexed_vertex_array->load_mesh(mesh);
#endif // ENABLE_SMOOTH_NORMALS
                            } else {
	                        	// Reload the original volume.
#if ENABLE_SMOOTH_NORMALS
                                volume.indexed_vertex_array->load_mesh(m_model->objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh(), true);
#else
                                volume.indexed_vertex_array->load_mesh(m_model->objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh());
#endif // ENABLE_SMOOTH_NORMALS
                            }
                            volume.finalize_geometry(true);
	                    }
                    	//FIXME it is an ugly hack to write the timestamp into the "offsets" field to not have to add another member variable
                    	// to the GLVolume. We should refactor GLVolume significantly, so that the GLVolume will not contain member variables
                    	// of various concenrs (model vs. 3D print path).
                    	volume.offsets = { state.step[istep].timestamp };
                    } else if (state.step[istep].state == PrintStateBase::DONE) {
                        // Check whether there is an existing auxiliary volume to be updated, or a new auxiliary volume to be created.
						ModelVolumeState key(state.step[istep].timestamp, instance.instance_id.id);
						auto it = std::lower_bound(aux_volume_state.begin(), aux_volume_state.end(), key, model_volume_state_lower);
						assert(it != aux_volume_state.end() && it->geometry_id == key.geometry_id);
                    	if (it->new_geometry()) {
                            // This can be an SLA support structure that should not be rendered (in case someone used undo
                            // to revert to before it was generated). If that's the case, we should not generate anything.
                            if (model_object->sla_points_status != sla::PointsStatus::NoPoints)
                                instances[istep].emplace_back(std::pair<size_t, size_t>(instance_idx, print_instance_idx));
                            else
                                shift_zs[object_idx] = 0.;
                        } else {
                            // Recycling an old GLVolume. Update the Object/Instance indices into the current Model.
                            m_volumes.volumes[it->volume_idx]->composite_id = GLVolume::CompositeID(object_idx, m_volumes.volumes[it->volume_idx]->volume_idx(), instance_idx);
                            m_volumes.volumes[it->volume_idx]->set_instance_transformation(model_object->instances[instance_idx]->get_transformation());
                        }
                    }
            }

            for (size_t istep = 0; istep < sla_steps.size(); ++istep)
                if (!instances[istep].empty())
                    m_volumes.load_object_auxiliary(print_object, object_idx, instances[istep], sla_steps[istep], state.step[istep].timestamp, m_initialized);
        }

		// Shift-up all volumes of the object so that it has the right elevation with respect to the print bed
		for (GLVolume* volume : m_volumes.volumes)
			if (volume->object_idx() < (int)m_model->objects.size() && m_model->objects[volume->object_idx()]->instances[volume->instance_idx()]->is_printable())
				volume->set_sla_shift_z(shift_zs[volume->object_idx()]);
    }

    // BBS
    if (printer_technology == ptFFF && m_config->has("filament_colour") && (m_canvas_type != ECanvasType::CanvasAssembleView)) {
        // Should the wipe tower be visualized ?
        unsigned int filaments_count = (unsigned int)dynamic_cast<const ConfigOptionStrings*>(m_config->option("filament_colour"))->values.size();

        bool wt = dynamic_cast<const ConfigOptionBool*>(m_config->option("enable_prime_tower"))->value;

        const DynamicPrintConfig &dconfig           = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        auto timelapse_type = dconfig.option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
        bool need_wipe_tower = timelapse_type ? (timelapse_type->value == TimelapseType::tlSmooth) : false;

        if (dconfig.has("enable_wrapping_detection")) {
            need_wipe_tower |= dynamic_cast<const ConfigOptionBool*>(dconfig.option("enable_wrapping_detection"))->value;
        }

        //Orio: a plate with a "Traitement" sheet has its own prime tower settings.
        bool any_sheet = false;
        for (int plate_id = 0; plate_id < n_plates; plate_id++)
            any_sheet |= ppl.get_plate(plate_id)->has_sheet();
        if ((any_sheet || (wt && (need_wipe_tower || filaments_count > 1))) && !wxGetApp().plater()->only_gcode_mode() && !wxGetApp().plater()->is_gcode_3mf()) {
            for (int plate_id = 0; plate_id < n_plates; plate_id++) {
                // If print ByObject and there is only one object in the plate, the wipe tower is allowed to be generated.
                PartPlate* part_plate = ppl.get_plate(plate_id);
                //Orio: process settings of the plate, its sheet over the global ones.
                DynamicPrintConfig sheet_print_cfg;
                bool               plate_wt              = wt;
                bool               plate_need_wipe_tower = need_wipe_tower;
                if (part_plate->has_sheet()) {
                    sheet_print_cfg = dconfig;
                    PartPlate::apply_sheet_config(sheet_print_cfg, part_plate->sheet_config());
                    plate_wt              = sheet_print_cfg.opt_bool("enable_prime_tower");
                    plate_need_wipe_tower = sheet_print_cfg.opt_enum<TimelapseType>("timelapse_type") == TimelapseType::tlSmooth ||
                                            sheet_print_cfg.opt_bool("enable_wrapping_detection");
                }
                const DynamicPrintConfig &plate_print_cfg = part_plate->has_sheet() ? sheet_print_cfg : dconfig;
                if (! plate_wt || ! (plate_need_wipe_tower || filaments_count > 1))
                    continue;
                // Sequence of the plate, else of its sheet, else the global one.
                if (part_plate->get_real_print_seq() == PrintSequence::ByObject) {
                    if (ppl.get_plate(plate_id)->printable_instance_size() != 1)
                        continue;
                }

                DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
                float x = dynamic_cast<const ConfigOptionFloats*>(proj_cfg.option("wipe_tower_x"))->get_at(plate_id);
                float y = dynamic_cast<const ConfigOptionFloats*>(proj_cfg.option("wipe_tower_y"))->get_at(plate_id);
                float w = dynamic_cast<const ConfigOptionFloat*>(plate_print_cfg.option("prime_tower_width"))->value;
                float a = dynamic_cast<const ConfigOptionFloat*>(proj_cfg.option("wipe_tower_rotation_angle"))->value;
                // BBS
                std::vector<double> v = dynamic_cast<const ConfigOptionFloats*>(m_config->option("filament_prime_volume"))->values;
                if (proj_cfg.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")->value == pvmSaving) {
                    for (auto& val : v)
                        val = 15.f;
                }
                Vec3d plate_origin = ppl.get_plate(plate_id)->get_origin();

                const Print* print = m_process->fff_print();
                const Print* current_print = part_plate->fff_print();
                if (!plate_need_wipe_tower && part_plate->get_extruders(true).size() < 2) continue;
                if (part_plate->get_objects_on_this_plate().empty()) continue;

                float brim_width = print->wipe_tower_data(filaments_count).brim_width;
                const DynamicPrintConfig &print_cfg   = plate_print_cfg;
                double wipe_vol = get_max_element(v);
                int nozzle_nums = wxGetApp().preset_bundle->get_printer_extruder_count();
                Vec3d wipe_tower_size = ppl.get_plate(plate_id)->estimate_wipe_tower_size(print_cfg, w, wipe_vol, nozzle_nums, 0, false, dynamic_cast<const ConfigOptionBool*>(plate_print_cfg.option("enable_wrapping_detection"))->value);

                {
                    const float                 margin     = WIPE_TOWER_MARGIN;
                    BoundingBoxf3               plate_bbox = part_plate->get_bounding_box();
                    BoundingBoxf                plate_bbox_2d(Vec2d(plate_bbox.min(0), plate_bbox.min(1)), Vec2d(plate_bbox.max(0), plate_bbox.max(1)));
                    const std::vector<Pointfs> &extruder_areas = part_plate->get_extruder_areas();
                    for (Pointfs points : extruder_areas) {
                        BoundingBoxf bboxf(points);
                        plate_bbox_2d.min = plate_bbox_2d.min(0) >= bboxf.min(0) ? plate_bbox_2d.min : bboxf.min;
                        plate_bbox_2d.max = plate_bbox_2d.max(0) <= bboxf.max(0) ? plate_bbox_2d.max : bboxf.max;
                    }

                    coordf_t plate_bbox_x_min_local_coord = plate_bbox_2d.min(0) - plate_origin(0);
                    coordf_t plate_bbox_x_max_local_coord = plate_bbox_2d.max(0) - plate_origin(0);
                    coordf_t plate_bbox_y_max_local_coord = plate_bbox_2d.max(1) - plate_origin(1);

                    if (!current_print->is_step_done(psWipeTower) || !current_print->wipe_tower_data().wipe_tower_mesh_data) {
                        // update for wipe tower position
                        {
                            int volume_idx_wipe_tower_new = m_volumes.load_wipe_tower_preview(1000 + plate_id, x + plate_origin(0), y + plate_origin(1),
                                                                                              (float) wipe_tower_size(0), (float) wipe_tower_size(1), (float) wipe_tower_size(2),
                                                                                              a,
                                                                                              /*!print->is_step_done(psWipeTower)*/ true, brim_width, m_initialized);
                            int volume_idx_wipe_tower_old = volume_idxs_wipe_tower_old[plate_id];
                            if (volume_idx_wipe_tower_old != -1) map_glvolume_old_to_new[volume_idx_wipe_tower_old] = volume_idx_wipe_tower_new;
                        }
                    } else {
                        auto        tower_bottom = current_print->wipe_tower_data().wipe_tower_mesh_data->bottom;
                        tower_bottom.translate(scaled(Vec2d{x, y}));
                        tower_bottom.translate(scaled(Vec2d{plate_origin[0], plate_origin[1]}));
                        auto tower_bottom_bbox = get_extents(tower_bottom);
                        BoundingBoxf3 plate_bbox        = wxGetApp().plater()->get_partplate_list().get_plate(plate_id)->get_build_volume(true);
                        BoundingBox   plate_bbox2d      = BoundingBox(scaled(Vec2f(plate_bbox.min[0], plate_bbox.min[1])), scaled(Vec2f(plate_bbox.max[0], plate_bbox.max[1])));
                        Vec2f         offset            = WipeTower::move_box_inside_box(tower_bottom_bbox, plate_bbox2d, scaled(margin));
                        int volume_idx_wipe_tower_new = m_volumes.load_real_wipe_tower_preview(1000 + plate_id, x + plate_origin(0), y + plate_origin(1),
                                                                                               current_print->wipe_tower_data().wipe_tower_mesh_data->real_wipe_tower_mesh,
                                                                                               current_print->wipe_tower_data().wipe_tower_mesh_data->real_brim_mesh,
                                                                                            true,a,/*!print->is_step_done(psWipeTower)*/ true, m_initialized);
                        int volume_idx_wipe_tower_old = volume_idxs_wipe_tower_old[plate_id];
                        if (volume_idx_wipe_tower_old != -1) map_glvolume_old_to_new[volume_idx_wipe_tower_old] = volume_idx_wipe_tower_new;
                    }
                }
            }
        }
    }

    update_volumes_colors_by_extruder();
    if (m_selection.is_enabled()) {
        // Update selection indices based on the old/new GLVolumeCollection.
        if (m_selection.get_mode() == Selection::Instance)
            m_selection.instances_changed(instance_ids_selected);
        else
            m_selection.volumes_changed(map_glvolume_old_to_new);
    }
    if (m_gizmos.is_enabled()) {
        m_gizmos.update_data();
        m_gizmos.update_assemble_view_data();
        m_gizmos.refresh_on_off_state();
    }

    // Update the toolbar
    //BBS: notify the PartPlateList to reload all objects
    if (update_object_list)
    {
        post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
    }

    //BBS:exclude the assmble view
    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        _update_slice_error_status();
        // checks for geometry outside the print volume to render it accordingly
        if (!m_volumes.empty()) {
            ModelInstanceEPrintVolumeState state;
            ObjectFilamentResults object_results;
            const bool contained_min_one = m_volumes.check_outside_state(m_bed.build_volume(), &state, &object_results, *m_model);
            const bool partlyOut = (state == ModelInstanceEPrintVolumeState::ModelInstancePVS_Partly_Outside);
            const bool fullyOut = (state == ModelInstanceEPrintVolumeState::ModelInstancePVS_Fully_Outside);
           // const bool objectLimited = (state == ModelInstanceEPrintVolumeState::ModelInstancePVS_Limited);

            PartPlate *cur_plate  = wxGetApp().plater()->get_partplate_list().get_curr_plate();
            bool show_read_wipe_tower = cur_plate->fff_print()->is_step_done(psWipeTower);
            bool       wipe_tower_outside   = m_volumes.check_wipe_tower_outside_state(m_bed.build_volume(), wxGetApp().plater()->get_partplate_list().get_curr_plate_index());
            bool show_wipe_tower_outside_error = show_read_wipe_tower ? !wipe_tower_outside : false;
            _set_warning_notification(EWarning::PrimeTowerOutside, show_wipe_tower_outside_error);

            auto clash_flag = construct_error_string(object_results, get_object_clashed_text());
            auto unprintable_flag= construct_extruder_unprintable_error(object_results, get_left_extruder_unprintable_text(), get_right_extruder_unprintable_text());

            bool is_flushing_volume_valid = is_flushing_matrix_error();
            _set_warning_notification(EWarning::FlushingVolumeZero, is_flushing_volume_valid);
            _set_warning_notification(EWarning::ObjectClashed, clash_flag);
            _set_warning_notification(EWarning::LeftExtruderPrintableError, unprintable_flag.first);
            _set_warning_notification(EWarning::RightExtruderPrintableError, unprintable_flag.second);
            //_set_warning_notification(EWarning::ObjectLimited, objectLimited);
            //BBS: turn off the warning when fully outside
            //_set_warning_notification(EWarning::ObjectOutside, fullyOut);
            //if (printer_technology != ptSLA || !contained_min_one)
            //    _set_warning_notification(EWarning::SlaSupportsOutside, false);

            auto full_config_temp = wxGetApp().preset_bundle->full_config();
            bool tpu_valid = cur_plate->check_tpu_printable_status(full_config_temp, wxGetApp().preset_bundle->get_used_tpu_filaments(cur_plate->get_extruders(true)));
            _set_warning_notification(EWarning::TPUPrintableError, !tpu_valid);

            bool filament_printable = cur_plate->check_filament_printable(full_config_temp, filament_printable_error_msg);
            _set_warning_notification(EWarning::FilamentPrintableError, !filament_printable);

            bool mix_pla_and_petg = cur_plate->check_mixture_of_pla_and_petg(full_config_temp);
            _set_warning_notification(EWarning::MixUsePLAAndPETG, !mix_pla_and_petg);

            bool multi_filament_with_wipe_tower = cur_plate->check_multi_filament_without_prime_tower(full_config_temp);
            _set_warning_notification(EWarning::MultiFilaNoWipeTower, !multi_filament_with_wipe_tower);

            bool filament_nozzle_compatible = cur_plate->check_compatible_of_nozzle_and_filament(full_config_temp, wxGetApp().preset_bundle->filament_presets, get_nozzle_filament_incompatible_text());
            _set_warning_notification(EWarning::NozzleFilamentIncompatible, !filament_nozzle_compatible);

            bool filament_mixture_compatible = cur_plate->check_mixture_filament_compatible(full_config_temp, get_filament_mixture_warning_text());
            _set_warning_notification(EWarning::MixtureFilamentIncompatible, !filament_mixture_compatible);

            bool model_fits = contained_min_one && !m_model->objects.empty() && !partlyOut && object_results.filaments.empty() && tpu_valid && filament_printable;
            post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, model_fits));
            ppl.get_curr_plate()->update_slice_ready_status(model_fits);

            bool filament_flow_compatible = cur_plate->check_flow_compatible_of_nozzle_and_filament(full_config_temp, wxGetApp().preset_bundle->filament_presets, get_filament_flow_warning_text());
            _set_warning_notification(EWarning::FilamentNozzleFlowIncompatible, !filament_flow_compatible);

            bool tpu_nozzle_has_multi_filaments = cur_plate->check_tpu_nozzle_has_multiple_filaments(full_config_temp, get_tpu_nozzle_multi_filaments_warning_text());
            _set_warning_notification(EWarning::TpuNozzleMultipleFilaments, tpu_nozzle_has_multi_filaments);

            bool high_temp_need_wrapping = cur_plate->check_high_temp_need_wrapping_detection(full_config_temp, get_high_temp_wrapping_warning_text());
            _set_warning_notification(EWarning::HighTempNeedWrappingDetection, high_temp_need_wrapping);

            bool single_extruder_mixed_risk = cur_plate->check_single_extruder_mixed_filament_risk(full_config_temp, get_single_extruder_mixed_filament_warning_text());
            _set_warning_notification(EWarning::SingleExtruderMixedFilament, single_extruder_mixed_risk);
        }
        else {
            _set_warning_notification(EWarning::ObjectOutside, false);
            _set_warning_notification(EWarning::FlushingVolumeZero, false);
            _set_warning_notification(EWarning::ObjectClashed, false);
            _set_warning_notification(EWarning::LeftExtruderPrintableError, false);
            _set_warning_notification(EWarning::RightExtruderPrintableError, false);
            //_set_warning_notification(EWarning::ObjectLimited, false);
            //_set_warning_notification(EWarning::SlaSupportsOutside, false);
           _set_warning_notification(EWarning::TPUPrintableError, false);
           _set_warning_notification(EWarning::FilamentPrintableError, false);
           _set_warning_notification(EWarning::MixUsePLAAndPETG, false);
           _set_warning_notification(EWarning::MultiFilaNoWipeTower, false);
           _set_warning_notification(EWarning::PrimeTowerOutside, false);
           _set_warning_notification(EWarning::MultiExtruderPrintableError,false);
           _set_warning_notification(EWarning::MultiExtruderHeightOutside,false);
           _set_warning_notification(EWarning::NozzleFilamentIncompatible,false);
           _set_warning_notification(EWarning::MixtureFilamentIncompatible,false);
           _set_warning_notification(EWarning::TpuNozzleMultipleFilaments, false);
           _set_warning_notification(EWarning::HighTempNeedWrappingDetection, false);
           _set_warning_notification(EWarning::SingleExtruderMixedFilament, false);

           post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, false));
        }
    }
    else
    {
        bool flag = false;
        if (!m_volumes.empty()) {
            std::vector<std::string> object_results;
            object_results.reserve(10);
            struct TempVolumeData
            {
                BoundingBoxf3 m_aabb;
                GLVolume* m_p_volume{ nullptr };
            };
            std::vector<TempVolumeData> temp_volume_data_list;
            temp_volume_data_list.reserve(m_volumes.volumes.size());
            BoundingBoxf3 assembly_bb;
            for (GLVolume* volume : m_volumes.volumes) {
                if (!m_apply_zoom_to_volumes_filter || ((volume != nullptr) && volume->zoom_to_volumes)) {
                    const auto v_bb = volume->transformed_bounding_box();
                    assembly_bb.merge(v_bb);

                    TempVolumeData t_volume_data;
                    t_volume_data.m_aabb = v_bb;
                    t_volume_data.m_p_volume = volume;
                    temp_volume_data_list.emplace_back(t_volume_data);
                }
            }

            if (assembly_bb.max_size() >= 1e4f) { // 10m
                for (const auto& t_volume_data : temp_volume_data_list) {
                    if (!t_volume_data.m_p_volume) {
                        continue;
                    }
                    const auto t_length = t_volume_data.m_aabb.center().norm();
                    if (t_length >= 1e4f) {
                        const auto& p_object = (*m_model).objects[t_volume_data.m_p_volume->object_idx()];
                        if (p_object) {
                            object_results.emplace_back(p_object->name);
                        }
                    }
                }
                flag = construct_assembly_warning_string(object_results, get_assembly_too_far_text());
            }
        }
        else {
            flag = false;
        }
        _set_warning_notification(EWarning::AsemblyInvalid, flag);
    }

    refresh_camera_scene_box();

    if (m_gizmos.is_enabled() && m_selection.is_empty()) {
        // If no object is selected, deactivate the active gizmo, if any
        // Otherwise it may be shown after cleaning the scene (if it was active while the objects were deleted)
        m_gizmos.reset_all_states();
        // BBS
#if 0
        // If no object is selected, reset the objects manipulator on the sidebar
        // to force a reset of its cache
        auto manip = wxGetApp().obj_manipul();
        if (manip != nullptr)
            manip->set_dirty();
#endif
    }
    //Orio: instances of the hidden « Hors impression » area.
    update_parked_volumes_visibility();
    wxGetApp().plater()->get_partplate_list().reset_thumbnail_assembly_view_data(); // reload scene
    // and force this canvas to be redrawn.
    m_dirty = true;
}

static void reserve_new_volume_finalize_old_volume(GLVolume& vol_new, GLVolume& vol_old, bool gl_initialized, size_t prealloc_size = VERTEX_BUFFER_RESERVE_SIZE)
{
	// Assign the large pre-allocated buffers to the new GLVolume.
	*(vol_new.indexed_vertex_array) = std::move(*(vol_old.indexed_vertex_array));
	// Copy the content back to the old GLVolume.
	*(vol_old.indexed_vertex_array) = *(vol_new.indexed_vertex_array);
	// Clear the buffers, but keep them pre-allocated.
	vol_new.indexed_vertex_array->clear();
	// Just make sure that clear did not clear the reserved memory.
	// Reserving number of vertices (3x position + 3x color)
	vol_new.indexed_vertex_array->reserve(prealloc_size / 6);
	// Finalize the old geometry, possibly move data to the graphics card.
	vol_old.finalize_geometry(gl_initialized);
}

//BBS: always load shell at preview
void GLCanvas3D::load_shells(const Print& print, bool force_previewing)
{
    if (m_initialized) {
        auto& t_gcode_viewer = get_gcode_viewer();
        t_gcode_viewer.load_shells(print, m_initialized, force_previewing);
        t_gcode_viewer.update_shells_color_by_extruder(m_config);
    }
}

void GLCanvas3D::reset_shells()
{
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.reset_shell();
}

void GLCanvas3D::set_shells_on_previewing(bool is_preview)
{
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.set_shells_on_preview(is_preview);
}

//BBS: add only gcode mode
void GLCanvas3D::load_gcode_preview(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors, bool only_gcode)
{
    PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
    PartPlate* plate = partplate_list.get_curr_plate();
    const std::vector<BoundingBoxf3>& exclude_bounding_box = plate->get_exclude_areas();

    //BBS: init is called in GLCanvas3D.render()
    //when load gcode directly, it is too late
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.init(wxGetApp().get_mode(), wxGetApp().preset_bundle);
    t_gcode_viewer.load(gcode_result, *this->fff_print(), wxGetApp().plater()->build_volume(), exclude_bounding_box,
        m_initialized, wxGetApp().get_mode(), only_gcode);

    if (wxGetApp().is_editor()) {
        //BBS: always load shell at preview, do this in load_shells
        _update_slice_error_status();
    }

    t_gcode_viewer.refresh(gcode_result, str_tool_colors);
    t_gcode_viewer.refresh_render_paths();
    set_as_dirty();
    request_extra_frame();
}

void GLCanvas3D::refresh_gcode_preview_render_paths()
{
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.refresh_render_paths();
    set_as_dirty();
    request_extra_frame();
}

void GLCanvas3D::load_sla_preview()
{
    const SLAPrint* print = sla_print();
    if (m_canvas != nullptr && print != nullptr) {
        _set_current(true);
	    // Release OpenGL data before generating new data.
	    reset_volumes();
        _load_sla_shells();
        _update_sla_shells_outside_state();
        _set_warning_notification_if_needed(EWarning::SlaSupportsOutside);
    }
}

/*void GLCanvas3D::load_preview(const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values)
{
    const Print *print = this->fff_print();
    if (print == nullptr)
        return;

    _set_current();

    // Release OpenGL data before generating new data.
    this->reset_volumes();

    const BuildVolume &build_volume = m_bed.build_volume();
    _load_print_toolpaths(build_volume);
    _load_wipe_tower_toolpaths(build_volume, str_tool_colors);
    for (const PrintObject* object : print->objects())
        _load_print_object_toolpaths(*object, build_volume, str_tool_colors, color_print_values);

    _set_warning_notification_if_needed(EWarning::ToolpathOutside);
}*/

void GLCanvas3D::bind_event_handlers()
{
    if (m_canvas != nullptr) {
        m_canvas->Bind(wxEVT_SIZE, &GLCanvas3D::on_size, this);
        m_canvas->Bind(wxEVT_IDLE, &GLCanvas3D::on_idle, this);
        m_canvas->Bind(wxEVT_CHAR, &GLCanvas3D::on_char, this);
        m_canvas->Bind(wxEVT_KEY_DOWN, &GLCanvas3D::on_key, this);
        m_canvas->Bind(wxEVT_KEY_UP, &GLCanvas3D::on_key, this);
        m_canvas->Bind(wxEVT_MOUSEWHEEL, &GLCanvas3D::on_mouse_wheel, this);
        m_canvas->Bind(wxEVT_TIMER, &GLCanvas3D::on_timer, this);
        m_canvas->Bind(EVT_GLCANVAS_RENDER_TIMER, &GLCanvas3D::on_render_timer, this);
        m_toolbar_highlighter.set_timer_owner(m_canvas, 0);
        m_canvas->Bind(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, [this](wxTimerEvent&) { m_toolbar_highlighter.blink(); });
        m_gizmo_highlighter.set_timer_owner(m_canvas, 0);
        m_canvas->Bind(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, [this](wxTimerEvent&) { m_gizmo_highlighter.blink(); });
        m_canvas->Bind(wxEVT_LEFT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEFT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MOTION, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_ENTER_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEAVE_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEFT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_PAINT, &GLCanvas3D::on_paint, this);
        m_canvas->Bind(wxEVT_SET_FOCUS, &GLCanvas3D::on_set_focus, this);
        m_canvas->Bind(wxEVT_KILL_FOCUS, &GLCanvas3D::on_kill_focus, this);
        m_event_handlers_bound = true;

        m_canvas->Bind(wxEVT_GESTURE_PAN, &GLCanvas3D::on_gesture, this);
        m_canvas->Bind(wxEVT_GESTURE_ZOOM, &GLCanvas3D::on_gesture, this);
        m_canvas->Bind(wxEVT_GESTURE_ROTATE, &GLCanvas3D::on_gesture, this);
        m_canvas->EnableTouchEvents(wxTOUCH_ZOOM_GESTURE | wxTOUCH_ROTATE_GESTURE);
#if __WXOSX__
        initGestures(m_canvas->GetHandle(), m_canvas); // for UIPanGestureRecognizer allowedScrollTypesMask
#endif
    }
}

void GLCanvas3D::unbind_event_handlers()
{
    if (m_canvas != nullptr && m_event_handlers_bound) {
        m_canvas->Unbind(wxEVT_SIZE, &GLCanvas3D::on_size, this);
        m_canvas->Unbind(wxEVT_IDLE, &GLCanvas3D::on_idle, this);
        m_canvas->Unbind(wxEVT_CHAR, &GLCanvas3D::on_char, this);
        m_canvas->Unbind(wxEVT_KEY_DOWN, &GLCanvas3D::on_key, this);
        m_canvas->Unbind(wxEVT_KEY_UP, &GLCanvas3D::on_key, this);
        m_canvas->Unbind(wxEVT_MOUSEWHEEL, &GLCanvas3D::on_mouse_wheel, this);
        m_canvas->Unbind(wxEVT_TIMER, &GLCanvas3D::on_timer, this);
        m_canvas->Unbind(EVT_GLCANVAS_RENDER_TIMER, &GLCanvas3D::on_render_timer, this);
        m_canvas->Unbind(wxEVT_LEFT_DOWN, &GLCanvas3D::on_mouse, this);
		m_canvas->Unbind(wxEVT_LEFT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MOTION, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_ENTER_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_LEAVE_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_LEFT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_PAINT, &GLCanvas3D::on_paint, this);
        m_canvas->Unbind(wxEVT_SET_FOCUS, &GLCanvas3D::on_set_focus, this);
        m_event_handlers_bound = false;

        m_canvas->Unbind(wxEVT_GESTURE_PAN, &GLCanvas3D::on_gesture, this);
        m_canvas->Unbind(wxEVT_GESTURE_ZOOM, &GLCanvas3D::on_gesture, this);
        m_canvas->Unbind(wxEVT_GESTURE_ROTATE, &GLCanvas3D::on_gesture, this);
    }
}

void GLCanvas3D::on_size(wxSizeEvent& evt)
{
    m_dirty = true;
}

void GLCanvas3D::on_idle(wxIdleEvent& evt)
{
    if (!m_initialized)
        return;

    const auto& p_main_toolbar = get_main_toolbar();
    if (p_main_toolbar) {
        m_dirty |= p_main_toolbar->update_items_state();
    }
    // BBS
    //m_dirty |= wxGetApp().plater()->get_view_toolbar().update_items_state();
    m_dirty |= wxGetApp().plater()->sidebar().get_update_3d_state();
    if (wxGetApp().plater()->sidebar().get_update_3d_state()) {
        wxGetApp().plater()->sidebar().cancel_update_3d_state();
    }
    m_dirty |= wxGetApp().plater()->get_collapse_toolbar().update_items_state();
    bool mouse3d_controller_applied = wxGetApp().plater()->get_mouse3d_controller().apply(get_active_camera());
    m_dirty |= mouse3d_controller_applied;
    m_dirty |= wxGetApp().plater()->get_notification_manager()->update_notifications(*this);
    auto gizmo = wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager().get_current();
    if (gizmo != nullptr) m_dirty |= gizmo->update_items_state();
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    // ImGuiWrapper::m_requires_extra_frame may have been set by a render made outside of the OnIdle mechanism
    bool imgui_requires_extra_frame = wxGetApp().imgui()->requires_extra_frame();
    m_dirty |= imgui_requires_extra_frame;
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

    if (!m_dirty)
        return;

#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    // this needs to be done here.
    // during the render launched by the refresh the value may be set again
    wxGetApp().imgui()->reset_requires_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

    _refresh_if_shown_on_screen();

#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    if (m_extra_frame_requested || mouse3d_controller_applied || imgui_requires_extra_frame || wxGetApp().imgui()->requires_extra_frame()) {
#else
    if (m_extra_frame_requested || mouse3d_controller_applied) {
        m_dirty = true;
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
        m_extra_frame_requested = false;
        evt.RequestMore();
    }
    else
        m_dirty = false;
}

void GLCanvas3D::on_char(wxKeyEvent& evt)
{
    if (!m_initialized)
        return;

    // see include/wx/defs.h enum wxKeyCode
    int keyCode = evt.GetKeyCode();
    int ctrlMask = wxMOD_CONTROL;
    int shiftMask = wxMOD_SHIFT;
    int altMask   = wxMOD_ALT;

    auto imgui = wxGetApp().imgui();
    if (imgui->update_key_data(evt)) {
        render();
        return;
    }

    bool is_in_painting_mode = false;
    GLGizmoPainterBase *current_gizmo_painter = dynamic_cast<GLGizmoPainterBase *>(get_gizmos_manager().get_current());
    if (current_gizmo_painter != nullptr) {
        is_in_painting_mode = true;
    }

    //BBS: add orient deactivate logic
    if (keyCode == WXK_ESCAPE
        && (_deactivate_arrange_menu() || _deactivate_orient_menu() || _deactivate_layersediting_menu() || _deactivate_speedediting_menu() ||
            _deactivate_infillwallsediting_menu()))
        return;

    if (m_gizmos.on_char(evt))
        return;

    if ((evt.GetModifiers() & ctrlMask) != 0) {
        if ((evt.GetModifiers() & shiftMask) != 0) {
            switch (keyCode) {
            case 'd':
            case 'D':
            case WXK_CONTROL_D: {
                post_event(SimpleEvent(EVT_GLTOOLBAR_DELETE_ALL));
                return;
            }
            case 'z':
            case 'Z':
            case WXK_CONTROL_Z: {
                if (m_canvas_type == CanvasView3D || m_canvas_type == CanvasAssembleView) {
                    post_event(SimpleEvent(EVT_GLCANVAS_REDO));
                }
                return;
            }
            case 'e':
            case 'E':
            case WXK_CONTROL_E: {
                m_labels.show_object_labels(!m_labels.are_object_labels_shown());
                m_dirty = true;
                return;
            }
            }
        }
        // CTRL is pressed
        switch (keyCode) {
#ifdef __APPLE__
        case 'a':
        case 'A':
#else /* __APPLE__ */
        case WXK_CONTROL_A:
#endif /* __APPLE__ */
            //Orio: Ctrl+A selects all the lines of the Preview legend when it has the focus.
            if (m_canvas_type == CanvasPreview && gcode::BaseRenderer::select_all_legend_items())
                m_dirty = true;
            else if (!is_in_painting_mode && !m_layers_editing.is_enabled() && !m_speed_editing.is_enabled() && !m_infill_walls_editing.is_enabled())
                post_event(SimpleEvent(EVT_GLCANVAS_SELECT_ALL));
        break;
#ifdef __APPLE__
        case 'c':
        case 'C':
#else /* __APPLE__ */
        case WXK_CONTROL_C:
#endif /* __APPLE__ */
            if (!is_in_painting_mode)
                post_event(SimpleEvent(EVT_GLTOOLBAR_COPY));
        break;
#ifdef __APPLE__
        case 'm':
        case 'M':
#else  /* __APPLE__ */
        case WXK_CONTROL_M:
#endif /* __APPLE__ */
        {
#ifdef _WIN32
            if (wxGetApp().app_config->get("use_legacy_3DConnexion") == "true") {
#endif //_WIN32
#ifdef __APPLE__
                // On OSX use Cmd+Shift+M to "Show/Hide 3Dconnexion devices settings dialog"
                if ((evt.GetModifiers() & shiftMask) != 0) {
#endif // __APPLE__
                    Mouse3DController &controller = wxGetApp().plater()->get_mouse3d_controller();
                    controller.show_settings_dialog(!controller.is_settings_dialog_shown());
                    m_dirty = true;
#ifdef __APPLE__
                } else
                    // and Cmd+M to minimize application
                    wxGetApp().mainframe->Iconize();
#endif // __APPLE__
#ifdef _WIN32
            }
#endif //_WIN32
            break;
        }
#ifdef __APPLE__
        case 'v':
        case 'V':
#else /* __APPLE__ */
        case WXK_CONTROL_V:
#endif /* __APPLE__ */
            if (!is_in_painting_mode)
                post_event(SimpleEvent(EVT_GLTOOLBAR_PASTE));
        break;

#ifdef __APPLE__
        case 'x':
        case 'X':
#else /* __APPLE__ */
        case WXK_CONTROL_X:
#endif /* __APPLE__ */
            if (!is_in_painting_mode)
                post_event(SimpleEvent(EVT_GLTOOLBAR_CUT));
        break;

#ifdef __APPLE__
        case 'f':
        case 'F':
#else /* __APPLE__ */
        case WXK_CONTROL_F:
#endif /* __APPLE__ */
            break;

#ifdef __APPLE__
        case 'l':
        case 'L':
#else  /* __APPLE__ */
        case WXK_CONTROL_L:
#endif /* __APPLE__ */
        {
            show_overhang(!is_overhang_shown());
            m_dirty = true;
            break;
        }

#ifdef __APPLE__
        case 'u':
        case 'U':
#else  /* __APPLE__ */
        case WXK_CONTROL_U:
#endif /* __APPLE__ */
        {
            //Orio: show / hide the supports of the last slicing.
            if (m_canvas_type == CanvasView3D)
                show_sliced_supports(!are_sliced_supports_shown());
            m_dirty = true;
            break;
        }

#ifdef __APPLE__
        case 'y':
        case 'Y':
#else /* __APPLE__ */
        case WXK_CONTROL_Y:
#endif /* __APPLE__ */
            if (m_canvas_type == CanvasView3D || m_canvas_type == CanvasAssembleView) {
                post_event(SimpleEvent(EVT_GLCANVAS_REDO));
            }
        break;
#ifdef __APPLE__
        case 'z':
        case 'Z':
#else /* __APPLE__ */
        case WXK_CONTROL_Z:
#endif /* __APPLE__ */
            // only support redu/undo in CanvasView3D
            if (m_canvas_type == CanvasView3D || m_canvas_type == CanvasAssembleView) {
                post_event(SimpleEvent(EVT_GLCANVAS_UNDO));
            }
        break;

        // BBS
#ifdef __APPLE__
        case 'E':
        case 'e':
#else /* __APPLE__ */
        case WXK_CONTROL_E:
#endif /* __APPLE__ */
        {
            m_labels.show_layer_labels(!m_labels.are_layer_labels_shown());
            m_dirty = true;
            break;
        }
#ifdef __APPLE__
        case 'W':
        case 'w':
#else  /* __APPLE__ */
        case WXK_CONTROL_W:
#endif /* __APPLE__ */
        {
            wxGetApp().plater()->reset_window_layout();
            break;
        }
        case '0': {
            select_view("plate");
            zoom_to_bed();
            break; }
        case '1': { select_view("top"); break; }
        case '2': { select_view("bottom"); break; }
        case '3': { select_view("front"); break; }
        case '4': { select_view("rear"); break; }
        case '5': { select_view("left"); break; }
        case '6': { select_view("right"); break; }
        case '7': { select_view("iso");break; }
        //Orio: Ctrl+D duplicates the selected objects, Ctrl+K instantiates them (Bambu's "Clone" was on Ctrl+K).
#ifdef __APPLE__
        case 'd':
        case 'D':
#else /* __APPLE__ */
        case WXK_CONTROL_D:
#endif /* __APPLE__ */
            if (!is_in_painting_mode && m_canvas_type == CanvasView3D)
                post_event(SimpleEvent(EVT_GLTOOLBAR_DUPLICATE));
            break;
#ifdef __APPLE__
        case 'k':
        case 'K':
#else /* __APPLE__ */
        case WXK_CONTROL_K:
#endif /* __APPLE__ */
            if (!is_in_painting_mode && m_canvas_type == CanvasView3D)
                post_event(SimpleEvent(EVT_GLTOOLBAR_INSTANTIATE));
            break;
        default:            evt.Skip();
        }
    } else {
        auto obj_list = wxGetApp().obj_list();
        switch (keyCode)
        {
        //case WXK_BACK:
        case WXK_DELETE: { post_event(SimpleEvent(EVT_GLTOOLBAR_DELETE)); break; }
        // BBS
#ifdef __APPLE__
        case WXK_BACK: { post_event(SimpleEvent(EVT_GLTOOLBAR_DELETE)); break; }
#endif
        case WXK_ESCAPE: {
            // Orio : Apercu, Echap rend la legende au plateau entier (objet choisi au clic)
            if (m_canvas_type == CanvasPreview && get_gcode_viewer().get_picked_label() >= 0) {
                get_gcode_viewer().set_picked_label(-1);
                set_as_dirty();
            } else
                deselect_all();
            break;
        }
        //case WXK_F5: {
        //    if ((wxGetApp().is_editor() && !wxGetApp().plater()->model().objects.empty()) ||
        //        (wxGetApp().is_gcode_viewer() && !wxGetApp().plater()->get_last_loaded_gcode().empty()))
        //        post_event(SimpleEvent(EVT_GLCANVAS_RELOAD_FROM_DISK));
        //    break;
        //}

        // BBS: use keypad to change extruder
        case '0'://Color logic for material 10
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': {
            int digit = keyCode - '0';
            const bool starting_new_sequence = (m_color_input_value < 0 || !m_timer_set_color.IsRunning());
            if (starting_new_sequence && digit == 0) {
                // Map single zero key to filament #10 immediately
                if (m_timer_set_color.IsRunning())
                    m_timer_set_color.Stop();
                if (obj_list != nullptr && m_gizmos.get_current_type() != GLGizmosManager::MmuSegmentation)
                    obj_list->set_extruder_for_selected_items(10);
                m_color_input_value = -1;
                break;
            }
            else if (starting_new_sequence) {
                m_color_input_value = digit;
            } else {
                m_color_input_value = m_color_input_value * 10 + digit;
            }
            if (m_timer_set_color.IsRunning()) m_timer_set_color.Stop();
            m_timer_set_color.StartOnce(500);
            break;
        }

        //case '+': {
        //    if (dynamic_cast<Preview*>(m_canvas->GetParent()) != nullptr)
        //        post_event(wxKeyEvent(EVT_GLCANVAS_EDIT_COLOR_CHANGE, evt));
        //    else
        //        post_event(Event<int>(EVT_GLCANVAS_INCREASE_INSTANCES, +1));
        //    break;
        //}
        //case '-': {
        //    if (dynamic_cast<Preview*>(m_canvas->GetParent()) != nullptr)
        //        post_event(wxKeyEvent(EVT_GLCANVAS_EDIT_COLOR_CHANGE, evt));
        //    else
        //        post_event(Event<int>(EVT_GLCANVAS_INCREASE_INSTANCES, -1));
        //    break;
        //}
        case '?': { post_event(SimpleEvent(EVT_GLCANVAS_QUESTION_MARK)); break; }
        //Orio: [V] shows / hides the lines selected in the legend of the Preview.
        case 'V':
        case 'v':
            if (m_canvas_type == CanvasPreview) {
                gcode::BaseRenderer::toggle_selected_legend_items();
                m_dirty = true;
            } else
                evt.Skip();
            break;
        case 'A':
        case 'a':
            {
                if ((evt.GetModifiers() & shiftMask) != 0)
                    post_event(SimpleEvent(EVT_GLCANVAS_ARRANGE_PARTPLATE));
                else if ((evt.GetModifiers() & altMask) != 0)
                    post_event(SimpleEvent(EVT_GLCANVAS_ARRANGE_OUTPLATE));
                else
                    post_event(SimpleEvent(EVT_GLCANVAS_ARRANGE));
                break;
            }
        case 'r':
        case 'R':
            {
                if ((evt.GetModifiers() & shiftMask) != 0)
                    post_event(SimpleEvent(EVT_GLCANVAS_ORIENT_PARTPLATE));
                else
                    post_event(SimpleEvent(EVT_GLCANVAS_ORIENT));
                break;
            }
#if !BBL_RELEASE_TO_PUBLIC
        case 'C':
        case 'c': {
            auto& t_gcode_viewer = get_gcode_viewer();
            t_gcode_viewer.toggle_gcode_window_visibility();
            m_dirty = true;
            request_extra_frame();
            break;
        }
#endif
        //case 'G':
        //case 'g': {
        //    if ((evt.GetModifiers() & shiftMask) != 0) {
        //        if (dynamic_cast<Preview*>(m_canvas->GetParent()) != nullptr)
        //            post_event(wxKeyEvent(EVT_GLCANVAS_JUMP_TO, evt));
        //    }
        //    break;
        //}
        //case 'I':
        //case 'i': { _update_camera_zoom(1.0); break; }

        //case 'L':
        //case 'l': {
            //if (!m_main_toolbar.is_enabled()) {
            //    m_gcode_viewer.enable_legend(!m_gcode_viewer.is_legend_enabled());
            //    m_dirty = true;
            //    wxGetApp().plater()->update_preview_bottom_toolbar();
            //}
            //break;
        //}
        //case 'O':
        //case 'o': { _update_camera_zoom(-1.0); break; }
#if ENABLE_RENDER_PICKING_PASS
        case 'T':
        case 't': {
            m_show_picking_texture = !m_show_picking_texture;
            m_dirty = true;
            break;
        }
#endif // ENABLE_RENDER_PICKING_PASS
        default:  { evt.Skip(); break; }
        }
    }
}

class TranslationProcessor
{
    using UpAction = std::function<void(void)>;
    using DownAction = std::function<void(const Vec3d&, bool, bool)>;

    UpAction m_up_action{ nullptr };
    DownAction m_down_action{ nullptr };

    bool m_running{ false };
    Vec3d m_direction{ Vec3d::UnitX() };

public:
    TranslationProcessor(UpAction up_action, DownAction down_action)
        : m_up_action(up_action), m_down_action(down_action)
    {
    }

    void process(wxKeyEvent& evt)
    {
        const int keyCode = evt.GetKeyCode();
        wxEventType type = evt.GetEventType();
        if (type == wxEVT_KEY_UP) {
            switch (keyCode)
            {
            case WXK_NUMPAD_LEFT:  case WXK_LEFT:
            case WXK_NUMPAD_RIGHT: case WXK_RIGHT:
            case WXK_NUMPAD_UP:    case WXK_UP:
            case WXK_NUMPAD_DOWN:  case WXK_DOWN:
            {
                m_running = false;
                m_up_action();
                break;
            }
            default: { break; }
            }
        }
        else if (type == wxEVT_KEY_DOWN) {
            bool apply = false;

            switch (keyCode)
            {
            case WXK_SHIFT:
            {
                if (m_running)
                    apply = true;

                break;
            }
            case WXK_NUMPAD_LEFT:
            case WXK_LEFT:
            {
                m_direction = -Vec3d::UnitX();
                apply = true;
                break;
            }
            case WXK_NUMPAD_RIGHT:
            case WXK_RIGHT:
            {
                m_direction = Vec3d::UnitX();
                apply = true;
                break;
            }
            case WXK_NUMPAD_UP:
            case WXK_UP:
            {
                m_direction = Vec3d::UnitY();
                apply = true;
                break;
            }
            case WXK_NUMPAD_DOWN:
            case WXK_DOWN:
            {
                m_direction = -Vec3d::UnitY();
                apply = true;
                break;
            }
            default: { break; }
            }

            if (apply) {
                m_running = true;
                m_down_action(m_direction, evt.ShiftDown(), evt.CmdDown());
            }
        }
    }
};

void GLCanvas3D::on_key(wxKeyEvent& evt)
{
    static GLCanvas3D const * thiz = nullptr;
    static TranslationProcessor translationProcessor(nullptr, nullptr);
    if (thiz != this) {
        thiz = this;
        translationProcessor = TranslationProcessor(
        [this]() {
            do_move(L("Tool Move"));
            m_gizmos.update_data();

            // BBS
            //wxGetApp().obj_manipul()->set_dirty();
            // Let the plater know that the dragging finished, so a delayed refresh
            // of the scene with the background processing data should be performed.
            post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
            // updates camera target constraints
            refresh_camera_scene_box();
            m_dirty = true;
        },
        [this](const Vec3d& direction, bool slow, bool camera_space) {
            //Orio: a volume of the "Modifiers" tool is placed with the tool, not moved alone with the arrows
            if (m_gizmos.is_ban_move_glvolume() || m_selection.is_only_modifier_tool_volumes()) {
                return;
            }
            m_selection.setup_cache();
            double multiplier = slow ? 1.0 : 10.0;

            Vec3d displacement;
            if (camera_space) {
                Eigen::Matrix<double, 3, 3, Eigen::DontAlign> inv_view_3x3 = get_active_camera().get_view_matrix().inverse().matrix().block(0, 0, 3, 3);
                displacement = multiplier * (inv_view_3x3 * direction);
                displacement.z() = 0.0;
            }
            else
                displacement = multiplier * direction;

            TransformationType trafo_type;
            trafo_type.set_relative();
            m_selection.translate(displacement, trafo_type);
            m_dirty = true;
        }
    );}

    const int keyCode = evt.GetKeyCode();

    auto imgui = wxGetApp().imgui();
    if (imgui->update_key_data(evt)) {
        render();
    }
    else
    {
        if (!m_gizmos.on_key(evt)) {
            if (evt.GetEventType() == wxEVT_KEY_UP) {
                if (evt.ShiftDown() && evt.ControlDown() && keyCode == WXK_SPACE) {
#if !BBL_RELEASE_TO_PUBLIC
                    wxGetApp().plater()->toggle_render_statistic_dialog();
                    m_dirty = true;
#endif
                } else if ((evt.ShiftDown() && evt.ControlDown() && keyCode == WXK_RETURN) ||
                    evt.ShiftDown() && evt.AltDown() && keyCode == WXK_RETURN) {
                    wxGetApp().plater()->toggle_show_wireframe();
                    m_dirty = true;
                } else if ((evt.ShiftDown() && evt.ControlDown() && keyCode == 'T')) {
                    wxGetApp().plater()->toggle_text_cs();
                    m_dirty = true;
                } else if ((evt.ShiftDown() && evt.ControlDown() && keyCode == 'L')) {
                    wxGetApp().plater()->toggle_non_manifold_edges();
                    m_dirty = true;
                }
                else if (m_tab_down && keyCode == WXK_TAB && !evt.HasAnyModifiers()) {
                    // Enable switching between 3D and Preview with Tab
                    // m_canvas->HandleAsNavigationKey(evt);   // XXX: Doesn't work in some cases / on Linux
                    post_event(SimpleEvent(EVT_GLCANVAS_TAB));
                }
                else if (keyCode == WXK_SHIFT) {
                    translationProcessor.process(evt);

                    if (m_picking_enabled && m_rectangle_selection.is_dragging()) {
                        _update_selection_from_hover();
                        m_rectangle_selection.stop_dragging();
                        m_mouse.ignore_left_up = true;
                        m_dirty = true;
                    }
//                    set_cursor(Standard);
                }
                else if (keyCode == WXK_ALT) {
                    if (m_picking_enabled && m_rectangle_selection.is_dragging()) {
                        _update_selection_from_hover();
                        m_rectangle_selection.stop_dragging();
                        m_mouse.ignore_left_up = true;
                        m_dirty = true;
                    }
//                    set_cursor(Standard);
                }
                else if (keyCode == WXK_CONTROL)
                    m_dirty = true;
                else if (m_gizmos.is_enabled() && !m_selection.is_empty() && m_canvas_type != CanvasAssembleView) {
                    translationProcessor.process(evt);

                    switch (keyCode)
                    {
                    case WXK_NUMPAD_PAGEUP:   case WXK_PAGEUP:
                    case WXK_NUMPAD_PAGEDOWN: case WXK_PAGEDOWN:
                    {
                        if (m_gizmos.get_gizmo_active_condition(GLGizmosManager::EType::Rotate)) {
                            do_rotate(L("Tool Rotate"));
                            m_gizmos.update_data();

                            // BBS
                            // wxGetApp().obj_manipul()->set_dirty();
                            // Let the plater know that the dragging finished, so a delayed refresh
                            // of the scene with the background processing data should be performed.
                            post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
                            // updates camera target constraints
                            refresh_camera_scene_box();
                            m_dirty = true;
                        }
                        break;
                    }
                    default: { break; }
                    }
                }

                // BBS: add select view logic
                if (evt.ControlDown()) {
                    switch (keyCode) {
                        case '0':
                        case WXK_NUMPAD0: //0 on numpad
                            { select_view("plate");
                              zoom_to_bed();
                            break;
                        }
                        case '1':
                        case WXK_NUMPAD1: //1 on numpad
                            { select_view("top"); break; }
                        case '2':
                        case WXK_NUMPAD2: //2 on numpad
                            { select_view("bottom"); break; }
                        case '3':
                        case WXK_NUMPAD3: //3 on numpad
                        {
                            select_view("front");
                            m_gizmos.update_paint_base_camera_rotate_rad();
                            break;
                        }
                        case '4':
                        case WXK_NUMPAD4: //4 on numpad
                        {
                            select_view("rear");
                            m_gizmos.update_paint_base_camera_rotate_rad();
                            break;
                        }
                        case '5':
                        case WXK_NUMPAD5: //5 on numpad
                        {
                            select_view("left");
                            m_gizmos.update_paint_base_camera_rotate_rad();
                            break;
                        }
                        case '6':
                        case WXK_NUMPAD6: //6 on numpad
                        {
                            select_view("right");
                            m_gizmos.update_paint_base_camera_rotate_rad();
                            break;
                        }
                        case '7':
                        case WXK_NUMPAD7: //7 on numpad
                        {
                            select_view("iso");
                            m_gizmos.update_paint_base_camera_rotate_rad();
                            break;
                        }
                        default: break;
                    }
                }
            }
            else if (evt.GetEventType() == wxEVT_KEY_DOWN) {
                m_tab_down = keyCode == WXK_TAB && !evt.HasAnyModifiers();
                if (keyCode == WXK_SHIFT) {
                    translationProcessor.process(evt);

                    if (m_picking_enabled && (m_gizmos.get_current_type() != GLGizmosManager::SlaSupports))
                    {
                        m_mouse.ignore_left_up = false;
//                        set_cursor(Cross);
                    }
                }
                else if (keyCode == WXK_ALT) {
                    if (m_picking_enabled && (m_gizmos.get_current_type() != GLGizmosManager::SlaSupports))
                    {
                        m_mouse.ignore_left_up = false;
//                        set_cursor(Cross);
                    }
                }
                else if (keyCode == WXK_CONTROL)
                    m_dirty = true;
                else if (keyCode == WXK_TAB && evt.ShiftDown() && !evt.ControlDown() && !wxGetApp().is_gcode_viewer()) {
                    // Collapse side-panel with Shift+Tab
                    post_event(SimpleEvent(EVT_GLCANVAS_COLLAPSE_SIDEBAR));
                } else if (m_gizmos.is_enabled() && !m_selection.is_empty() && m_canvas_type != CanvasAssembleView) {
                    auto _do_rotate = [this](double angle_z_rad) {
                        if (m_gizmos.is_ban_move_glvolume()) {
                            return;
                        }
                        if (!m_gizmos.get_gizmo_active_condition(GLGizmosManager::EType::Rotate)) {
                            return;
                        }
                        m_selection.setup_cache();
                        m_selection.rotate(Vec3d(0.0, 0.0, angle_z_rad), TransformationType(TransformationType::World_Relative_Joint));
                        m_dirty = true;
//                        wxGetApp().obj_manipul()->set_dirty();
                    };

                    translationProcessor.process(evt);

                    switch (keyCode)
                    {
                    case WXK_NUMPAD_PAGEUP:   case WXK_PAGEUP:   { _do_rotate(0.25 * M_PI); break; }
                    case WXK_NUMPAD_PAGEDOWN: case WXK_PAGEDOWN: { _do_rotate(-0.25 * M_PI); break; }
                    default: { break; }
                    }
                } else if (!m_gizmos.is_enabled()) {
                    // DoubleSlider navigation in Preview
                    if (m_canvas_type == CanvasPreview) {
                        IMSlider *m_layers_slider = get_gcode_viewer().get_layers_slider();
                        IMSlider *m_moves_slider  = get_gcode_viewer().get_moves_slider();
                        if (evt.CmdDown() || evt.ShiftDown()) {
                            if (evt.GetKeyCode() == 'G') {
                                m_layers_slider->show_go_to_layer(true);
                            }
                            IMSlider *m_moves_slider  = get_gcode_viewer().get_moves_slider();
                            if (keyCode == WXK_UP || keyCode == WXK_DOWN) {
                                // Orio : sur le curseur vertical affiche (couches Z, ou coupe X/Y),
                                // plus sur le curseur Z quand il est cache ; marque modifie, applique dans render_gcode()
                                get_gcode_viewer().move_vertical_slider(keyCode == WXK_UP ? 5 : -5);
                            } else if (keyCode == WXK_LEFT || keyCode == WXK_RIGHT) {
                                const int new_pos = keyCode == WXK_RIGHT ? m_moves_slider->GetHigherValue() + 5 : m_moves_slider->GetHigherValue() - 5;
                                m_moves_slider->SetHigherValue(new_pos);
                                // BBS set as dirty, update in render_gcode()
                                m_moves_slider->set_as_dirty();
                            }
                        }
                        else if (keyCode == WXK_UP || keyCode == WXK_DOWN) {
                            // Orio : sur le curseur vertical affiche (couches Z, ou coupe X/Y),
                            // plus sur le curseur Z quand il est cache ; marque modifie, applique dans render_gcode()
                            get_gcode_viewer().move_vertical_slider(keyCode == WXK_UP ? 1 : -1);
                        } else if (keyCode == WXK_LEFT || keyCode == WXK_RIGHT) {
                            const int new_pos = keyCode == WXK_RIGHT ? m_moves_slider->GetHigherValue() + 1 : m_moves_slider->GetHigherValue() - 1;
                            m_moves_slider->SetHigherValue(new_pos);
                            // BBS set as dirty, update in render_gcode()
                            m_moves_slider->set_as_dirty();
                        }
                        m_dirty = true;
                    }
                }
            }
        }
        else return;
    }

    if (keyCode != WXK_TAB
        && keyCode != WXK_LEFT
        && keyCode != WXK_UP
        && keyCode != WXK_RIGHT
        && keyCode != WXK_DOWN) {
        evt.Skip();   // Needed to have EVT_CHAR generated as well
    }
}

void GLCanvas3D::on_mouse_wheel(wxMouseEvent& evt)
{
#ifdef WIN32
    // Try to filter out spurious mouse wheel events comming from 3D mouse.
    if (wxGetApp().plater()->get_mouse3d_controller().process_mouse_wheel())
        return;
#endif

    if (!m_initialized)
        return;

    // Ignore the wheel events if the middle button is pressed.
    if (evt.MiddleIsDown())
        return;

#if ENABLE_RETINA_GL
    const float scale = m_retina_helper->get_scale_factor();
    evt.SetX(evt.GetX() * scale);
    evt.SetY(evt.GetY() * scale);
#endif

    if (wxGetApp().imgui()->update_mouse_data(evt)) {
        if (m_canvas_type == CanvasPreview) {
            // Orio : la molette va au curseur vertical affiche (couches Z, ou coupe X/Y), plus au
            // curseur Z quand il est cache derriere un curseur de coupe
            IMSlider* m_moves_slider = get_gcode_viewer().get_moves_slider();
            get_gcode_viewer().on_vertical_slider_mouse_wheel(evt);
            m_moves_slider->on_mouse_wheel(evt);
        }
        else if (m_canvas_type == CanvasView3D) {
            //Orio: the wheel over the cut slider of the Prepare tab moves it.
            m_prepare_cut.on_mouse_wheel(evt);
        }
        render();
        m_dirty = true;
        return;
    }

#ifdef __WXMSW__
	// For some reason the Idle event is not being generated after the mouse scroll event in case of scrolling with the two fingers on the touch pad,
	// if the event is not allowed to be passed further.
    // evt.Skip() used to trigger the needed screen refresh, but it does no more. wxWakeUpIdle() seem to work now.
    wxWakeUpIdle();
#endif /* __WXMSW__ */

    //Orio: performs variable infill / walls editing updates, if enabled
    if (is_infill_walls_editing_enabled()) {
        int object_idx_selected = m_selection.get_object_idx();
        if (object_idx_selected != -1) {
            // A volume is selected. Test, whether hovering over the infill / walls bar; with Ctrl (Cmd on macOS), anywhere in
            // the scene: the brush is sized without leaving the part.
            if (evt.CmdDown() || m_infill_walls_editing.bar_rect_contains(*this, (float)evt.GetX(), (float)evt.GetY())) {
                // Adjust the width of the selection.
                m_infill_walls_editing.band_width = std::max(std::min(m_infill_walls_editing.band_width * (1.0f + 0.1f * (float)evt.GetWheelRotation() / (float)evt.GetWheelDelta()), 10.0f), 1.5f);
                if (m_canvas != nullptr)
                    m_canvas->Refresh();

                return;
            }
        }
    }

    //BBS: performs variable speed editing updates, if enabled
    if (is_speed_editing_enabled()) {
        int object_idx_selected = m_selection.get_object_idx();
        if (object_idx_selected != -1) {
            // A volume is selected. Test, whether hovering over the speed bar; Orio: with Ctrl (Cmd on macOS), anywhere
            // in the scene: the brush is sized without leaving the part.
            if (evt.CmdDown() || m_speed_editing.bar_rect_contains(*this, (float)evt.GetX(), (float)evt.GetY())) {
                // Adjust the width of the selection.
                m_speed_editing.band_width = std::max(std::min(m_speed_editing.band_width * (1.0f + 0.1f * (float)evt.GetWheelRotation() / (float)evt.GetWheelDelta()), 10.0f), 1.5f);
                if (m_canvas != nullptr)
                    m_canvas->Refresh();

                return;
            }
        }
    }

    // Performs layers editing updates, if enabled
    if (is_layers_editing_enabled()) {
        int object_idx_selected = m_selection.get_object_idx();
        if (object_idx_selected != -1) {
            // A volume is selected. Test, whether hovering over a layer thickness bar.
            //Orio: or Ctrl held, anywhere in the scene (on the part, where the band is seen too).
            if (evt.ControlDown() || m_layers_editing.bar_rect_contains(*this, (float)evt.GetX(), (float)evt.GetY())) {
                // Adjust the width of the selection.
                m_layers_editing.band_width = std::max(std::min(m_layers_editing.band_width * (1.0f + 0.1f * (float)evt.GetWheelRotation() / (float)evt.GetWheelDelta()), 10.0f), 1.5f);
                if (m_canvas != nullptr)
                    m_canvas->Refresh();

                return;
            }
        }
    }

    // Inform gizmos about the event so they have the opportunity to react.
    if (m_gizmos.on_mouse_wheel(evt))
        return;

    if (m_canvas_type == CanvasAssembleView && (evt.AltDown() || evt.CmdDown())) {
        float rotation = (float)evt.GetWheelRotation() / (float)evt.GetWheelDelta();
        if (evt.AltDown()) {
            auto clp_dist = m_gizmos.m_assemble_view_data->model_objects_clipper()->get_position();
            clp_dist = rotation < 0.f
                ? std::max(0., clp_dist - 0.01)
                : std::min(1., clp_dist + 0.01);
            m_gizmos.m_assemble_view_data->model_objects_clipper()->set_position(clp_dist, true);
        }
        else if (evt.CmdDown()) {
            m_explosion_ratio = rotation < 0.f
                ? std::max(1., m_explosion_ratio - 0.01)
                : std::min(3., m_explosion_ratio + 0.01);
            if (m_explosion_ratio != GLVolume::explosion_ratio) {
                for (GLVolume* volume : m_volumes.volumes) {
                    volume->set_bounding_boxes_as_dirty();
                }
                GLVolume::explosion_ratio = m_explosion_ratio;
            }
        }
        return;
    }
    // Calculate the zoom delta and apply it to the current zoom factor
#ifdef SUPPORT_REVERSE_MOUSE_ZOOM
    double direction_factor = (wxGetApp().app_config->get("reverse_mouse_wheel_zoom") == "1") ? -1.0 : 1.0;
#else
    double direction_factor = 1.0;
#endif
    auto delta = direction_factor * (double)evt.GetWheelRotation() / (double)evt.GetWheelDelta();
    bool zoom_to_mouse = wxGetApp().app_config->get("zoom_to_mouse") == "true";
    if (!zoom_to_mouse) {// zoom to center
        _update_camera_zoom(delta);
    }
    else {
        auto cnv_size = get_canvas_size();
        Camera& camera = get_active_camera();
        //Orio: the point under the mouse and the centre of the view at the same depth: the one of what the mouse
        // hovers, else the one of the target of the camera. Over the background the depth is the far plane: the camera
        // was pulled towards it at each step and the far plane moved away with it, up to infinity (grey view, no view
        // cube, "fit" without effect). At the same depth, the camera only moves across the view.
        const auto &viewport = camera.get_viewport();
        GLfloat     mouse_z  = 1.0f;
        wxGetApp().get_opengl_manager()->read_pixel("", (GLint)evt.GetX(), (GLint)(viewport[3] - evt.GetY()), 1, 1, EPixelFormat::DepthComponent,
                                                    EPixelDataType::Float, (void*)&mouse_z);
        if (!(mouse_z > 0.0f && mouse_z < 1.0f)) {
            const Vec3d target = camera.get_target();
            const Vec4d clip   = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix() * Vec4d(target.x(), target.y(), target.z(), 1.0);
            mouse_z = clip.w() != 0.0 ? float(0.5 * (clip.z() / clip.w() + 1.0)) : 0.5f;
        }
        auto screen_center_3d_pos = _mouse_to_3d(camera, { cnv_size.get_width() * 0.5, cnv_size.get_height() * 0.5 }, &mouse_z);
        auto mouse_3d_pos = _mouse_to_3d(camera, {evt.GetX(), evt.GetY()}, &mouse_z);
        Vec3d displacement = mouse_3d_pos - screen_center_3d_pos;
        if (!displacement.allFinite()) {
            _update_camera_zoom(delta);
            return;
        }
        camera.translate(displacement);
        auto origin_zoom = camera.get_zoom();
        _update_camera_zoom(delta);
        auto new_zoom = camera.get_zoom();
        camera.translate((-displacement) / (new_zoom / origin_zoom));
    }
}

void GLCanvas3D::on_timer(wxTimerEvent& evt)
{
    if (m_layers_editing.state == LayersEditing::Editing)
        _perform_layer_editing_action();
    //BBS: variable print speed along Z.
    else if (m_speed_editing.state == SpeedEditing::Editing)
        _perform_speed_editing_action();
    //Orio: variable sparse infill density / wall count along Z.
    else if (m_infill_walls_editing.state == InfillWallsEditing::Editing)
        _perform_infill_walls_editing_action();
}

void GLCanvas3D::on_render_timer(wxTimerEvent& evt)
{
    // no need to wake up idle
    // right after this event, idle event is fired
    // m_dirty = true;
    // wxWakeUpIdle();
}

void GLCanvas3D::on_set_color_timer(wxTimerEvent& evt)
{
    auto obj_list = wxGetApp().obj_list();
    if (m_gizmos.get_current_type() != GLGizmosManager::MmuSegmentation && m_color_input_value > 0) { obj_list->set_extruder_for_selected_items(m_color_input_value); }
    m_color_input_value = -1;
    m_timer_set_color.Stop();
}


void GLCanvas3D::schedule_extra_frame(int miliseconds)
{
    // Schedule idle event right now
    if (miliseconds == 0)
    {
        // We want to wakeup idle evnt but most likely this is call inside render cycle so we need to wait
        if (m_in_render)
            miliseconds = 33;
        else {
            m_dirty = true;
            wxWakeUpIdle();
            return;
        }
    }
    int remaining_time = m_render_timer.GetInterval();
    // Timer is not running
    if (!m_render_timer.IsRunning()) {
        m_render_timer.StartOnce(miliseconds);
    // Timer is running - restart only if new period is shorter than remaning period
    } else {
        if (miliseconds + 20 < remaining_time) {
            m_render_timer.Stop();
            m_render_timer.StartOnce(miliseconds);
        }
    }
}

int GLCanvas3D::get_main_toolbar_item_id(const std::string& name) const
{
    if (!m_main_toolbar) {
        return -1;
    }
    return m_main_toolbar->get_item_id(name);
}

void GLCanvas3D::force_main_toolbar_left_action(int item_id)
{
    if (!m_main_toolbar) {
        return;
    }

    m_dirty |= m_main_toolbar->update_items_state();
    m_main_toolbar->force_left_action(item_id, *this);
}

void GLCanvas3D::force_main_toolbar_right_action(int item_id)
{
    m_main_toolbar->force_right_action(item_id, *this);
}

#ifndef NDEBUG
// #define SLIC3R_DEBUG_MOUSE_EVENTS
#endif

#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
std::string format_mouse_event_debug_message(const wxMouseEvent &evt)
{
	static int idx = 0;
	char buf[2048];
	std::string out;
	sprintf(buf, "Mouse Event %d - ", idx ++);
	out = buf;

	if (evt.Entering())
		out += "Entering ";
	if (evt.Leaving())
		out += "Leaving ";
	if (evt.Dragging())
		out += "Dragging ";
	if (evt.Moving())
		out += "Moving ";
	if (evt.Magnify())
		out += "Magnify ";
	if (evt.LeftDown())
		out += "LeftDown ";
	if (evt.LeftUp())
		out += "LeftUp ";
	if (evt.LeftDClick())
		out += "LeftDClick ";
	if (evt.MiddleDown())
		out += "MiddleDown ";
	if (evt.MiddleUp())
		out += "MiddleUp ";
	if (evt.MiddleDClick())
		out += "MiddleDClick ";
	if (evt.RightDown())
		out += "RightDown ";
	if (evt.RightUp())
		out += "RightUp ";
	if (evt.RightDClick())
		out += "RightDClick ";

	sprintf(buf, "(%d, %d)", evt.GetX(), evt.GetY());
	out += buf;
	return out;
}
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */

void GLCanvas3D::on_gesture(wxGestureEvent &evt)
{
    if (!m_initialized || !_set_current(true))
        return;

    auto & camera = get_active_camera();
    if (evt.GetEventType() == wxEVT_GESTURE_PAN) {
        auto p = evt.GetPosition();
        auto d = static_cast<wxPanGestureEvent&>(evt).GetDelta();
        float z = 0;
        const Vec3d &p2 = _mouse_to_3d(camera, {p.x, p.y}, &z);
        const Vec3d &p1 = _mouse_to_3d(camera, {p.x - d.x, p.y - d.y}, &z);
        camera.set_target(camera.get_target() + p1 - p2);
    } else if (evt.GetEventType() == wxEVT_GESTURE_ZOOM) {
        static float zoom_start = 1;
        if (evt.IsGestureStart())
            zoom_start = camera.get_zoom();
        camera.set_zoom(zoom_start * static_cast<wxZoomGestureEvent&>(evt).GetZoomFactor());
    } else if (evt.GetEventType() == wxEVT_GESTURE_ROTATE) {
        PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        bool rotate_limit = current_printer_technology() != ptSLA;
        static double last_rotate = 0;
        if (evt.IsGestureStart())
            last_rotate = 0;
        auto rotate = static_cast<wxRotateGestureEvent&>(evt).GetRotationAngle() - last_rotate;
        last_rotate += rotate;
        if (plate)
            camera.rotate_on_sphere_with_target(-rotate, 0, rotate_limit, plate->get_bounding_box().center());
        else
            camera.rotate_on_sphere(-rotate, 0, rotate_limit);
    }
    m_dirty = true;
}

//Orio: GLVolume of a volume of the "Modifiers" tool.
bool GLCanvas3D::is_modifier_tool_glvolume(const GLVolume *volume) const
{
    if (volume == nullptr || volume->is_wipe_tower || m_model == nullptr)
        return false;
    const int object_idx = volume->object_idx();
    const int volume_idx = volume->volume_idx();
    if (object_idx < 0 || object_idx >= int(m_model->objects.size()))
        return false;
    const ModelObject *model_object = m_model->objects[object_idx];
    return volume_idx >= 0 && volume_idx < int(model_object->volumes.size()) && model_object->volumes[volume_idx]->is_modifier_tool_volume();
}

void GLCanvas3D::on_mouse(wxMouseEvent& evt)
{
    if (!m_initialized || !_set_current(true))
        return;

    // BBS: single snapshot
    Plater::SingleSnapshot single(wxGetApp().plater());

#if ENABLE_RETINA_GL
    const float scale = m_retina_helper->get_scale_factor();
    evt.SetX(evt.GetX() * scale);
    evt.SetY(evt.GetY() * scale);
#endif

    Point pos(evt.GetX(), evt.GetY());

    ImGuiWrapper* imgui = wxGetApp().imgui();
    if (m_tooltip.is_in_imgui() && evt.LeftUp())
        // ignore left up events coming from imgui windows and not processed by them
        m_mouse.ignore_left_up = true;
    m_tooltip.set_in_imgui(false);
    if (imgui->update_mouse_data(evt)) {
        if (evt.LeftDown() && m_canvas != nullptr)
            m_canvas->SetFocus();
        m_mouse.position = evt.Leaving() ? Vec2d(-1.0, -1.0) : pos.cast<double>();
        m_tooltip.set_in_imgui(true);
        render();
#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
        printf((format_mouse_event_debug_message(evt) + " - Consumed by ImGUI\n").c_str());
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */
        m_dirty = true;
        // do not return if dragging or tooltip not empty to allow for tooltip update
        // also, do not return if the mouse is moving and also is inside MM gizmo to allow update seed fill selection
        if (!m_mouse.dragging && m_tooltip.is_empty() && (m_gizmos.get_current_type() != GLGizmosManager::MmuSegmentation || !evt.Moving()))
            return;
    }

#ifdef __WXMSW__
	bool on_enter_workaround = false;
    if (! evt.Entering() && ! evt.Leaving() && m_mouse.position.x() == -1.0) {
        // Workaround for SPE-832: There seems to be a mouse event sent to the window before evt.Entering()
        m_mouse.position = pos.cast<double>();
        render();
#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
		printf((format_mouse_event_debug_message(evt) + " - OnEnter workaround\n").c_str());
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */
		on_enter_workaround = true;
    } else
#endif /* __WXMSW__ */
    {
#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
		printf((format_mouse_event_debug_message(evt) + " - other\n").c_str());
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */
	}

    const auto& p_main_toolbar = get_main_toolbar();
    if (p_main_toolbar) {
        if (p_main_toolbar->on_mouse(evt, *this)) {
            if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
                mouse_up_cleanup();
            m_mouse.set_start_position_3D_as_invalid();
            return;
        }
    }

    if (wxGetApp().plater()->get_collapse_toolbar().on_mouse(evt, *this)) {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }

    // BBS
#if 0
    if (wxGetApp().plater()->get_view_toolbar().on_mouse(evt, *this)) {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }
#endif

    for (GLVolume* volume : m_volumes.volumes) {
        volume->force_sinking_contours = false;
    }

    auto show_sinking_contours = [this]() {
        const Selection::IndicesList& idxs = m_selection.get_volume_idxs();
        for (unsigned int idx : idxs) {
            m_volumes.volumes[idx]->force_sinking_contours = true;
        }
        m_dirty = true;
    };

    if (m_gizmos.on_mouse(evt)) {
        if (m_gizmos.is_running()) {
            _deactivate_arrange_menu();
            _deactivate_orient_menu();
            _deactivate_layersediting_menu();
            //BBS: variable print speed along Z.
            _deactivate_speedediting_menu();
            //Orio: variable sparse infill density / wall count along Z.
            _deactivate_infillwallsediting_menu();
        }
        if (wxWindow::FindFocus() != m_canvas)
            // Grab keyboard focus for input in gizmo dialogs.
            m_canvas->SetFocus();

        if (evt.LeftDown()) {
            // Clear hover state in main toolbar
            wxMouseEvent evt2 = evt;
            evt2.SetEventType(wxEVT_MOTION);
            evt2.SetLeftDown(false);
            const auto& p_main_toolbar = get_main_toolbar();
            if (p_main_toolbar) {
                p_main_toolbar->on_mouse(evt2, *this);
            }
        }

        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();

        m_mouse.set_start_position_3D_as_invalid();
        m_mouse.position = pos.cast<double>();

        if (evt.Dragging() && current_printer_technology() == ptFFF && (fff_print()->config().print_sequence == PrintSequence::ByObject)) {
            if (can_sequential_clearance_show_in_gizmo()) {
                update_sequential_clearance();
            }
        }
        else if (evt.Dragging()) {
            switch (m_gizmos.get_current_type())
            {
            case GLGizmosManager::EType::Move:
            case GLGizmosManager::EType::Scale:
            case GLGizmosManager::EType::Rotate:
            {
                show_sinking_contours();
                break;
            }
            default: { break; }
            }
        }
        else if (evt.LeftUp() &&
            m_gizmos.get_current_type() == GLGizmosManager::EType::Scale &&
            m_gizmos.get_current()->get_state() == GLGizmoBase::EState::On) {
            wxGetApp().obj_list()->selection_changed();
        }

        return;
    }

    bool any_gizmo_active = m_gizmos.get_current() != nullptr;

    int selected_object_idx = m_selection.get_object_idx();
    int layer_editing_object_idx = is_layers_editing_enabled() ? selected_object_idx : -1;
    //BBS: variable print speed along Z.
    int speed_editing_object_idx = is_speed_editing_enabled() ? selected_object_idx : -1;
    //Orio: variable sparse infill density / wall count along Z.
    int infill_walls_editing_object_idx = is_infill_walls_editing_enabled() ? selected_object_idx : -1;


    if (m_mouse.drag.move_requires_threshold && m_mouse.is_move_start_threshold_position_2D_defined() && m_mouse.is_move_threshold_met(pos)) {
        m_mouse.drag.move_requires_threshold = false;
        m_mouse.set_move_start_threshold_position_2D_as_invalid();
    }

    if (evt.ButtonDown() && wxWindow::FindFocus() != m_canvas)
        // Grab keyboard focus on any mouse click event.
        m_canvas->SetFocus();

    if (evt.Entering()) {
//#if defined(__WXMSW__) || defined(__linux__)
//        // On Windows and Linux needs focus in order to catch key events
        // Set focus in order to remove it from sidebar fields
        if (m_canvas != nullptr) {
            // Only set focus, if the top level window of this canvas is active.
            auto p = dynamic_cast<wxWindow*>(evt.GetEventObject());
            while (p->GetParent())
                p = p->GetParent();
            auto *top_level_wnd = dynamic_cast<wxTopLevelWindow*>(p);
            if (top_level_wnd && top_level_wnd->IsActive() && !wxGetApp().get_side_menu_popup_status()) {
                // m_canvas->SetFocus();
            }
            m_mouse.position = pos.cast<double>();
            m_tooltip_enabled = false;
            // 1) forces a frame render to ensure that m_hover_volume_idxs is updated even when the user right clicks while
            // the context menu is shown, ensuring it to disappear if the mouse is outside any volume and to
            // change the volume hover state if any is under the mouse
            // 2) when switching between 3d view and preview the size of the canvas changes if the side panels are visible,
            // so forces a resize to avoid multiple renders with different sizes (seen as flickering)
            _refresh_if_shown_on_screen();
            m_tooltip_enabled = true;
        }
        m_mouse.set_start_position_2D_as_invalid();
//#endif
    }
    else if (evt.Leaving()) {
        // to remove hover on objects when the mouse goes out of this canvas
        m_mouse.position = Vec2d(-1.0, -1.0);
        m_dirty = true;
    }
    else if (evt.LeftDClick()) {
        // switch to object panel if double click on object, otherwise switch to global panel if double click on background
        if (selected_object_idx >= 0)
            post_event(SimpleEvent(EVT_GLCANVAS_SWITCH_TO_OBJECT));
        else
            post_event(SimpleEvent(EVT_GLCANVAS_SWITCH_TO_GLOBAL));
    }
    else if (evt.LeftDown() || evt.RightDown() || evt.MiddleDown()) {
        m_show_assembly_view_preview_menu = false;
        //BBS: add orient deactivate logic
        if (!m_gizmos.on_mouse(evt)) {
            if (_deactivate_arrange_menu() || _deactivate_orient_menu())
                return;
        }

        // If user pressed left or right button we first check whether this happened on a volume or not.
        m_layers_editing.state = LayersEditing::Unknown;
        //BBS: variable print speed along Z.
        m_speed_editing.state = SpeedEditing::Unknown;
        m_infill_walls_editing.state = InfillWallsEditing::Unknown;
        if (layer_editing_object_idx != -1 && m_layers_editing.bar_rect_contains(*this, pos(0), pos(1))) {
            // A volume is selected and the mouse is inside the layer thickness bar.
            // Start editing the layer height.
            m_layers_editing.state = LayersEditing::Editing;
            m_layers_editing.part_stroke = false;
            _perform_layer_editing_action(&evt);
        }
        //Orio: or on the edited object itself, seen under the mouse (not through another one), with the same
        //buttons as on the bar; the middle button still pans the view.
        else if (layer_editing_object_idx != -1 && ! evt.MiddleDown() && ! m_hover_volume_idxs.empty() &&
                 m_volumes.volumes[get_first_hover_volume_idx()]->object_idx() == layer_editing_object_idx &&
                 ! std::isnan(m_layers_editing.part_coordinate_at(*this, pos(0), pos(1)))) {
            m_layers_editing.state = LayersEditing::Editing;
            m_layers_editing.part_stroke = true;
            _perform_layer_editing_action(&evt);
        }
        //BBS: a volume is selected and the mouse is inside the variable speed bar.
        else if (speed_editing_object_idx != -1 && m_speed_editing.bar_rect_contains(*this, pos(0), pos(1))) {
            m_speed_editing.state = SpeedEditing::Editing;
            m_speed_editing.part_stroke = false;
            _perform_speed_editing_action(&evt);
        }
        //Orio: or on the edited object itself, seen under the mouse (not through another one), with the same
        //buttons as on the bar; the middle button still pans the view.
        else if (speed_editing_object_idx != -1 && ! evt.MiddleDown() && ! m_hover_volume_idxs.empty() &&
                 m_volumes.volumes[get_first_hover_volume_idx()]->object_idx() == speed_editing_object_idx &&
                 ! std::isnan(m_speed_editing.part_coordinate_at(*this, pos(0), pos(1)))) {
            m_speed_editing.state = SpeedEditing::Editing;
            m_speed_editing.part_stroke = true;
            _perform_speed_editing_action(&evt);
        }
        //Orio: a volume is selected and the mouse is inside the variable infill / walls bar.
        else if (infill_walls_editing_object_idx != -1 && m_infill_walls_editing.bar_rect_contains(*this, pos(0), pos(1))) {
            m_infill_walls_editing.state = InfillWallsEditing::Editing;
            m_infill_walls_editing.part_stroke = false;
            _perform_infill_walls_editing_action(&evt);
        }
        //Orio: or on the edited object itself, as for the variable speed.
        else if (infill_walls_editing_object_idx != -1 && ! evt.MiddleDown() && ! m_hover_volume_idxs.empty() &&
                 m_volumes.volumes[get_first_hover_volume_idx()]->object_idx() == infill_walls_editing_object_idx &&
                 ! std::isnan(m_infill_walls_editing.part_coordinate_at(*this, pos(0), pos(1)))) {
            m_infill_walls_editing.state = InfillWallsEditing::Editing;
            m_infill_walls_editing.part_stroke = true;
            _perform_infill_walls_editing_action(&evt);
        }

        else {
            //Orio: a click on a volume of the "Modifiers" tool (modifier, negative volume, support blocker /
            //enforcer, part placed by the tool) selects its whole object, and dragging moves the object: such a volume
            //is no more moved alone (Move [M]), it is placed again with the tool. Alt does not change that.
            const bool hover_tool_volume = !m_hover_volume_idxs.empty() && is_modifier_tool_glvolume(m_volumes.volumes[get_first_hover_volume_idx()]);
            // BBS: define Alt key to enable volume selection mode
            m_selection.set_volume_selection_mode((evt.AltDown() && !hover_tool_volume) ? Selection::Volume : Selection::Instance);
            if (evt.LeftDown() && evt.ShiftDown() && m_picking_enabled && m_layers_editing.state != LayersEditing::Editing && m_speed_editing.state != SpeedEditing::Editing &&
                m_infill_walls_editing.state != InfillWallsEditing::Editing &&
                //Orio: no rectangle selection while the variable speed tool is open: drawn in the void (where the hidden
                // objects are, for instance), it deselected the edited object and closed the window of the tool. The same for
                // the variable infill / walls and the variable layer height tools.
                ! is_speed_editing_enabled() && ! is_infill_walls_editing_enabled() && ! is_layers_editing_enabled()) {
                if (m_gizmos.get_current_type() != GLGizmosManager::SlaSupports
                    && !m_gizmos.is_paint_gizmo()) {
                    m_rectangle_selection.start_dragging(m_mouse.position, evt.ShiftDown() ? GLSelectionRectangle::Select : GLSelectionRectangle::Deselect);
                    m_dirty = true;
                }
            }
            else {
                // Select volume in this 3D canvas.
                // Don't deselect a volume if layer editing is enabled or any gizmo is active. We want the object to stay selected
                // during the scene manipulation.

                if (m_picking_enabled && (m_gizmos.is_allow_multi_select_parts_or_objects() || !evt.CmdDown()) && (!m_hover_volume_idxs.empty())) {
                    if (evt.LeftDown() && !m_hover_volume_idxs.empty()) {
                        int volume_idx = get_first_hover_volume_idx();
                        //Orio: a volume of the "Modifiers" tool is selected through a solid part of its instance
                        if (hover_tool_volume) {
                            const GLVolume *tool_volume = m_volumes.volumes[volume_idx];
                            for (int i = 0; i < int(m_volumes.volumes.size()); ++i) {
                                const GLVolume *v = m_volumes.volumes[i];
                                if (v->object_idx() == tool_volume->object_idx() && v->instance_idx() == tool_volume->instance_idx() && !v->is_modifier &&
                                    !is_modifier_tool_glvolume(v)) {
                                    volume_idx = i;
                                    break;
                                }
                            }
                        }
                        bool already_selected = m_selection.contains_volume(volume_idx);
                        bool ctrl_down = evt.CmdDown();
                        bool alt_down  = evt.AltDown() && !hover_tool_volume;
                        Selection::IndicesList curr_idxs = m_selection.get_volume_idxs();
                        if (already_selected && ctrl_down)
                            m_selection.remove(volume_idx);
                        else if (alt_down) {//support  select multi parts
                            if (m_selection.is_single_full_object()) {
                                m_selection.clear();
                                curr_idxs.clear();
                            }
                            Selection::EMode mode = Selection::Volume;
                            std::vector<int> volume_idxs;
                            for (auto idx : curr_idxs) {
                                volume_idxs.emplace_back(idx);
                            }
                            if (already_selected) {
                                if (!is_in_same_model_object(volume_idxs)) {
                                    m_selection.clear();
                                    m_selection.add_volumes(mode, {(unsigned int) volume_idx}, false);
                                } else {
                                    m_selection.remove_volumes(mode, {(unsigned int) volume_idx});
                                }
                            } else {
                                volume_idxs.emplace_back(volume_idx);
                                if (!is_in_same_model_object(volume_idxs)) {
                                    m_selection.clear();
                                }
                                m_selection.add_volumes(mode, {(unsigned int) volume_idx}, false);
                            }
                        }
                        else {
                            bool change_another_part = false;
                            auto cur_selection_mode = m_selection.get_mode();
                            if (cur_selection_mode == Selection::Volume && !alt_down && !already_selected && curr_idxs.size() > 0 && !hover_tool_volume) {
                                std::vector<int> volume_idxs;
                                for (auto idx : curr_idxs) { volume_idxs.emplace_back(idx); }
                                auto temp_gl_volume        = m_selection.get_volume(volume_idx);
                                auto new_select_object_idx = temp_gl_volume->object_idx();
                                auto first_gl_volume       = m_selection.get_volume(volume_idxs[0]);
                                if (first_gl_volume->object_idx() == new_select_object_idx) {
                                    Selection::EMode mode = Selection::Volume;
                                    m_selection.clear();
                                    m_selection.add_volumes(mode, {(unsigned int) volume_idx}, false);
                                    change_another_part = true;
                                }
                            }
                            if (!change_another_part) {
                                m_selection.add(volume_idx, !ctrl_down, true);
                                m_mouse.drag.move_requires_threshold = !already_selected;
                                if (already_selected)
                                    m_mouse.set_move_start_threshold_position_2D_as_invalid();
                                else
                                    m_mouse.drag.move_start_threshold_position_2D = pos;
                            }
                        }

                        // propagate event through callback
                        if (curr_idxs != m_selection.get_volume_idxs()) {
                            if (m_selection.is_empty())
                                m_gizmos.reset_all_states();
                            else
                                m_gizmos.refresh_on_off_state();

                            m_gizmos.update_data();
                            post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
                            m_dirty = true;
                        }
                    }
                }

                if (!m_hover_volume_idxs.empty()) {
                    if (evt.LeftDown() && m_moving_enabled && m_mouse.drag.move_volume_idx == -1) {
                        // Only accept the initial position, if it is inside the volume bounding box.
                        if (!any_gizmo_active || !evt.CmdDown()) {
                            int volume_idx = get_first_hover_volume_idx();
                            BoundingBoxf3 volume_bbox = m_volumes.volumes[volume_idx]->transformed_bounding_box();
                            volume_bbox.offset(1.0);
                            const Camera& camera = wxGetApp().plater()->get_picking_camera();
                            m_mouse.scene_position = _mouse_to_3d(camera, { 0, 1 }, nullptr, OpenGLManager::s_picking_frame);
                            if (volume_bbox.contains(m_mouse.scene_position) && !m_selection.is_any_connector()) {
                                m_volumes.volumes[volume_idx]->hover = GLVolume::HS_None;
                                // The dragging operation is initiated.
                                m_mouse.drag.move_volume_idx = volume_idx;
                                m_selection.start_dragging();
                                m_mouse.drag.start_position_3D = m_mouse.scene_position;
                                m_sequential_print_clearance_first_displacement = true;
                                m_moving = true;
                            }
                        }
                    }
                }
            }
        }
    }
    else if (evt.Dragging() && evt.LeftIsDown() && m_mouse.drag.move_volume_idx != -1 && m_layers_editing.state == LayersEditing::Unknown) {
        if (m_canvas_type != ECanvasType::CanvasAssembleView && m_gizmos.is_allow_drag_volume()) {
            if (!m_mouse.drag.move_requires_threshold) {
                m_mouse.dragging = true;
                Vec3d cur_pos = m_mouse.drag.start_position_3D;
                // we do not want to translate objects if the user just clicked on an object while pressing shift to remove it from the selection and then drag
                if (m_selection.contains_volume(get_first_hover_volume_idx())) {
                    const Camera& camera = get_active_camera();
                    auto          camera_up_down_rad_limit = abs(asin(camera.get_dir_forward()(2) / 1.0f));
                    if (camera_up_down_rad_limit < PI/20.0f) {
                        // side view -> move selected volumes orthogonally to camera view direction
                        Linef3 ray = mouse_ray(pos);
                        Vec3d dir = ray.unit_vector();
                        // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing throught the starting position
                        // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebric form
                        // in our case plane normal and ray direction are the same (orthogonal view)
                        // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
                        Vec3d inters = ray.a + (m_mouse.drag.start_position_3D - ray.a).dot(dir) / dir.squaredNorm() * dir;
                        // vector from the starting position to the found intersection
                        Vec3d inters_vec = inters - m_mouse.drag.start_position_3D;

                        Vec3d camera_right = camera.get_dir_right();
                        Vec3d camera_up = camera.get_dir_up();

                        // finds projection of the vector along the camera axes
                        double projection_x = inters_vec.dot(camera_right);
                        double projection_z = inters_vec.dot(camera_up);

                        // apply offset
                        cur_pos = m_mouse.drag.start_position_3D + projection_x * camera_right + projection_z * camera_up;
                        cur_pos[2] = m_mouse.drag.start_position_3D(2);
                    }
                    else {
                        // Generic view
                        // Get new position at the same Z of the initial click point.
                        float z0 = 0.0f;
                        float z1 = 1.0f;
                        cur_pos = Linef3(_mouse_to_3d(camera, pos, &z0), _mouse_to_3d(camera, pos, &z1)).intersect_plane(m_mouse.drag.start_position_3D(2));
                    }
                }
                TransformationType trafo_type;
                trafo_type.set_relative();
                m_selection.translate(cur_pos - m_mouse.drag.start_position_3D, trafo_type);
                if (current_printer_technology() == ptFFF && (fff_print()->config().print_sequence == PrintSequence::ByObject))
                    update_sequential_clearance();
                // BBS
                //wxGetApp().obj_manipul()->set_dirty();
                m_dirty = true;
            }
        }
    }
    else if (evt.Dragging() && evt.LeftIsDown() && m_picking_enabled && m_rectangle_selection.is_dragging()) {
        //BBS not in assemble view
        if (m_canvas_type != ECanvasType::CanvasAssembleView) {
            m_rectangle_selection.dragging(pos.cast<double>());
            m_dirty = true;
        }
    }
    else if (evt.Dragging()) {
        m_mouse.dragging = true;

        if (m_layers_editing.state != LayersEditing::Unknown && layer_editing_object_idx != -1) {
            if (m_layers_editing.state == LayersEditing::Editing) {
                _perform_layer_editing_action(&evt);
                m_mouse.position = pos.cast<double>();
            }
        }
        //BBS: variable print speed along Z.
        else if (m_speed_editing.state != SpeedEditing::Unknown && speed_editing_object_idx != -1) {
            if (m_speed_editing.state == SpeedEditing::Editing) {
                _perform_speed_editing_action(&evt);
                m_mouse.position = pos.cast<double>();
            }
        }
        //Orio: variable sparse infill density / wall count along Z.
        else if (m_infill_walls_editing.state != InfillWallsEditing::Unknown && infill_walls_editing_object_idx != -1) {
            if (m_infill_walls_editing.state == InfillWallsEditing::Editing) {
                _perform_infill_walls_editing_action(&evt);
                m_mouse.position = pos.cast<double>();
            }
        }
        // do not process the dragging if the left mouse was set down in another canvas
        else if (evt.LeftIsDown()) {
            // if dragging over blank area with left button, rotate
            if ((any_gizmo_active || m_hover_volume_idxs.empty()) && m_mouse.is_start_position_3D_defined()) {
                const Vec3d rot = (Vec3d(pos.x(), pos.y(), 0.) - m_mouse.drag.start_position_3D) * (PI * TRACKBALLSIZE / 180.);
                if (this->m_canvas_type == ECanvasType::CanvasAssembleView || m_gizmos.is_paint_gizmo()) {
                    //BBS rotate around target
                    Camera& camera = get_active_camera();
                    Vec3d rotate_target = Vec3d::Zero();
                    if (!m_selection.is_empty())
                        rotate_target = m_selection.get_bounding_box().center();
                    else
                        rotate_target = volumes_bounding_box(is_volumes_limit_to_expand_plate()).center();
                    if (m_gizmos.is_paint_gizmo()) {
                        //Orio: the painting tools (supports, seam, fuzzy skin, color) orbit like the camera of the
                        //Prepare and Preview tabs, Z kept vertical on screen, still around the painted object.
                        //A view left tilted or upside down by a free rotation (assembly view, 3D mouse) is leveled first,
                        //along the same line of sight: the target is refreshed on it, so that the view does not jump.
                        if (std::abs(camera.get_dir_right().z()) > EPSILON || camera.get_dir_up().z() < -EPSILON) {
                            const Vec3d target = camera.get_target();
                            camera.look_at(camera.get_position(), target, Vec3d::UnitZ());
                        }
                        camera.rotate_on_sphere_with_target(rot.x(), rot.y(), current_printer_technology() != ptSLA, rotate_target);
                    } else {
                        //BBS do not limit rotate in assemble view
                        camera.rotate_local_with_target(Vec3d(rot.y(), rot.x(), 0.), rotate_target);
                        //camera.rotate_on_sphere_with_target(rot.x(), rot.y(), false, rotate_target);
                    }
                }
                else {
#ifdef SUPPORT_FEEE_CAMERA
                    if (wxGetApp().app_config->get("use_free_camera") == "1")
                        // Virtual track ball (similar to the 3DConnexion mouse).
                        get_active_camera().rotate_local_around_target(Vec3d(rot.y(), rot.x(), 0.));
                    else {
#endif
                        // Forces camera right vector to be parallel to XY plane in case it has been misaligned using the 3D mouse free rotation.
                        // It is cheaper to call this function right away instead of testing wxGetApp().plater()->get_mouse3d_controller().connected(),
                        // which checks an atomics (flushes CPU caches).
                        // See GH issue #3816.
                        Camera& camera = get_active_camera();

                        bool rotate_limit = current_printer_technology() != ptSLA;
                        Vec3d rotate_target = m_selection.get_bounding_box().center();

                        camera.recover_from_free_camera();
                        //BBS modify rotation
                        if (evt.ControlDown() || evt.CmdDown()) {
                            if ((m_rotation_center.x() == 0.f) && (m_rotation_center.y() == 0.f) && (m_rotation_center.z() == 0.f)) {
                                auto canvas_w = float(get_canvas_size().get_width());
                                auto canvas_h = float(get_canvas_size().get_height());
                                Point screen_center(canvas_w/2, canvas_h/2);
                                //camera.rotate_on_sphere_with_target(rot.x(), rot.y(), rotate_limit, wxGetApp().plater()->get_partplate_list().get_bounding_box().center());
                                m_rotation_center = _mouse_to_3d(camera, screen_center);
                                m_rotation_center(2) = 0.f;
                            }
                            camera.rotate_on_sphere_with_target(rot.x(), rot.y(), rotate_limit, m_rotation_center);
                        } else {
                            //BBS rotate with current plate center
                            PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
                            if (plate)
                                camera.rotate_on_sphere_with_target(rot.x(), rot.y(), rotate_limit, plate->get_bounding_box().center());
                            else
                                camera.rotate_on_sphere(rot.x(), rot.y(), rotate_limit);
                        }
#ifdef SUPPORT_FEEE_CAMERA
                    }
#endif
                }

                m_dirty = true;
            }
            if (m_mouse.is_move_threshold_met(pos)) {
                m_mouse.rotating = true;
            }
            m_mouse.drag.start_position_3D = Vec3d((double)pos(0), (double)pos(1), 0.0);
            m_mouse.drag.move_start_threshold_position_2D = pos;
        }
        else if (evt.MiddleIsDown() || evt.RightIsDown()) {
            // If dragging over blank area with right button, pan.
            if (m_mouse.is_start_position_2D_defined()) {
                // get point in model space at Z = 0
                float z = 0.0f;
                Camera& camera = get_active_camera();
                const Vec3d& cur_pos = _mouse_to_3d(camera, pos, &z);
                Vec3d orig = _mouse_to_3d(camera, m_mouse.drag.start_position_2D, &z);
#ifdef SUPPORT_FREE_CAMERA
                if (this->m_canvas_type != ECanvasType::CanvasAssembleView) {
                    if (wxGetApp().app_config->get("use_free_camera") != "1")
                        // Forces camera right vector to be parallel to XY plane in case it has been misaligned using the 3D mouse free rotation.
                        // It is cheaper to call this function right away instead of testing wxGetApp().plater()->get_mouse3d_controller().connected(),
                        // which checks an atomics (flushes CPU caches).
                        // See GH issue #3816.
                        camera.recover_from_free_camera();
                }
#endif

                camera.set_target(camera.get_target() + orig - cur_pos);
                m_dirty = true;
                m_mouse.ignore_right_up = true;
            }
            if (m_mouse.is_move_threshold_met(pos)) {
                m_mouse.panning = true;
            }
            m_mouse.drag.start_position_2D = pos;
            m_mouse.drag.move_start_threshold_position_2D = pos;
        }
    }
    else if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp()) {

        if (evt.LeftUp()) {
            m_selection.stop_dragging();
            m_rotation_center(0) = m_rotation_center(1) = m_rotation_center(2) = 0.f;
        }
        // Orio : Apercu, un clic (sans tourner la vue) sur un objet le choisit : cadre blanc, son oeil garde, ses
        // statistiques dans la legende, l'encart de la vue "Fusion de couche". Un clic sur lui ou dans le vide (Echap
        // aussi) rend la legende au plateau. Objet sous la souris : _render_object_hide_eye.
        if (evt.LeftUp() && m_canvas_type == ECanvasType::CanvasPreview && !m_mouse.dragging &&
            !m_mouse.rotating && !m_mouse.panning && !m_mouse.ignore_left_up) {
            gcode::GCodeViewer &viewer = get_gcode_viewer();
            const int           picked = viewer.get_picked_label();
            if (m_hover_pick_label >= 0 || picked >= 0) {
                viewer.set_picked_label(m_hover_pick_label == picked ? -1 : m_hover_pick_label);
                set_as_dirty();
            }
        }

        if (m_layers_editing.state != LayersEditing::Unknown) {
            m_layers_editing.state = LayersEditing::Unknown;
            _stop_timer();
            m_layers_editing.accept_changes(*this);
        }
        //BBS: variable print speed along Z.
        else if (m_speed_editing.state != SpeedEditing::Unknown) {
            m_speed_editing.state = SpeedEditing::Unknown;
            _stop_timer();
            m_speed_editing.accept_changes(*this);
        }
        //Orio: variable sparse infill density / wall count along Z.
        else if (m_infill_walls_editing.state != InfillWallsEditing::Unknown) {
            m_infill_walls_editing.state = InfillWallsEditing::Unknown;
            _stop_timer();
            m_infill_walls_editing.accept_changes(*this);
        }
        else if (m_mouse.drag.move_volume_idx != -1 && m_mouse.dragging) {
            do_move(L("Move Object"));
            // BBS
            //wxGetApp().obj_manipul()->set_dirty();
            // Let the plater know that the dragging finished, so a delayed refresh
            // of the scene with the background processing data should be performed.
            post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
        } else if (evt.LeftUp() && m_hover_volume_idxs.empty() && m_gizmos.is_gizmo_click_empty_not_exit() &&
                   !m_rectangle_selection.is_dragging()) { // Orio : une sélection au rectangle (Maj) se termine
            // Click on blank and not exit the gizmo tool
        }
        else if (evt.LeftUp() && m_picking_enabled && m_rectangle_selection.is_dragging() && m_layers_editing.state != LayersEditing::Editing && m_speed_editing.state != SpeedEditing::Editing &&
                 m_infill_walls_editing.state != InfillWallsEditing::Editing) {
            //BBS: don't use alt as de-select
            //if (evt.ShiftDown() || evt.AltDown())
            if (evt.ShiftDown())
                _update_selection_from_hover();

            m_rectangle_selection.stop_dragging();
        }
        else if (evt.LeftUp() && !m_mouse.ignore_left_up && !m_mouse.dragging && m_hover_volume_idxs.empty() && m_hover_plate_idxs.empty() && !is_layers_editing_enabled() && !is_speed_editing_enabled() &&
                 !is_infill_walls_editing_enabled()) {
            // deselect and propagate event through callback
            if (!evt.ShiftDown() && (!any_gizmo_active || !evt.CmdDown()) && m_picking_enabled)
                deselect_all();
        }
        //BBS Select plate in this 3D canvas.
        else if (evt.LeftUp() && !m_mouse.rotating && !m_mouse.panning && m_picking_enabled && !m_hover_plate_idxs.empty() && (m_canvas_type == CanvasView3D) && !is_layers_editing_enabled() && !is_speed_editing_enabled() &&
                 !is_infill_walls_editing_enabled())
        {
                int hover_idx = m_hover_plate_idxs.front();
                wxGetApp().plater()->select_plate_by_hover_id(hover_idx);
                //wxGetApp().plater()->get_partplate_list().select_plate_view();
                //deselect all the objects
                if (m_gizmos.get_current_type() != GLGizmosManager::MeshBoolean && m_hover_volume_idxs.empty()) {
                    bool ignore = false;

                    auto mo                 = get_selected_model_object(*this);
                    auto cur_selection_mode = m_selection.get_mode();
                    ignore                  = cur_selection_mode == Selection::Volume && mo && m_gizmos.is_allow_multi_select_parts_or_objects() &&
                        is_mouse_in_screen_convex_hull(Vec2i(evt.GetX(), evt.GetY()), mo->bounding_box());

                    if (!ignore) {
                        deselect_all();
                    }
                }
        }
        else if (evt.RightUp() && !is_layers_editing_enabled() && !is_speed_editing_enabled() && !is_infill_walls_editing_enabled()) {
            m_mouse.position = pos.cast<double>();
            // forces a frame render to ensure that m_hover_volume_idxs is updated even when the user right clicks while
            // the context menu is already shown
            render();
            if (!m_hover_volume_idxs.empty()) {
                // if right clicking on volume, propagate event through callback (shows context menu)
                int volume_idx = get_first_hover_volume_idx();
                if (!m_volumes.volumes[volume_idx]->is_wipe_tower // no context menu for the wipe tower
                    && m_gizmos.get_current_type() != GLGizmosManager::SlaSupports)  // disable context menu when the gizmo is open
                {
                    // forces the selection of the volume
                    /* m_selection.add(volume_idx); // #et_FIXME_if_needed
                     * To avoid extra "Add-Selection" snapshots,
                     * call add() with check_for_already_contained=true
                     * */
                    m_selection.add(volume_idx, true, true);
                    m_gizmos.refresh_on_off_state();
                    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
                    m_gizmos.update_data();
                    // BBS
                    //wxGetApp().obj_manipul()->set_dirty();
                    // forces a frame render to update the view before the context menu is shown
                    render();
                }
            }

            //BBS change plate selection
            if (!m_hover_plate_idxs.empty() && (m_canvas_type == CanvasView3D) && !m_mouse.dragging) {
                int hover_idx = m_hover_plate_idxs.front();
                wxGetApp().plater()->select_plate_by_hover_id(hover_idx, true);
                if (m_hover_volume_idxs.empty())
                    deselect_all();
                render();
            }

            Vec2d logical_pos = pos.cast<double>();
#if ENABLE_RETINA_GL
            const float factor = m_retina_helper->get_scale_factor();
            logical_pos = logical_pos.cwiseQuotient(Vec2d(factor, factor));
#endif // ENABLE_RETINA_GL

            if (!m_mouse.ignore_right_up && m_gizmos.get_current_type() == GLGizmosManager::EType::Undefined) {
                //BBS post right click event
                if (!m_hover_plate_idxs.empty()) {
                    post_event(RBtnPlateEvent(EVT_GLCANVAS_PLATE_RIGHT_CLICK, { logical_pos, m_hover_plate_idxs.front() }));
                }
                else {
                    // do not post the event if the user is panning the scene
                    // or if right click was done over the wipe tower
                    bool post_right_click_event = m_hover_volume_idxs.empty() || !m_volumes.volumes[get_first_hover_volume_idx()]->is_wipe_tower;
                    if (post_right_click_event)
                        post_event(RBtnEvent(EVT_GLCANVAS_RIGHT_CLICK, { logical_pos, m_hover_volume_idxs.empty() }));
                }
            }
        }

        mouse_up_cleanup();
    }
    else if (evt.Moving()) {
        m_mouse.position = pos.cast<double>();

        // updates gizmos overlay
        if (m_selection.is_empty())
            m_gizmos.reset_all_states();

        m_dirty = true;
    }
    else if (evt.MiddleDClick()) {
        // Orio : double-clic molette = recadrer la caméra sur la sélection, sinon sur toute la scène
        // (le G-code dans l'Aperçu), sans changer l'angle de vue (la touche Z, elle, remet aussi la vue de face).
        if (!m_selection.is_empty())
            zoom_to_selection();
        else if (m_canvas_type == ECanvasType::CanvasPreview && get_gcode_viewer().get_paths_bounding_box().defined)
            zoom_to_gcode();
        else if (m_canvas_type != ECanvasType::CanvasPreview && volumes_bounding_box(is_volumes_limit_to_expand_plate()).defined)
            zoom_to_volumes();
        else
            zoom_to_bed();
        m_dirty = true;
    }
    else
        evt.Skip();

            // Detection of doubleclick on text to open emboss edit window
    auto type = m_gizmos.get_current_type();
    if (evt.LeftDClick() && !m_hover_volume_idxs.empty()) {
        bool for_text_or_svg =  type == GLGizmosManager::EType::Undefined || type == GLGizmosManager::EType::Text || type == GLGizmosManager::EType::Svg;
        if (m_canvas_type == ECanvasType::CanvasAssembleView) {
            for_text_or_svg = false;
        }
        for (int hover_volume_id : m_hover_volume_idxs) {
            const GLVolume &hover_gl_volume = *m_volumes.volumes[hover_volume_id];
            int             object_idx      = hover_gl_volume.object_idx();
            if (object_idx < 0 || static_cast<size_t>(object_idx) >= m_model->objects.size())
                continue;
            const ModelObject *hover_object     = m_model->objects[object_idx];
            int                hover_volume_idx = hover_gl_volume.volume_idx();
            if (hover_volume_idx < 0 || static_cast<size_t>(hover_volume_idx) >= hover_object->volumes.size())
                continue;
            const ModelVolume *hover_volume = hover_object->volumes[hover_volume_idx];

            if (for_text_or_svg && hover_volume->is_text()) {
                m_selection.add_volumes(Selection::EMode::Volume, {(unsigned) hover_volume_id});
                if (type == GLGizmosManager::EType::Text)
                    m_gizmos.open_gizmo(GLGizmosManager::EType::Text); // close text
                wxGetApp().obj_list()->update_selections();
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Edit existed text by double click");
                m_gizmos.open_gizmo(GLGizmosManager::EType::Text);
                return;
            }
           /* else if (hover_volume->text_configuration.has_value()) {
                m_selection.add_volumes(Selection::EMode::Volume, {(unsigned) hover_volume_id});
                if (type != GLGizmosManager::EType::Emboss) m_gizmos.open_gizmo(GLGizmosManager::EType::Emboss);
                wxGetApp().obj_list()->update_selections();
                return;
            }*/
            else if (for_text_or_svg && hover_volume->emboss_shape.has_value()) {
                m_selection.add_volumes(Selection::EMode::Volume, {(unsigned) hover_volume_id});
                wxGetApp().obj_list()->update_selections();
                if (m_main_toolbar) {
                    const auto svg_item_name = GLGizmosManager::convert_gizmo_type_to_string(GLGizmosManager::EType::Svg);
                    if (!m_main_toolbar->is_item_pressed(svg_item_name)) {
                        force_main_toolbar_left_action(get_main_toolbar_item_id(svg_item_name));
                    }
                }
                return;
            }
        }

        if (m_hover_volume_idxs.size() == 1 ) {
            const int hover_volume_id = get_first_hover_volume_idx();
            if (hover_volume_id >= 0 && static_cast<size_t>(hover_volume_id) < m_volumes.volumes.size()) {
                const GLVolume &hover_gl_volume = *m_volumes.volumes[hover_volume_id];
                const int       object_idx      = hover_gl_volume.object_idx();
                if (object_idx >= 0 && static_cast<size_t>(object_idx) < m_model->objects.size()) {
                    const ModelObject *hover_object     = m_model->objects[object_idx];
                    const int          hover_volume_idx = hover_gl_volume.volume_idx();
                    if (hover_volume_idx >= 0 && static_cast<size_t>(hover_volume_idx) < hover_object->volumes.size()) {
                        int model_part_count = 0;
                        for (const ModelVolume *volume : hover_object->volumes) {
                            if (m_canvas_type == ECanvasType::CanvasAssembleView && volume->is_model_part()) {
                                ++model_part_count;
                            } else {
                                ++model_part_count;
                            }
                        }
                        if (!(model_part_count == 1 && m_selection.is_single_full_object())) {
                            m_selection.add_volumes(Selection::EMode::Volume, {(unsigned) hover_volume_id});
                            if (model_part_count >= 2) {
                                m_selection.unlock_volume_selection_mode();
                                m_selection.set_volume_selection_mode(Selection::Volume);
                                if (m_canvas_type == ECanvasType::CanvasAssembleView) { m_selection.lock_volume_selection_mode(); }
                            }

                            m_gizmos.refresh_on_off_state();
                            m_gizmos.update_data();
                            post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
                            m_dirty = true;
                            return;
                        }
                    }
                }
            }
        }
    }

    if (m_moving)
        show_sinking_contours();

#ifdef __WXMSW__
	if (on_enter_workaround)
		m_mouse.position = Vec2d(-1., -1.);
#endif /* __WXMSW__ */
}

void GLCanvas3D::on_paint(wxPaintEvent& evt)
{
    if (m_initialized)
        m_dirty = true;
    else
        // Call render directly, so it gets initialized immediately, not from On Idle handler.
        this->render();
}

void GLCanvas3D::on_kill_focus(wxFocusEvent &evt)
{
    ImGui::SetWindowFocus(nullptr);
    render();
    evt.Skip();
}

void GLCanvas3D::force_set_focus() {
    m_canvas->SetFocus();
};

void GLCanvas3D::on_set_focus(wxFocusEvent& evt)
{
    m_tooltip_enabled = false;
    _refresh_if_shown_on_screen();
    m_tooltip_enabled = true;
}

void GLCanvas3D::on_back_slice_begin() {
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.update_default_view_type();
}

Size GLCanvas3D::get_canvas_size() const
{
    int w = 0;
    int h = 0;

    if (m_canvas != nullptr)
        m_canvas->GetSize(&w, &h);

#if ENABLE_RETINA_GL
    const float factor = m_retina_helper->get_scale_factor();
    w *= factor;
    h *= factor;
#else
    const float factor = 1.0f;
#endif

    return Size(w, h, factor);
}

Vec2d GLCanvas3D::get_local_mouse_position() const
{
    if (m_canvas == nullptr)
		return Vec2d::Zero();

    wxPoint mouse_pos = m_canvas->ScreenToClient(wxGetMousePosition());
    const double factor =
#if ENABLE_RETINA_GL
        m_retina_helper->get_scale_factor();
#else
        1.0;
#endif
    return Vec2d(factor * mouse_pos.x, factor * mouse_pos.y);
}

void GLCanvas3D::set_tooltip(const std::string& tooltip)
{
    if (m_canvas != nullptr)
        m_tooltip.set_text(tooltip);
}

void GLCanvas3D::do_move(const std::string &snapshot_type,bool force_volume_move)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(snapshot_type);

    std::set<std::pair<int, int>> done;  // keeps track of modified instances
    bool object_moved = false;

    // BBS: support wipe-tower for multi-plates
    int n_plates = wxGetApp().plater()->get_partplate_list().get_plate_count();
    std::vector<Vec3d> wipe_tower_origins(n_plates, Vec3d::Zero());

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes) {
        int object_idx = v->object_idx();
        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        std::pair<int, int> done_id(object_idx, instance_idx);

        if (0 <= object_idx && object_idx < (int)m_model->objects.size()) {
            done.insert(done_id);

            // Move instances/volumes
            ModelObject* model_object = m_model->objects[object_idx];
            if (model_object != nullptr) {
                if (force_volume_move || selection_mode == Selection::Volume) {
                    auto cur_mv = model_object->volumes[volume_idx];
                    if (cur_mv->get_offset() != v->get_volume_offset()) {
                        cur_mv->set_transformation(v->get_volume_transformation());
                        // BBS: backup
                        Slic3r::save_object_mesh(*model_object);
                    }
                }
                else if (selection_mode == Selection::Instance) {
                    if (m_canvas_type == GLCanvas3D::ECanvasType::CanvasAssembleView) {
                        if ((model_object->instances[instance_idx]->get_assemble_offset() - v->get_instance_offset()).norm() > 1e-2) {
                            model_object->instances[instance_idx]->set_assemble_offset(v->get_instance_offset());
                        }
                    } else {
                        model_object->instances[instance_idx]->set_offset(v->get_instance_offset());
                    }
                }

                object_moved = true;
                model_object->invalidate_bounding_box();
            }
        }
        else if (object_idx >= 1000 && object_idx < 1000 + n_plates) {
            // Move a wipe tower proxy.
            wipe_tower_origins[object_idx - 1000] = v->get_volume_offset();
        }
    }

    //BBS: notify instance updates to part plater list
    m_selection.notify_instance_update(-1, 0);

    // Fixes flying instances
    for (const std::pair<int, int>& i : done) {
        ModelObject* m = m_model->objects[i.first];
        const double shift_z = m->get_instance_min_z(i.second);
        //BBS: don't call translate if the z is zero
        if ((current_printer_technology() == ptSLA || shift_z > SINKING_Z_THRESHOLD) && (shift_z != 0.0f)) {
            const Vec3d shift(0.0, 0.0, -shift_z);
            m_selection.translate(i.first, i.second, shift);
            m->translate_instance(i.second, shift);
            //BBS: notify instance updates to part plater list
            m_selection.notify_instance_update(i.first, i.second);
        }
        wxGetApp().obj_list()->update_info_items(static_cast<size_t>(i.first));
    }
    //BBS: nofity object list to update
    wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();

    if (object_moved)
        post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_MOVED));

    // BBS: support wipe-tower for multi-plates
    for (int plate_id = 0; plate_id < wipe_tower_origins.size(); plate_id++) {
        Vec3d& wipe_tower_origin = wipe_tower_origins[plate_id];
        if (wipe_tower_origin == Vec3d::Zero())
            continue;

        PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
        DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
        Vec3d plate_origin = ppl.get_plate(plate_id)->get_origin();
        ConfigOptionFloat wipe_tower_x(wipe_tower_origin(0) - plate_origin(0));
        ConfigOptionFloat wipe_tower_y(wipe_tower_origin(1) - plate_origin(1));

        ConfigOptionFloats* wipe_tower_x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x", true);
        ConfigOptionFloats* wipe_tower_y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y", true);
        wipe_tower_x_opt->set_at(&wipe_tower_x, plate_id, 0);
        wipe_tower_y_opt->set_at(&wipe_tower_y, plate_id, 0);
    }

    reset_sequential_print_clearance();

    m_dirty = true;
}

void GLCanvas3D::do_rotate(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(snapshot_type);

    // stores current min_z of instances
    std::map<std::pair<int, int>, double> min_zs;
    for (int i = 0; i < static_cast<int>(m_model->objects.size()); ++i) {
        const ModelObject* obj = m_model->objects[i];
        for (int j = 0; j < static_cast<int>(obj->instances.size()); ++j) {
            if (snapshot_type.empty() && m_selection.get_object_idx() == i) {
                // This means we are flattening this object. In that case pretend
                // that it is not sinking (even if it is), so it is placed on bed
                // later on (whatever is sinking will be left sinking).
                min_zs[{ i, j }] = SINKING_Z_THRESHOLD;
            } else
                min_zs[{ i, j }] = obj->instance_bounding_box(j).min.z();

        }
    }

    std::set<std::pair<int, int>> done;  // keeps track of modified instances

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes) {
        int object_idx = v->object_idx();
        if (object_idx < 0 || (int)m_model->objects.size() <= object_idx)
            continue;

        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        done.insert(std::pair<int, int>(object_idx, instance_idx));

        // Rotate instances/volumes.
        ModelObject* model_object = m_model->objects[object_idx];
        if (model_object != nullptr) {
            if (selection_mode == Selection::Instance) {
                if (m_canvas_type == GLCanvas3D::ECanvasType::CanvasAssembleView) {
                    model_object->instances[instance_idx]->set_assemble_from_transform(v->get_instance_transformation().get_matrix());
                } else {
                    model_object->instances[instance_idx]->set_transformation(v->get_instance_transformation());
                }
            }
            else if (selection_mode == Selection::Volume) {
                auto cur_mv = model_object->volumes[volume_idx];
                if (cur_mv->get_rotation() != v->get_volume_rotation()) {
                    cur_mv->set_transformation(v->get_volume_transformation());
                    // BBS: backup
                    Slic3r::save_object_mesh(*model_object);
                }
            }
            model_object->invalidate_bounding_box();
        }
    }

    //BBS: notify instance updates to part plater list
    m_selection.notify_instance_update(-1, -1);
    if (m_canvas_type != CanvasAssembleView) {
        // Fixes sinking/flying instances
        for (const std::pair<int, int> &i : done) {
            ModelObject *m = m_model->objects[i.first];

            // BBS: don't call translate if the z is zero
            const double shift_z = m->get_instance_min_z(i.second);
            // leave sinking instances as sinking
            if ((min_zs.find({i.first, i.second})->second >= SINKING_Z_THRESHOLD || shift_z > SINKING_Z_THRESHOLD) && (shift_z != 0.0f)) {
                const Vec3d shift(0.0, 0.0, -shift_z);
                m_selection.translate(i.first, i.second, shift);
                m->translate_instance(i.second, shift);
                // BBS: notify instance updates to part plater list
                m_selection.notify_instance_update(i.first, i.second);
            }

            wxGetApp().obj_list()->update_info_items(static_cast<size_t>(i.first));
        }
    }
    //BBS: nofity object list to update
    wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();

    if (!done.empty())
        post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_ROTATED));

    m_dirty = true;
}

void GLCanvas3D::do_scale(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(snapshot_type);

    // stores current min_z of instances
    std::map<std::pair<int, int>, double> min_zs;
    if (!snapshot_type.empty()) {
        for (int i = 0; i < static_cast<int>(m_model->objects.size()); ++i) {
            const ModelObject* obj = m_model->objects[i];
            for (int j = 0; j < static_cast<int>(obj->instances.size()); ++j) {
                min_zs[{ i, j }] = obj->instance_bounding_box(j).min.z();
            }
        }
    }

    std::set<std::pair<int, int>> done;  // keeps track of modified instances

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes) {
        int object_idx = v->object_idx();
        if (object_idx < 0 || (int)m_model->objects.size() <= object_idx)
            continue;

        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        done.insert(std::pair<int, int>(object_idx, instance_idx));

        // Rotate instances/volumes
        ModelObject* model_object = m_model->objects[object_idx];
        if (model_object != nullptr) {
            if (selection_mode == Selection::Instance) {
                model_object->instances[instance_idx]->set_transformation(v->get_instance_transformation());
            }
            else if (selection_mode == Selection::Volume) {
                auto cur_mv = model_object->volumes[volume_idx];
                if (cur_mv->get_scaling_factor() != v->get_volume_scaling_factor()) {
                    model_object->instances[instance_idx]->set_transformation(v->get_instance_transformation());
                    cur_mv->set_transformation(v->get_volume_transformation());
                    // BBS: backup
                    Slic3r::save_object_mesh(*model_object);
                }
            }
            model_object->invalidate_bounding_box();
        }
    }

    //BBS: notify instance updates to part plater list
    m_selection.notify_instance_update(-1, -1);

    // Fixes sinking/flying instances
    for (const std::pair<int, int>& i : done) {
        ModelObject* m = m_model->objects[i.first];

        //BBS: don't call translate if the z is zero
        double shift_z = m->get_instance_min_z(i.second);
        // leave sinking instances as sinking
        if ((min_zs.empty() || min_zs.find({ i.first, i.second })->second >= SINKING_Z_THRESHOLD || shift_z > SINKING_Z_THRESHOLD) && (shift_z != 0.0f)) {
            Vec3d shift(0.0, 0.0, -shift_z);
            m_selection.translate(i.first, i.second, shift);
            m->translate_instance(i.second, shift);
            //BBS: notify instance updates to part plater list
            m_selection.notify_instance_update(i.first, i.second);
        }
        wxGetApp().obj_list()->update_info_items(static_cast<size_t>(i.first));
    }
    //BBS: nofity object list to update
    wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();
    //BBS: notify object info update
    wxGetApp().plater()->show_object_info();

    if (!done.empty())
        post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_SCALED));

    m_dirty = true;
}

void GLCanvas3D::do_flatten(const Vec3d& normal, const std::string& snapshot_type)
{
    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(snapshot_type);

    m_selection.flattening_rotate(normal);
    do_rotate(""); // avoid taking another snapshot
}

void GLCanvas3D::do_center()
{
    if (m_model == nullptr)
        return;

    m_selection.center();
}

void GLCanvas3D::do_center_plate(const int plate_idx) {
    if (m_model == nullptr)
        return;

    m_selection.center_plate(plate_idx);
}

void GLCanvas3D::do_mirror(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(snapshot_type);

    // stores current min_z of instances
    std::map<std::pair<int, int>, double> min_zs;
    if (!snapshot_type.empty()) {
        for (int i = 0; i < static_cast<int>(m_model->objects.size()); ++i) {
            const ModelObject* obj = m_model->objects[i];
            for (int j = 0; j < static_cast<int>(obj->instances.size()); ++j) {
                min_zs[{ i, j }] = obj->instance_bounding_box(j).min.z();
            }
        }
    }

    std::set<std::pair<int, int>> done;  // keeps track of modified instances

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes) {
        int object_idx = v->object_idx();
        if (object_idx < 0 || (int)m_model->objects.size() <= object_idx)
            continue;

        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        done.insert(std::pair<int, int>(object_idx, instance_idx));

        // Mirror instances/volumes
        ModelObject* model_object = m_model->objects[object_idx];
        if (model_object != nullptr) {
            if (selection_mode == Selection::Instance)
                model_object->instances[instance_idx]->set_transformation(v->get_instance_transformation());
            else if (selection_mode == Selection::Volume) {
                if (model_object->volumes[volume_idx]->get_transformation() != v->get_volume_transformation()) {
                    model_object->volumes[volume_idx]->set_transformation(v->get_volume_transformation());
                    // BBS: backup
                    Slic3r::save_object_mesh(*model_object);
                }
            }

            model_object->invalidate_bounding_box();
        }
    }

    //BBS: notify instance updates to part plater list
    m_selection.notify_instance_update(-1, -1);

    // Fixes sinking/flying instances
    for (const std::pair<int, int>& i : done) {
        ModelObject* m = m_model->objects[i.first];

        //BBS: don't call translate if the z is zero
        double shift_z = m->get_instance_min_z(i.second);
        // leave sinking instances as sinking
        if ((min_zs.empty() || min_zs.find({ i.first, i.second })->second >= SINKING_Z_THRESHOLD || shift_z > SINKING_Z_THRESHOLD)&&(shift_z != 0.0f)) {
            Vec3d shift(0.0, 0.0, -shift_z);
            m_selection.translate(i.first, i.second, shift);
            m->translate_instance(i.second, shift);
            //BBS: notify instance updates to part plater list
            m_selection.notify_instance_update(i.first, i.second);
        }
        wxGetApp().obj_list()->update_info_items(static_cast<size_t>(i.first));

        //BBS: notify instance updates to part plater list
        PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
        plate_list.notify_instance_update(i.first, i.second);

        //BBS: nofity object list to update
        wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();
    }
    //BBS: nofity object list to update
    wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();

    post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));

    m_dirty = true;
}

void GLCanvas3D::update_gizmos_on_off_state()
{
    set_as_dirty();
    m_gizmos.update_data();
    m_gizmos.refresh_on_off_state();
}

void GLCanvas3D::handle_sidebar_focus_event(const std::string& opt_key, bool focus_on)
{
    m_sidebar_field = focus_on ? opt_key : "";

    //BBS: this event was sent from gizmo now, no need to clear gizmo
    //if (!m_sidebar_field.empty())
    //    m_gizmos.reset_all_states();

    m_dirty = true;
}

void GLCanvas3D::handle_layers_data_focus_event(const t_layer_height_range range, const EditorType type)
{
    //Orio: the tool "Modifiers" edits the height ranges: it stays open and draws them itself.
    if (m_gizmos.get_current_type() == GLGizmosManager::Modifiers)
        return;
    std::string field = "layer_" + std::to_string(type) + "_" + std::to_string(range.first) + "_" + std::to_string(range.second);
    m_gizmos.reset_all_states();
    handle_sidebar_focus_event(field, true);
}

void GLCanvas3D::update_ui_from_settings()
{
    m_dirty = true;

#if __APPLE__
    // Update OpenGL scaling on OSX after the user toggled the "use_retina_opengl" settings in Preferences dialog.
    const float orig_scaling = m_retina_helper->get_scale_factor();

    const bool use_retina = true;
    BOOST_LOG_TRIVIAL(debug) << "GLCanvas3D: Use Retina OpenGL: " << use_retina;
    m_retina_helper->set_use_retina(use_retina);
    const float new_scaling = m_retina_helper->get_scale_factor();

    if (new_scaling != orig_scaling) {
        BOOST_LOG_TRIVIAL(debug) << "GLCanvas3D: Scaling factor: " << new_scaling;

        Camera& camera = get_active_camera();
        camera.set_zoom(camera.get_zoom() * new_scaling / orig_scaling);
        _refresh_if_shown_on_screen();
    }
#endif // ENABLE_RETINA_GL
}

// BBS: add partplate logic
GLCanvas3D::WipeTowerInfo GLCanvas3D::get_wipe_tower_info(int plate_idx) const
{
    WipeTowerInfo wti;

    for (const GLVolume* vol : m_volumes.volumes) {
        if (vol->is_wipe_tower && vol->object_idx() - 1000 == plate_idx) {
            DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
            wti.m_pos = Vec2d(proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x")->get_at(plate_idx),
                              proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y")->get_at(plate_idx));
            // BBS: don't support rotation
            //wti.m_rotation = (M_PI/180.) * proj_cfg->opt_float("wipe_tower_rotation_angle");

            auto& preset = wxGetApp().preset_bundle->prints.get_edited_preset();
            float wt_brim_width = preset.config.opt_float("prime_tower_brim_width");

            const BoundingBoxf3& bb = vol->bounding_box();
            if (wt_brim_width < 0) wt_brim_width = WipeTower::get_auto_brim_by_height((float)bb.max.z());
            wti.m_bb = BoundingBoxf{to_2d(bb.min), to_2d(bb.max)};
            wti.m_bb.offset(wt_brim_width);

            float brim_width = wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_float("prime_tower_brim_width");
            if (brim_width < 0) brim_width = WipeTower::get_auto_brim_by_height((float) bb.max.z());
            wti.m_bb.offset((brim_width));

            // BBS: the wipe tower pos might be outside bed
            PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
            Vec2d plate_size = plate->get_size();
            wti.m_pos.x() = std::clamp(wti.m_pos.x(), 0.0, plate_size(0) - wti.m_bb.size().x());
            wti.m_pos.y() = std::clamp(wti.m_pos.y(), 0.0, plate_size(1) - wti.m_bb.size().y());

            // BBS: add partplate logic
            wti.m_plate_idx = plate_idx;
            break;
        }
    }

    return wti;
}

Linef3 GLCanvas3D::mouse_ray(const Point& mouse_pos)
{
    float z0 = 0.0f;
    float z1 = 1.0f;
    const Camera& camera = get_active_camera();
    return Linef3(_mouse_to_3d(camera, mouse_pos, &z0), _mouse_to_3d(camera, mouse_pos, &z1));
}

double GLCanvas3D::get_size_proportional_to_max_bed_size(double factor) const
{
    const BoundingBoxf& bbox = m_bed.build_volume().bounding_volume2d();
    return factor * std::max(bbox.size()[0], bbox.size()[1]);
}

//BBS
std::vector<Vec2f> GLCanvas3D::get_empty_cells(const Vec2f start_point, const Vec2f step)
{
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    BoundingBoxf3 build_volume = plate->get_build_volume();
    Vec2d vmin(build_volume.min.x(), build_volume.min.y()), vmax(build_volume.max.x(), build_volume.max.y());
    BoundingBoxf bbox(vmin, vmax);
    std::vector<Vec2f> cells;
    auto min_x = start_point.x() - step(0) * int((start_point.x() - bbox.min.x()) / step(0));
    auto min_y = start_point.y() - step(1) * int((start_point.y() - bbox.min.y()) / step(1));
    for (float x = min_x; x < bbox.max.x() - step(0) / 2; x += step(0))
        for (float y = min_y; y < bbox.max.y() - step(1) / 2; y += step(1))
        {
            cells.emplace_back(x, y);
        }
    for (size_t i = 0; i < m_model->objects.size(); ++i) {
        ModelObject* model_object = m_model->objects[i];
        auto id = model_object->id().id;
        ModelInstance* model_instance0 = model_object->instances.front();
        Geometry::Transformation temp(model_instance0->get_transformation());
        temp.set_offset(Vec3d(0.0, 0.0, model_instance0->get_offset().z()));
        Polygon hull_2d = model_object->convex_hull_2d(temp.get_matrix());
        if (hull_2d.empty())
            continue;

        const auto& instances = model_object->instances;
        double rotation_z0 = instances.front()->get_rotation().z();
        for (const auto& instance : instances) {
            Geometry::Transformation transformation;
            const Vec3d& offset = instance->get_offset();
            transformation.set_offset({ scale_(offset.x()), scale_(offset.y()), 0.0 });
            transformation.set_rotation({0, 0, instance->get_rotation().z() - rotation_z0});
            const Transform3d& trafo = transformation.get_matrix();
            Polygon inst_hull_2d = hull_2d.transform(trafo);

            for (auto it = cells.begin(); it != cells.end(); )
            {
                if (inst_hull_2d.contains(Point(scale_(it->x()), scale_(it->y()))))
                    it = cells.erase(it);
                else
                    it++;
            }
        }
    }

    Vec2f start = start_point;
    if (start_point(0) < 0 && start_point(1) < 0) {
        start(0) = bbox.center()(0);
        start(1) = bbox.center()(1);
    }
    std::sort(cells.begin(), cells.end(), [start](const Vec2f& cell1, const Vec2f& cell2) {return (cell1 - start).norm() < (cell2 - start).norm(); });
    return cells;
}

Vec2f GLCanvas3D::get_nearest_empty_cell(const Vec2f start_point, const Vec2f step)
{
    std::vector<Vec2f> empty_cells = get_empty_cells(start_point, step);
    if (!empty_cells.empty())
        return empty_cells.front();
    else {
        double offset = get_size_proportional_to_max_bed_size(0.05);
        return { start_point(0) + offset, start_point(1) + offset };
    }
}

void GLCanvas3D::set_cursor(ECursorType type)
{
    if ((m_canvas != nullptr) && (m_cursor_type != type))
    {
        switch (type)
        {
        case Standard: { m_canvas->SetCursor(*wxSTANDARD_CURSOR); break; }
        case Cross: { m_canvas->SetCursor(*wxCROSS_CURSOR); break; }
        case Hand: { m_canvas->SetCursor(wxCursor(wxCURSOR_HAND));break;}
        }

        m_cursor_type = type;
    }
}

void GLCanvas3D::msw_rescale()
{
}

bool GLCanvas3D::has_toolpaths_to_export() const
{
    const auto& t_gcode_viewer = get_gcode_viewer();
    return t_gcode_viewer.can_export_toolpaths();
}

void GLCanvas3D::export_toolpaths_to_obj(const char* filename) const
{
    auto& t_gcode_viewer = get_gcode_viewer();
    t_gcode_viewer.export_toolpaths_to_obj(filename);
}

void GLCanvas3D::mouse_up_cleanup()
{
    m_moving = false;
    m_mouse.drag.move_volume_idx = -1;
    m_mouse.set_start_position_3D_as_invalid();
    m_mouse.set_start_position_2D_as_invalid();
    m_mouse.dragging = false;
    m_mouse.rotating = false;
    m_mouse.panning = false;
    m_mouse.ignore_left_up = false;
    m_mouse.ignore_right_up = false;
    m_dirty = true;

    if (m_canvas->HasCapture())
        m_canvas->ReleaseMouse();
}

bool GLCanvas3D::can_sequential_clearance_show_in_gizmo() {
    switch (m_gizmos.get_current_type()) {
    case GLGizmosManager::EType::Move:
    case GLGizmosManager::EType::Scale:
    case GLGizmosManager::EType::Rotate: {
        return true;
    }
    }
    return false;
}

void GLCanvas3D::update_sequential_clearance()
{
    if (current_printer_technology() != ptFFF || (fff_print()->config().print_sequence == PrintSequence::ByLayer))
        return;

    if (m_gizmos.is_dragging())
        return;

    // collects instance transformations from volumes
    // first define temporary cache
    unsigned int instances_count = 0;
    std::vector<std::vector<std::optional<Geometry::Transformation>>> instance_transforms;
    for (size_t obj = 0; obj < m_model->objects.size(); ++obj) {
        instance_transforms.emplace_back(std::vector<std::optional<Geometry::Transformation>>());
        const ModelObject* model_object = m_model->objects[obj];
        for (size_t i = 0; i < model_object->instances.size(); ++i) {
            instance_transforms[obj].emplace_back(std::optional<Geometry::Transformation>());
            ++instances_count;
        }
    }

    //if (instances_count == 1)
    //    return;

    // second fill temporary cache with data from volumes
    for (const GLVolume* v : m_volumes.volumes) {
        if (v->is_modifier || v->is_wipe_tower)
            continue;

        auto& transform = instance_transforms[v->object_idx()][v->instance_idx()];
        if (!transform.has_value())
            transform = v->get_instance_transformation();
    }

    // calculates objects 2d hulls (see also: Print::sequential_print_horizontal_clearance_valid())
    // this is done only the first time this method is called while moving the mouse,
    // the results are then cached for following displacements
    if (m_sequential_print_clearance_first_displacement) {
        m_sequential_print_clearance.m_hull_2d_cache.clear();
        bool all_objects_are_short = std::all_of(fff_print()->objects().begin(), fff_print()->objects().end(), \
            [&](PrintObject* obj) { return obj->height() < scale_(fff_print()->config().nozzle_height.value - MARGIN_HEIGHT); });
        float shrink_factor;
        if (all_objects_are_short)
            shrink_factor = scale_(0.5 * MAX_OUTER_NOZZLE_RADIUS - 0.1);
        else
            shrink_factor = static_cast<float>(scale_(0.5 * fff_print()->config().extruder_clearance_max_radius.value - EPSILON));

        double mitter_limit = scale_(0.1);
        m_sequential_print_clearance.m_hull_2d_cache.reserve(m_model->objects.size());
        for (size_t i = 0; i < m_model->objects.size(); ++i) {
            ModelObject* model_object = m_model->objects[i];
            ModelInstance* model_instance0 = model_object->instances.front();
            Geometry::Transformation temp(model_instance0->get_transformation());
            temp.set_offset(Vec3d(0.0, 0.0, model_instance0->get_offset().z()));
            Polygon hull_no_offset = model_object->convex_hull_2d(temp.get_matrix());
            auto tmp = offset(hull_no_offset,
                // Shrink the extruder_clearance_max_radius a tiny bit, so that if the object arrangement algorithm placed the objects
                // exactly by satisfying the extruder_clearance_max_radius, this test will not trigger collision.
                shrink_factor,
                jtRound, mitter_limit);
            Polygon hull_2d = !tmp.empty() ? tmp.front() : hull_no_offset;// tmp may be empty due to clipper's bug, see STUDIO-2452

            Pointf3s& cache_hull_2d = m_sequential_print_clearance.m_hull_2d_cache.emplace_back(Pointf3s());
            cache_hull_2d.reserve(hull_2d.points.size());
            for (const Point& p : hull_2d.points) {
                cache_hull_2d.emplace_back(unscale<double>(p.x()), unscale<double>(p.y()), 0.0);
            }
        }
        m_sequential_print_clearance_first_displacement = false;
    }

    // calculates instances 2d hulls (see also: Print::sequential_print_horizontal_clearance_valid())
    //BBS: add the height logic
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    Polygons polygons;
    std::vector<std::pair<Polygon, float>> height_polygons;
    polygons.reserve(instances_count);
    height_polygons.reserve(instances_count);
    std::vector<struct height_info> convex_and_bounding_boxes;
    struct height_info
    {
        double         instance_height;
        BoundingBox    bounding_box;
        Polygon        hull_polygon;
    };
    for (size_t i = 0; i < instance_transforms.size(); ++i) {
        const auto& instances = instance_transforms[i];
        double rotation_z0 = instances.front()->get_rotation().z();
        int index = 0;
        for (const auto& instance : instances) {
            Geometry::Transformation transformation;
            const Vec3d& offset = instance->get_offset();
            transformation.set_offset({ offset.x(), offset.y(), 0.0 });
            transformation.set_rotation({0, 0, instance->get_rotation().z() - rotation_z0});
            const Transform3d& trafo = transformation.get_matrix();
            const Pointf3s& hull_2d = m_sequential_print_clearance.m_hull_2d_cache[i];
            Points inst_pts;
            inst_pts.reserve(hull_2d.size());
            for (size_t j = 0; j < hull_2d.size(); ++j) {
                const Vec3d p = trafo * hull_2d[j];
                inst_pts.emplace_back(scaled<double>(p.x()), scaled<double>(p.y()));
            }
            Polygon convex_hull(std::move(inst_pts));
            BoundingBox bouding_box = convex_hull.bounding_box();
            BoundingBox plate_bb = plate->get_bounding_box_crd();
            double instance_height = m_model->objects[i]->get_instance_max_z(index++);
            //skip the object for not current plate
            if (!plate_bb.overlap(bouding_box))
                continue;
            convex_and_bounding_boxes.push_back({instance_height, bouding_box, convex_hull});
            polygons.emplace_back(std::move(convex_hull));
        }
    }

    //sort the print instance
    std::sort(convex_and_bounding_boxes.begin(), convex_and_bounding_boxes.end(),
        [](auto &l, auto &r) {
            auto ly1 = l.bounding_box.min.y();
            auto ly2 = l.bounding_box.max.y();
            auto ry1 = r.bounding_box.min.y();
            auto ry2 = r.bounding_box.max.y();
            auto inter_min = std::max(ly1, ry1);
            auto inter_max = std::min(ly2, ry2);
            auto lx = l.bounding_box.min.x();
            auto rx = r.bounding_box.min.x();
            if (inter_max - inter_min > 0)
                return (lx < rx) || ((lx == rx)&&(ly1 < ry1));
            else
                return (ly1 < ry1);
        });

    /*bool has_interlaced_objects = false;
    for (int k = 0; k < bounding_box_count; k++)
    {
        Polygon& convex = convex_and_bounding_boxes[k].hull_polygon;
        BoundingBox& bbox = convex_and_bounding_boxes[k].bounding_box;
        auto iy1 = bbox.min.y();
        auto iy2 = bbox.max.y();

        for (int i = k+1; i < bounding_box_count; i++)
        {
            Polygon&     next_convex = convex_and_bounding_boxes[i].hull_polygon;
            BoundingBox& next_bbox   = convex_and_bounding_boxes[i].bounding_box;
            auto py1 = next_bbox.min.y();
            auto py2 = next_bbox.max.y();
            auto inter_min = std::max(iy1, py1); // min y of intersection
            auto inter_max = std::min(iy2, py2); // max y of intersection. length=max_y-min_y>0 means intersection exists
            if (inter_max - inter_min > 0) {
                has_interlaced_objects = true;
                break;
            }
        }
        if (has_interlaced_objects)
            break;
    }*/

    int bounding_box_count = convex_and_bounding_boxes.size();
    double printable_height = fff_print()->config().printable_height;
    double hc1 = fff_print()->config().extruder_clearance_height_to_lid;
    double hc2 = fff_print()->config().extruder_clearance_height_to_rod;
    for (int k = 0; k < bounding_box_count; k++)
    {
        Polygon& convex = convex_and_bounding_boxes[k].hull_polygon;
        BoundingBox& bbox = convex_and_bounding_boxes[k].bounding_box;
        auto iy1 = bbox.min.y();
        auto iy2 = bbox.max.y();
        double height = (k == (bounding_box_count - 1))?printable_height:hc1;

        /*if (has_interlaced_objects) {
            if ((k < (bounding_box_count - 1)) && (convex_and_bounding_boxes[k].instance_height > hc2)) {
                height_polygons.emplace_back(std::make_pair(convex, hc2));
            }
        }
        else {
            if ((k < (bounding_box_count - 1)) && (convex_and_bounding_boxes[k].instance_height > hc1)) {
                height_polygons.emplace_back(std::make_pair(convex, hc1));
            }
        }*/

        for (int i = k+1; i < bounding_box_count; i++)
        {
            Polygon&     next_convex = convex_and_bounding_boxes[i].hull_polygon;
            BoundingBox& next_bbox   = convex_and_bounding_boxes[i].bounding_box;
            auto py1 = next_bbox.min.y();
            auto py2 = next_bbox.max.y();
            auto inter_min = std::max(iy1, py1); // min y of intersection
            auto inter_max = std::min(iy2, py2); // max y of intersection. length=max_y-min_y>0 means intersection exists
            if (inter_max - inter_min > 0) {
                height = hc2;
                break;
            }
        }
        if (height < convex_and_bounding_boxes[k].instance_height)
            height_polygons.emplace_back(std::make_pair(convex, height));
    }

    // sends instances 2d hulls to be rendered
    set_sequential_print_clearance_visible(true);
    set_sequential_print_clearance_render_fill(false);
    set_sequential_print_clearance_polygons(polygons, height_polygons);
}

bool GLCanvas3D::is_object_sinking(int object_idx) const
{
    for (const GLVolume* v : m_volumes.volumes) {
        if (v->object_idx() == object_idx && (v->is_sinking() || (!v->is_modifier && v->is_below_printbed())))
            return true;
    }
    return false;
}

void GLCanvas3D::apply_retina_scale(Vec2d &screen_coordinate) const
{
#if ENABLE_RETINA_GL
    double scale = static_cast<double>(m_retina_helper->get_scale_factor());
    screen_coordinate *= scale;
#endif // ENABLE_RETINA_GL}
}

bool GLCanvas3D::_is_shown_on_screen() const
{
    return (m_canvas != nullptr) ? m_canvas->IsShownOnScreen() : false;
}

// Getter for the const char*[]
static bool string_getter(const bool is_undo, int idx, const char** out_text)
{
    return wxGetApp().plater()->undo_redo_string_getter(is_undo, idx, out_text);
}

// Getter for the const char*[] for the search list
static bool search_string_getter(int idx, const char** label, const char** tooltip)
{
    return wxGetApp().plater()->search_string_getter(idx, label, tooltip);
}

//BBS: GUI refactor: adjust main toolbar position
bool GLCanvas3D::_render_orient_menu(float left, float right, float bottom, float top)
{
    ImGuiWrapper* imgui = wxGetApp().imgui();

    auto canvas_w = float(get_canvas_size().get_width());
    auto canvas_h = float(get_canvas_size().get_height());
    //BBS: GUI refactor: move main toolbar to the right
    //original use center as {0.0}, and top is (canvas_h/2), bottom is (-canvas_h/2), also plus inv_camera
    //now change to left_up as {0,0}, and top is 0, bottom is canvas_h
#if BBS_TOOLBAR_ON_TOP
    const float x = left * float(get_active_camera().get_zoom()) + 0.5f * canvas_w;
    ImGuiWrapper::push_toolbar_style(get_scale());
    float main_toolbar_height = 0.0f;
    const auto& p_main_toolbar = get_main_toolbar();
    if (p_main_toolbar) {
        main_toolbar_height = p_main_toolbar->get_height();
    }
    imgui->set_next_window_pos(x, main_toolbar_height, ImGuiCond_Always, 0.5f, 0.0f);
#else
    float main_toolbar_width = 0.0f;
    const auto& p_main_toolbar = get_main_toolbar();
    if (p_main_toolbar) {
        main_toolbar_width = p_main_toolbar->get_width();
    }
    const float x = canvas_w - main_toolbar_width;
    const float y = 0.5f * canvas_h - top * float(get_active_camera().get_zoom());
    imgui->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    imgui->begin(_L("Auto Orientation options"), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    OrientSettings settings = get_orient_settings();
    OrientSettings& settings_out = get_orient_settings();

    auto& appcfg = wxGetApp().app_config;
    PrinterTechnology ptech = current_printer_technology();

    bool settings_changed = false;
    float angle_min = 45.f;
    std::string angle_key = "overhang_angle", rot_key = "enable_rotation";
    std::string key_min_area = "min_area";
    std::string postfix = "_fff";

    if (ptech == ptSLA) {
        angle_min = 45.f;
        postfix = "_sla";
    }


    angle_key += postfix;
    rot_key += postfix;

    //if (imgui->slider_float(_L("Overhang Angle"), &settings.overhang_angle, angle_min, 90.0f, "%5.2f") || angle_min > settings.overhang_angle) {
    //    settings.overhang_angle = std::max(angle_min, settings.overhang_angle);
    //    settings_out.overhang_angle = settings.overhang_angle;
    //    appcfg->set("orient", angle_key, std::to_string(settings_out.overhang_angle));
    //    settings_changed = true;
    //}

    if (imgui->checkbox(_L("Enable rotation"), settings.enable_rotation)) {
        settings_out.enable_rotation = settings.enable_rotation;
        appcfg->set("orient", rot_key, settings_out.enable_rotation ? "1" : "0");
        settings_changed = true;
    }

    if (imgui->checkbox(_L("Optimize support interface area"), settings.min_area)) {
        settings_out.min_area = settings.min_area;
        appcfg->set("orient", key_min_area, settings_out.min_area ? "1" : "0");
        settings_changed = true;
    }

    ImGui::Separator();

    if (imgui->button(_L("Orient"))) {
        wxGetApp().plater()->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
        wxGetApp().plater()->orient();
    }

    ImGui::SameLine();

    if (imgui->button(_L("Reset"))) {
        settings_out = OrientSettings{};
        settings_out.overhang_angle = 60.f;
        appcfg->set("orient", angle_key, std::to_string(settings_out.overhang_angle));
        appcfg->set("orient", rot_key, settings_out.enable_rotation ? "1" : "0");
        appcfg->set("orient", key_min_area, settings_out.min_area? "1" : "0");
        settings_changed = true;
    }

    imgui->end();
    ImGuiWrapper::pop_toolbar_style();
    return settings_changed;
}

//Orio: combo box of the arrange menu, label on the left. Returns true if the selection changed.
static bool arrange_menu_combo(ImGuiWrapper *imgui, const wxString &label, const char *id, const std::vector<wxString> &options,
                               int &selection, float combo_left, float combo_width, const wxString &tooltip, wxString &hovered_tooltip)
{
    ImGui::AlignTextToFramePadding();
    imgui->text(label);
    // Shown by the caller after pop_combo_style(): under the combo style, a tooltip takes the popup background
    // (white in the light theme) while ImGuiWrapper::tooltip() writes in white.
    if (!tooltip.empty() && ImGui::IsItemHovered())
        hovered_tooltip = tooltip;
    ImGui::SameLine(combo_left);
    ImGui::PushItemWidth(combo_width);

    int               selection_out = selection;
    const std::string preview       = selection >= 0 && selection < int(options.size()) ? into_u8(options[selection]) : std::string();
    if (ImGui::BBLBeginCombo(id, preview.c_str(), 0)) {
        for (int i = 0; i < int(options.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(into_u8(options[i]).c_str(), i == selection))
                selection_out = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    const bool changed = selection_out != selection;
    selection          = selection_out;
    return changed;
}

//BBS: GUI refactor: adjust main toolbar position
bool GLCanvas3D::_render_arrange_menu(float left, float toolbar_height)
{
    ImGuiWrapper *imgui = wxGetApp().imgui();

    auto canvas_w = float(get_canvas_size().get_width());
    auto canvas_h = float(get_canvas_size().get_height());
    //BBS: GUI refactor: move main toolbar to the right
    //original use center as {0.0}, and top is (canvas_h/2), bottom is (-canvas_h/2), also plus inv_camera
    //now change to left_up as {0,0}, and top is 0, bottom is canvas_h
#if BBS_TOOLBAR_ON_TOP
    float zoom = (float)get_active_camera().get_zoom();
    const float x = 0.5 * canvas_w + left * zoom;
    imgui->set_next_window_pos(x, toolbar_height, ImGuiCond_Always, 0.0f, 0.0f);

#else
    float main_toolbar_width = 0.0f;
    const auto& p_main_toolbar = get_main_toolbar();
    if (p_main_toolbar) {
        main_toolbar_width = p_main_toolbar->get_width();
    }
    const float x = canvas_w - main_toolbar_width;
    const float y = 0.5f * canvas_h - top * float(get_active_camera().get_zoom());
    imgui->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    //BBS
    ImGuiWrapper::push_toolbar_style(get_scale());

    // Orio: floating, collapsible window (ImGuiWrapper::begin_tool_window), like the windows of the tools.
    imgui->begin_tool_window("toolbar_arrange", _L("Arrange options"), m_is_dark ? "toolbar_arrange_dark.svg" : "toolbar_arrange.svg",
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ArrangeSettings settings = get_arrange_settings();
    ArrangeSettings &settings_out = get_arrange_settings();
    const float slider_icon_width = imgui->get_slider_icon_size().x;
    //Orio: the sliders start after the longest label ("Spacing from bed" is shown when printing by layer).
    float cursor_slider_left = imgui->calc_text_size(_L("Spacing")).x + imgui->scaled(1.5f);
    if (!settings.is_seq_print)
        cursor_slider_left = std::max(cursor_slider_left, imgui->calc_text_size(_L("Spacing from bed")).x + imgui->scaled(1.5f));
    const float minimal_slider_width = imgui->scaled(4.f);
    float window_width  = minimal_slider_width + 2 * slider_icon_width;
    auto &appcfg = wxGetApp().app_config;
    PrinterTechnology ptech = current_printer_technology();

    bool settings_changed = false;
    float dist_min = 0.f;  // 0 means auto
    std::string dist_key = "min_object_distance", rot_key = "enable_rotation";
    std::string bed_shrink_x_key = "bed_shrink_x", bed_shrink_y_key = "bed_shrink_y";
    std::string multi_material_key = "allow_multi_materials_on_same_plate";
    std::string avoid_extrusion_key = "avoid_extrusion_cali_region";
    std::string align_to_y_axis_key = arrange_align_to_y_axis_key(); //Orio: saved per printer structure
    std::string save_svg_key        = "save_svg";
    std::string postfix             = settings.postfix;
    //BBS:
    bool seq_print = settings.is_seq_print;

    dist_key += postfix;
    rot_key  += postfix;
    bed_shrink_x_key += postfix;
    bed_shrink_y_key += postfix;

    ImGui::AlignTextToFramePadding();
    imgui->text(_L("Spacing"));
    ImGui::SameLine(1.2 * cursor_slider_left);
    ImGui::PushItemWidth(window_width - slider_icon_width);
    bool b_Spacing = imgui->bbl_slider_float_style("##Spacing", &settings.distance, dist_min, 100.0f, "%5.2f") || dist_min > settings.distance;
    ImGui::SameLine(window_width - slider_icon_width + 1.3 * cursor_slider_left);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    bool b_spacing_input = ImGui::BBLDragFloat("##spacing_input", &settings.distance, 0.05f, 0.0f, 0.0f, "%.2f");
    if (b_Spacing || b_spacing_input)
    {
        settings.distance = std::max(dist_min, settings.distance);
        settings_out.distance = settings.distance;
        appcfg->set("arrange", dist_key.c_str(), float_to_string_decimal_point(settings_out.distance));
        settings_changed = true;
    }
    imgui->text(_L("0 means auto spacing."));

    //Orio: options of the arrange engine of PrusaSlicer, used when printing by layer (Jobs/ArrangePrusa.hpp).
    ArrangePrusaSettings prusa_settings = ArrangePrusaSettings::load(*appcfg, postfix);
    bool                 prusa_changed  = false;
    if (!seq_print) {
        ImGui::AlignTextToFramePadding();
        imgui->text(_L("Spacing from bed"));
        if (ImGui::IsItemHovered())
            imgui->tooltip(_L("Minimum distance between the objects and the edges of the plate."), imgui->scaled(20.f));
        ImGui::SameLine(1.2 * cursor_slider_left);
        ImGui::PushItemWidth(window_width - slider_icon_width);
        bool b_bed_distance = imgui->bbl_slider_float_style("##SpacingFromBed", &prusa_settings.bed_distance, 0.f, 100.0f, "%5.2f");
        ImGui::PopItemWidth();
        ImGui::SameLine(window_width - slider_icon_width + 1.3 * cursor_slider_left);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_bed_distance_input = ImGui::BBLDragFloat("##spacing_from_bed_input", &prusa_settings.bed_distance, 0.05f, 0.0f, 0.0f, "%.2f");
        ImGui::PopItemWidth();
        if (b_bed_distance || b_bed_distance_input) {
            prusa_settings.bed_distance = std::max(0.f, prusa_settings.bed_distance);
            prusa_changed               = true;
        }
    }

    ImGui::Separator();
#if !BBL_RELEASE_TO_PUBLIC
    if (imgui->bbl_checkbox(_L("Save SVG"), settings.save_svg)) {
        settings_out.save_svg = settings.save_svg;
        appcfg->set("arrange", save_svg_key.c_str(), settings_out.save_svg ? "1" : "0");
        settings_changed = true;
    }
#endif
    //Orio: the options are saved as booleans. With `value ? "1" : "0"`, AppConfig::set() took its bool
    // overload (a string literal converts to true), so every box was saved as checked. The options without
    // a mode suffix are then reloaded into all the modes (load_shared_arrange_settings()).
    if (imgui->bbl_checkbox(_L("Auto rotate for arrangement"), settings.enable_rotation)) {
        settings_out.enable_rotation = settings.enable_rotation;
        appcfg->set("arrange", rot_key, settings_out.enable_rotation);
        load_shared_arrange_settings(); // without auto rotation, "Align to Y axis" gets its saved value back
        settings_changed = true;
    }

    if (imgui->bbl_checkbox(_L("Allow multiple materials on same plate"), settings.allow_multi_materials_on_same_plate)) {
        settings_out.allow_multi_materials_on_same_plate = settings.allow_multi_materials_on_same_plate;
        appcfg->set("arrange", multi_material_key, settings_out.allow_multi_materials_on_same_plate);
        load_shared_arrange_settings();
        settings_changed = true;
    }

    // only show this option if the printer has micro Lidar and can do first layer scan
    DynamicPrintConfig &current_config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    auto                op             = current_config.option("scan_first_layer");
    if (op && op->getBool()) {
        if (imgui->bbl_checkbox(_L("Avoid extrusion calibration region"), settings.avoid_extrusion_cali_region)) {
            settings_out.avoid_extrusion_cali_region = settings.avoid_extrusion_cali_region;
            appcfg->set("arrange", avoid_extrusion_key, settings_out.avoid_extrusion_cali_region);
            load_shared_arrange_settings();
            settings_changed = true;
        }
    } else {
        settings_out.avoid_extrusion_cali_region = false;
    }

    // Align to Y axis. Only enable this option when auto rotation not enabled
    {
        if (settings_out.enable_rotation) {  // do not allow align to Y axis if rotation is enabled
            imgui->disabled_begin(true);
            settings_out.align_to_y_axis = false;
        }

        if (imgui->bbl_checkbox(_L("Align to Y axis"), settings.align_to_y_axis)) {
            settings_out.align_to_y_axis = settings.align_to_y_axis;
            appcfg->set("arrange", align_to_y_axis_key, settings_out.align_to_y_axis);
            load_shared_arrange_settings();
            settings_changed = true;
        }

        if (settings_out.enable_rotation == true) { imgui->disabled_end(); }
    }

    //Orio: alignment of the pile and geometry handling, as in PrusaSlicer (the alignment, offered there only
    // for the XL, is offered for every printer).
    if (!seq_print) {
        const std::vector<wxString> alignments = {_L("Center"), _L("Rear left"), _L("Front left"), _L("Front right"), _L("Rear right"), _L("Random")};
        const std::vector<wxString> geometries = {_L("Fast"), _L("Balanced"), _L("Accurate")};

        // ImGui::SameLine() takes the position from the left side of the window, padding included.
        const float combo_left = ImGui::GetStyle().WindowPadding.x + imgui->scaled(1.f) +
                                 std::max(imgui->calc_text_size(_L("Alignment")).x, imgui->calc_text_size(_L("Geometry handling")).x);
        float       text_width = 0.f;
        for (const std::vector<wxString> *options : {&alignments, &geometries})
            for (const wxString &option : *options)
                text_width = std::max(text_width, imgui->calc_text_size(option).x);
        // BBLBeginCombo() draws a frame narrower than the item width by two arrow buttons (one line height each), then the
        // arrow inside it, on the left of the text: three line heights, plus the padding around the text.
        const float combo_width = text_width + 3.f * ImGui::GetFrameHeight() + 2.f * ImGui::GetStyle().FramePadding.x + imgui->scaled(0.5f);

        wxString hovered_tooltip;
        ImGuiWrapper::push_combo_style(get_scale());
        if (arrange_menu_combo(imgui, _L("Alignment"), "##arrange_alignment", alignments, prusa_settings.alignment, combo_left, combo_width,
                               _L("Where the arranged objects are gathered on the plate."), hovered_tooltip))
            prusa_changed = true;
        if (arrange_menu_combo(imgui, _L("Geometry handling"), "##arrange_geometry", geometries, prusa_settings.geometry, combo_left, combo_width,
                               _L("Fast: the objects are seen as their convex hull.\n"
                                  "Balanced: an object can go into the hollows of the objects already placed.\n"
                                  "Accurate: the real outlines of all the objects are used (slower)."), hovered_tooltip))
            prusa_changed = true;
        ImGuiWrapper::pop_combo_style();
        if (!hovered_tooltip.empty())
            imgui->tooltip(hovered_tooltip, imgui->scaled(20.f));
    } else {
        imgui->text_wrapped(_L("Printing by object: the placement follows the clearance of the extruder, without alignment nor geometry handling."),
                            imgui->scaled(20.f));
    }

    if (prusa_changed) {
        prusa_settings.save(*appcfg, postfix);
        settings_changed = true;
    }

    ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(15.0f, 10.0f));
    if (imgui->button(_L("Arrange"))) {
        wxGetApp().plater()->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
        wxGetApp().plater()->arrange();
    }
    if (ImGui::IsItemHovered())
        imgui->tooltip(_L("Arranges the objects of all the plates (A)."), imgui->scaled(20.f));

    ImGui::SameLine();

    //Orio: arranges only the objects of the current plate, as Shift+A (ArrangeJob::prepare_partplate()).
    if (imgui->button(_L("Arrange this plate"))) {
        wxGetApp().plater()->set_prepare_state(Job::PREPARE_STATE_MENU);
        wxGetApp().plater()->arrange();
    }
    if (ImGui::IsItemHovered())
        imgui->tooltip(_L("Arranges only the objects of the current plate (Shift+A)."), imgui->scaled(20.f));

    ImGui::SameLine();

    if (imgui->button(_L("Reset"))) {
        settings_out.reset();
        //BBS: add specific arrange settings
        if (seq_print) settings_out.is_seq_print = true;

        settings_out.align_to_y_axis = arrange_align_to_y_axis_default();

        //Orio: save all the reset options, so that they are not restored at the next start.
        appcfg->set("arrange", dist_key, float_to_string_decimal_point(settings_out.distance));
        appcfg->set("arrange", rot_key, settings_out.enable_rotation);
        appcfg->set("arrange", multi_material_key, settings_out.allow_multi_materials_on_same_plate);
        appcfg->set("arrange", avoid_extrusion_key, settings_out.avoid_extrusion_cali_region);
        appcfg->set("arrange", align_to_y_axis_key, settings_out.align_to_y_axis);
        load_shared_arrange_settings();
        ArrangePrusaSettings().save(*appcfg, postfix); // spacing from bed, alignment, geometry handling
        settings_changed = true;
    }
    ImGui::PopStyleVar(1);
    imgui->end_tool_window();

    //BBS
    ImGuiWrapper::pop_toolbar_style();

    return settings_changed;
}

static float       identityMatrix[16]   = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
static const float cameraProjection[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
void GLCanvas3D::_render_3d_navigator()
{
    if (!wxGetApp().show_3d_navigator()) {
        return;
    }
    if (!can_show_3d_navigator()) {
        return;
    }
    ImGuizmo::BeginFrame();
    ImGuizmo::AllowAxisFlip(false);

    auto &style                                = ImGuizmo::GetStyle();
    style.Colors[ImGuizmo::COLOR::DIRECTION_X] = ImGuiWrapper::to_ImVec4(ColorRGBA::Y());
    style.Colors[ImGuizmo::COLOR::DIRECTION_Y] = ImGuiWrapper::to_ImVec4(ColorRGBA::Z());
    style.Colors[ImGuizmo::COLOR::DIRECTION_Z] = ImGuiWrapper::to_ImVec4(ColorRGBA::X());
    style.Colors[ImGuizmo::COLOR::TEXT]        = m_is_dark ? ImVec4(224 / 255.f, 224 / 255.f, 224 / 255.f, 1.f) : ImVec4(.2f, .2f, .2f, 1.0f);
    style.Colors[ImGuizmo::COLOR::FACE]        = m_is_dark ? ImVec4(0.23f, 0.23f, 0.23f, 1.f) : ImVec4(0.77f, 0.77f, 0.77f, 1);
    strcpy(style.AxisLabels[ImGuizmo::Axis::Axis_X], "y");
    strcpy(style.AxisLabels[ImGuizmo::Axis::Axis_Y], "z");
    strcpy(style.AxisLabels[ImGuizmo::Axis::Axis_Z], "x");
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_FRONT], _utf8("Front").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_BACK], _utf8("Back").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_TOP], _utf8("Top").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_BOTTOM], _utf8("Bottom").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_LEFT], _CTX_utf8(L_CONTEXT("Left", "Camera"), "Camera").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_RIGHT], _CTX_utf8(L_CONTEXT("Right", "Camera"), "Camera").c_str());

    float sc = get_scale();
#ifdef WIN32
    const int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
    sc *= (float) dpi / (float) DPI_DEFAULT;
#endif // WIN32

    const ImGuiIO &io                 = ImGui::GetIO();
    const float    viewManipulateLeft = 0;
    const float    viewManipulateTop  = io.DisplaySize.y;
    const float    camDistance        = 8.f;
    ImGuizmo::SetID(0);

    Camera &    camera           = get_active_camera();
    Transform3d m                = Transform3d::Identity();
    m.matrix().block(0, 0, 3, 3) = camera.get_view_rotation().toRotationMatrix();
    // Rotate along X and Z axis for 90 degrees to have Y-up
    const auto coord_mapping_transform = Geometry::rotation_transform(Vec3d(0.5 * PI, 0, 0.5 * PI));
    m                                  = m * coord_mapping_transform;
    float cameraView[16];
    for (unsigned int c = 0; c < 4; ++c) {
        for (unsigned int r = 0; r < 4; ++r) { cameraView[c * 4 + r] = m(r, c); }
    }

    const float size  = 128 * sc;
    m_fit_camrea_button_pos[0] = size - 10;
    m_sc                       = sc;
    const bool  dirty = ImGuizmo::ViewManipulate(cameraView, cameraProjection, ImGuizmo::OPERATION::ROTATE, ImGuizmo::MODE::WORLD, identityMatrix, camDistance,
                                                ImVec2(viewManipulateLeft, viewManipulateTop - size), ImVec2(size, size), 0x00101010);

    if (dirty) {
        for (unsigned int c = 0; c < 4; ++c) {
            for (unsigned int r = 0; r < 4; ++r) { m(r, c) = cameraView[c * 4 + r]; }
        }
        // Rotate back
        m = m * (coord_mapping_transform.inverse());
        camera.set_rotation(m);

        request_extra_frame();
    }

    _render_fit_camera_toolbar();
}

bool GLCanvas3D::can_show_3d_navigator() {
    if (m_canvas_type == ECanvasType::CanvasPreview) {
        return m_can_show_navigator;
    }
    return true;
}
#define ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT 0
#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
static void debug_output_thumbnail(const ThumbnailData& thumbnail_data)
{
    // debug export of generated image
    wxImage image(thumbnail_data.width, thumbnail_data.height);
    image.InitAlpha();

    for (unsigned int r = 0; r < thumbnail_data.height; ++r)
    {
        unsigned int rr = (thumbnail_data.height - 1 - r) * thumbnail_data.width;
        for (unsigned int c = 0; c < thumbnail_data.width; ++c)
        {
            unsigned char* px = (unsigned char*)thumbnail_data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }

    image.SaveFile("test.png", wxBITMAP_TYPE_PNG);
}
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT

void GLCanvas3D::render_thumbnail_framebuffer(const std::shared_ptr<OpenGLManager> &  p_ogl_manager,
                                              ThumbnailData &                         thumbnail_data,
                                              unsigned int                            w,
                                              unsigned int                            h,
                                              const ThumbnailsParams &                thumbnail_params,
                                              PartPlateList &                         partplate_list,
                                              ModelObjectPtrs &                       model_objects,
                                              const GLVolumeCollection &              volumes,
                                              std::vector<std::array<float, 4>> &     extruder_colors,
                                              const std::shared_ptr<GLShaderProgram> &shader,
                                              Camera::EType                           camera_type,
                                              Camera::ViewAngleType                   camera_view_angle_type,
                                              bool                                    for_picking,
                                              bool                                    ban_light,
                                              const ExtraThumbData&                   extra_thumb_data)
{
    thumbnail_data.set(w, h);
    if (!thumbnail_data.is_valid())
        return;
    if (!p_ogl_manager) {
        return;
    }

    std::string thumbnail_fb_name{ "" };
    const auto fb_type = Slic3r::GUI::OpenGLManager::get_framebuffers_type();
    BOOST_LOG_TRIVIAL(info) << boost::format("framebuffer_type: %1%") % Slic3r::GUI::OpenGLManager::framebuffer_type_to_string(fb_type).c_str();
    EMSAAType msaa_type = EMSAAType::Disabled;
    const bool b_need_msaa = !for_picking;
    uint32_t samples = 0;
    if (fb_type != OpenGLManager::EFramebufferType::Unknown) {
        thumbnail_fb_name = "thumbnail_fb";
        if (b_need_msaa) {
            GLint max_samples;
            glsafe(::glGetIntegerv(GL_MAX_SAMPLES, &max_samples));
            if (max_samples >= 4) {
                msaa_type = EMSAAType::X4;
                samples = 4;
                BOOST_LOG_TRIVIAL(info) << boost::format("framebuffer msaa type: %1%") % "X4";
            }
            else if (max_samples == 2) {
                msaa_type = EMSAAType::X2;
                samples = 2;
                BOOST_LOG_TRIVIAL(info) << boost::format("framebuffer msaa type: %1%") % "X2";
            }
            else {
                BOOST_LOG_TRIVIAL(info) << boost::format("framebuffer msaa type: %1%") % "Disabled";
            }
        }
    }
    else if (p_ogl_manager->is_legacy_framebuffer_enabled()) {
        thumbnail_fb_name = OpenGLManager::s_back_frame;
    }

    if (thumbnail_fb_name.empty()) {
        return;
    }

    {
        OpenGLManager::FrameBufferModifier thumbnail_fb(*p_ogl_manager, thumbnail_fb_name, msaa_type);
        thumbnail_fb.set_width(w);
        thumbnail_fb.set_height(h);
    }
    if (extra_thumb_data.render_type != ThumbnailRenderRype::GLVolumes) {
        _render_custom_thumbnail_internal(thumbnail_data, thumbnail_params, partplate_list, model_objects, volumes, extruder_colors, shader, camera_type, camera_view_angle_type,
                                          for_picking, ban_light, extra_thumb_data);
    } else if (extra_thumb_data.canvas_type == ECanvasType::CanvasAssembleView) {
        _render_assembly_thumbnail_internal(thumbnail_data, thumbnail_params, partplate_list, model_objects, volumes, extruder_colors, shader, camera_type, camera_view_angle_type,
                                            for_picking, ban_light, extra_thumb_data);
    } else {
        _render_thumbnail_internal(thumbnail_data, thumbnail_params, partplate_list, model_objects, volumes, extruder_colors, shader, camera_type, camera_view_angle_type,
                                   for_picking, ban_light, extra_thumb_data);
    }

    std::string write_to_framebuffer_name = thumbnail_fb_name;
    if (thumbnail_params.post_processing_enabled) {
         // fxaa pass
        write_to_framebuffer_name = "thumbnail_fb_aa";
        _rebuild_postprocessing_pipeline(p_ogl_manager, thumbnail_fb_name, write_to_framebuffer_name, w, h);
         // end fxaa pass
    }

    p_ogl_manager->read_pixel(write_to_framebuffer_name, 0, 0, w, h, EPixelFormat::RGBA, EPixelDataType::UByte, (void*)thumbnail_data.pixels.data());

    const auto& p_fb = p_ogl_manager->get_frame_buffer(thumbnail_fb_name);
    int fb_id = 0;
    if (p_fb) {
        fb_id = p_fb->get_gl_id();
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail prepare: w %1%, h %2%, samples  %3%, render_fbo %4%") % w % h % samples % fb_id;

#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
    debug_output_thumbnail(thumbnail_data);
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT

    BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail prepare: finished");
}

//BBS: GUI refractor

void GLCanvas3D::_update_slice_error_status()
{
    _set_warning_notification_if_needed(EWarning::ToolHeightOutside);
    _set_warning_notification_if_needed(EWarning::ToolpathOutside);
    _set_warning_notification_if_needed(EWarning::GCodeConflict);
    _set_warning_notification_if_needed(EWarning::MultiExtruderPrintableError);
    _set_warning_notification_if_needed(EWarning::MultiExtruderHeightOutside);
    _set_warning_notification_if_needed(EWarning::FilamentUnPrintableOnFirstLayer);
}

void GLCanvas3D::_switch_toolbars_icon_filename()
{
    BackgroundTexture::Metadata background_data;
    background_data.filename = m_is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    const auto& p_main_toolbar = get_main_toolbar();
    if (p_main_toolbar) {
        p_main_toolbar->init(background_data);
        p_main_toolbar->set_dark_mode_enabled(m_is_dark);
    }

    wxGetApp().plater()->get_collapse_toolbar().init(background_data);

}
bool GLCanvas3D::_init_toolbars()
{
    if (!_init_main_toolbar())
        return false;

    if (!_init_return_toolbar())
        return false;

    if (!_init_select_plate_toolbar())
        return false;

    if (!_init_assembly_view_thumbnail())
        return false;

#if 0
    if (!_init_view_toolbar())
        return false;
#endif

    if (!_init_collapse_toolbar())
        return false;

    return true;
}

//BBS: GUI refactor: GLToolbar
bool GLCanvas3D::_init_main_toolbar()
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return false;
    }
    if (!p_main_toolbar->is_enabled())
        return true;

    if (m_canvas_type == ECanvasType::CanvasAssembleView) {
        m_main_toolbar->set_position_mode(ToolbarLayout::EPositionMode::Custom);
    }
    else {
        m_main_toolbar->set_position_mode(ToolbarLayout::EPositionMode::TopMiddle);
    }
    m_main_toolbar->set_collapsed_offset(8); // by design from UX

    BackgroundTexture::Metadata background_data;
    background_data.filename = m_is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (!p_main_toolbar->init(background_data))
    {
        // unable to init the toolbar texture, disable it
        p_main_toolbar->set_enabled(false);
        return true;
    }

    p_main_toolbar->set_dark_mode_enabled(m_is_dark);

    // init arrow
    BackgroundTexture::Metadata arrow_data;
    arrow_data.filename = "toolbar_arrow.svg";
    arrow_data.left = 0;
    arrow_data.top = 0;
    arrow_data.right = 0;
    arrow_data.bottom = 0;
    if (!p_main_toolbar->init_arrow(arrow_data))
    {
        BOOST_LOG_TRIVIAL(error) << "Main toolbar failed to load arrow texture.";
    }

    p_main_toolbar->set_layout_type(ToolbarLayout::EType::Horizontal);
    //BBS: main toolbar is at the top and left, we don't need the rounded-corner effect at the right side and the top side
    p_main_toolbar->set_horizontal_orientation(ToolbarLayout::HO_Right);
    p_main_toolbar->set_vertical_orientation(ToolbarLayout::VO_Top);
    p_main_toolbar->set_border(5.0f);
    p_main_toolbar->set_separator_size(5);
    p_main_toolbar->set_gap_size(4);

    p_main_toolbar->del_all_item();

    uint8_t sprite_id = 0;
    GLToolbarItem::Data item;

    if (m_canvas_type == ECanvasType::CanvasView3D) {
        item.name = "add";
        item.icon_filename_callback = [](bool is_dark_mode)->std::string {
            return is_dark_mode ? "toolbar_open_dark.svg" : "toolbar_open.svg";
            };
        item.tooltip = _utf8(L("Add")) + " [" + GUI::shortkey_ctrl_prefix() + "I]";
        item.sprite_id = sprite_id++;
        item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_ADD)); };
        item.enabling_callback = []()->bool {return wxGetApp().plater()->can_add_model(); };
        if (!p_main_toolbar->add_item(item))
            return false;

        item.name = "addplate";
        item.icon_filename_callback = [](bool is_dark_mode)->std::string {
            return is_dark_mode ? "toolbar_add_plate_dark.svg" : "toolbar_add_plate.svg";
            };
        item.tooltip = _utf8(L("Add plate"));
        item.sprite_id = sprite_id++;
        item.continuous_click = true;
        item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_ADD_PLATE)); };
        item.enabling_callback = []()->bool {return wxGetApp().plater()->can_add_plate(); };
        if (!p_main_toolbar->add_item(item))
            return false;

        item.name = "orient";
        item.icon_filename_callback = [](bool is_dark_mode)->std::string {
            return is_dark_mode ? "toolbar_orient_dark.svg" : "toolbar_orient.svg";
            };
        item.tooltip = _utf8(L("Auto orient"));
        item.sprite_id = sprite_id++;
        item.left.render_callback = nullptr;
        item.enabling_callback = []()->bool { return wxGetApp().plater()->can_do_ui_job(); };
        item.left.toggable = false;  // allow right mouse click
        //BBS: GUI refactor: adjust the main toolbar position
        item.left.action_callback = [this]() {
            if (m_canvas != nullptr)
            {
                wxGetApp().plater()->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
                wxGetApp().plater()->orient();
                //BBS do not show orient menu
                //_render_orient_menu(left, right, bottom, top);
                NetworkAgent* agent = GUI::wxGetApp().getAgent();
                if (agent) agent->track_update_property("auto_orient", std::to_string(++auto_orient_count));
            }
            };
        if (!p_main_toolbar->add_item(item))
            return false;

        item.name = "arrange";
        item.icon_filename_callback = [](bool is_dark_mode)->std::string {
            return is_dark_mode ? "toolbar_arrange_dark.svg" : "toolbar_arrange.svg";
            };
        item.tooltip = _utf8(L("Arrange all objects")) + " [A]\n" + _utf8(L("Arrange objects on selected plates")) + " [Shift+A]";
        item.sprite_id = sprite_id++;
        item.left.action_callback = [this]() {
            if (m_canvas != nullptr) {
                NetworkAgent* agent = GUI::wxGetApp().getAgent();
                if (agent) agent->track_update_property("auto_arrange", std::to_string(++auto_arrange_count));
            }
            };
        item.enabling_callback = []()->bool { return wxGetApp().plater()->can_do_ui_job(); };
        item.left.toggable = true;
        //BBS: GUI refactor: adjust the main toolbar position
        item.left.render_callback = [this](float left, float right, float bottom, float top, float toolbar_height) {
            if (m_canvas != nullptr)
            {
                _render_arrange_menu(left, main_toolbar_window_top(top)); //Orio: beside the toolbar if moved
                //_render_arrange_menu(0.5f * (left + right));
            }
            };
        if (!p_main_toolbar->add_item(item))
            return false;

        GLToolbarItem::Data layers_editing_item;
        layers_editing_item.name = "layersediting";
        layers_editing_item.icon_filename_callback = [](bool is_dark_mode)->std::string {
            return is_dark_mode ? "toolbar_variable_layer_height_dark.svg" : "toolbar_variable_layer_height.svg";
            };
        layers_editing_item.tooltip = _utf8(L("Variable layer height"));
        layers_editing_item.additional_tooltip = _u8L("Please select single object.");
        layers_editing_item.sprite_id = sprite_id++;
        layers_editing_item.left.action_callback = [this]()->void{
            if (m_canvas != nullptr) {
                wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_LAYERSEDITING));
                NetworkAgent* agent = GUI::wxGetApp().getAgent();
                if (agent) agent->track_update_property("custom_height", std::to_string(++custom_height_count));
            }
        };
        layers_editing_item.right.action_callback = [this]()->void {
            enable_layers_editing(false);
        };
        layers_editing_item.visibility_callback = [this, &p_main_toolbar]()->bool {
            bool res = current_printer_technology() == ptFFF;
            // turns off if changing printer technology
            if (!res && p_main_toolbar->is_item_visible("layersediting") && p_main_toolbar->is_item_pressed("layersediting"))
                force_main_toolbar_left_action(get_main_toolbar_item_id("layersediting"));

            return res;
            };
        layers_editing_item.enabling_callback = []()->bool { return wxGetApp().plater()->can_layers_editing(); };
        layers_editing_item.left.toggable = true;
        if (!p_main_toolbar->add_item(layers_editing_item))
            return false;

        //BBS: variable print speed along Z, same interaction model as the variable layer height.
        GLToolbarItem::Data speed_editing_item;
        speed_editing_item.name = "speedediting";
        speed_editing_item.icon_filename_callback = [](bool is_dark_mode)->std::string {
            return is_dark_mode ? "toolbar_variable_speed_dark.svg" : "toolbar_variable_speed.svg";
            };
        speed_editing_item.tooltip = _utf8(L("Variable speed"));
        speed_editing_item.additional_tooltip = _u8L("Please select single object.");
        speed_editing_item.sprite_id = sprite_id++;
        speed_editing_item.left.action_callback = [this]()->void {
            if (m_canvas != nullptr)
                wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_SPEEDEDITING));
        };
        speed_editing_item.right.action_callback = [this]()->void {
            enable_speed_editing(false);
        };
        speed_editing_item.visibility_callback = [this, &p_main_toolbar]()->bool {
            bool res = current_printer_technology() == ptFFF;
            // turns off if changing printer technology
            if (!res && p_main_toolbar->is_item_visible("speedediting") && p_main_toolbar->is_item_pressed("speedediting"))
                force_main_toolbar_left_action(get_main_toolbar_item_id("speedediting"));

            return res;
            };
        speed_editing_item.enabling_callback = []()->bool { return wxGetApp().plater()->can_speed_editing(); };
        //Orio: the tool is also opened or closed without the button (Undo / Redo, deleting the object,
        // another tool of the bar on the right): keep the button state in sync with it.
        speed_editing_item.pressed_recheck_callback = [this]()->bool { return is_speed_editing_enabled(); };
        speed_editing_item.left.toggable = true;
        if (!p_main_toolbar->add_item(speed_editing_item))
            return false;

        //Orio: variable sparse infill density / wall count along Z, same interaction model.
        GLToolbarItem::Data infill_walls_editing_item;
        infill_walls_editing_item.name = "infillwallsediting";
        infill_walls_editing_item.icon_filename_callback = [](bool is_dark_mode)->std::string {
            return is_dark_mode ? "toolbar_variable_infill_walls_dark.svg" : "toolbar_variable_infill_walls.svg";
            };
        infill_walls_editing_item.tooltip = _utf8(L("Variable infill and walls"));
        infill_walls_editing_item.additional_tooltip = _u8L("Please select single object.");
        infill_walls_editing_item.sprite_id = sprite_id++;
        infill_walls_editing_item.left.action_callback = [this]()->void {
            if (m_canvas != nullptr)
                wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_INFILLWALLSEDITING));
        };
        infill_walls_editing_item.right.action_callback = [this]()->void {
            enable_infill_walls_editing(false);
        };
        infill_walls_editing_item.visibility_callback = [this, &p_main_toolbar]()->bool {
            bool res = current_printer_technology() == ptFFF;
            // turns off if changing printer technology
            if (!res && p_main_toolbar->is_item_visible("infillwallsediting") && p_main_toolbar->is_item_pressed("infillwallsediting"))
                force_main_toolbar_left_action(get_main_toolbar_item_id("infillwallsediting"));

            return res;
            };
        infill_walls_editing_item.enabling_callback = []()->bool { return wxGetApp().plater()->can_infill_walls_editing(); };
        // The tool is also opened or closed without the button (Undo / Redo, deleting the object, another tool of
        // the bar on the right): keep the button state in sync with it.
        infill_walls_editing_item.pressed_recheck_callback = [this]()->bool { return is_infill_walls_editing_enabled(); };
        infill_walls_editing_item.left.toggable = true;
        if (!p_main_toolbar->add_item(infill_walls_editing_item))
            return false;

        {
            GLToolbarItem::Data sperate_item;
            sperate_item.name = "seperator2";
            sperate_item.icon_filename_callback = [](bool is_dark_mode)->std::string {
                return "seperator.svg";
                };
            sperate_item.sprite_id = sprite_id++;
            sperate_item.left.action_callback = [this]() {};
            sperate_item.visibility_callback = []()->bool { return true; };
            sperate_item.enabling_callback = []()->bool { return true; };
            if (!p_main_toolbar->add_item(sperate_item, GLToolbarItem::EType::SeparatorLine))
                return false;
        }
    }

    const auto do_add_other_items = [this](uint8_t& sprite_id)->void {
        if (m_canvas_type == ECanvasType::CanvasView3D) {
            const auto& p_main_toolbar = get_main_toolbar();
            if (!p_main_toolbar) {
                return;
            }
            GLToolbarItem::Data item;
            item.name = "splitobjects";
            item.icon_filename_callback = [](bool is_dark_mode)->std::string {
                return is_dark_mode ? "split_objects_dark.svg" : "split_objects.svg";
                };
            item.tooltip = _utf8(L("Split to objects"));
            item.additional_tooltip = _u8L("Please select single object.") + "\n" +
                _u8L("And it is valid when there are at least two parts in object or stl has at least two meshes.");
            item.sprite_id = sprite_id++;
            item.left.render_callback = nullptr;
            item.left.action_callback = [this]() {
                if (m_canvas != nullptr) {
                    wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_SPLIT_OBJECTS));
                    NetworkAgent* agent = GUI::wxGetApp().getAgent();
                    if (agent) agent->track_update_property("split_to_objects", std::to_string(++split_to_objects_count));
                }
                };
            item.visibility_callback = GLToolbarItem::Default_Visibility_Callback;
            item.left.toggable = false;
            item.enabling_callback = []()->bool { return wxGetApp().plater()->can_split_to_objects(); };
            p_main_toolbar->add_item(item);

            item.name = "splitvolumes";
            item.icon_filename_callback = [](bool is_dark_mode)->std::string {
                return is_dark_mode ? "split_parts_dark.svg" : "split_parts.svg";
                };
            item.tooltip = _utf8(L("Split to parts"));
            item.additional_tooltip = _u8L("Please select single object.") + "\n" +
                _u8L("And it is valid when importing an stl with at least two meshes.");
            item.sprite_id = sprite_id++;
            item.left.action_callback = [this]() {
                if (m_canvas != nullptr) {
                    wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_SPLIT_VOLUMES));
                    NetworkAgent* agent = GUI::wxGetApp().getAgent();
                    if (agent) agent->track_update_property("split_to_part", std::to_string(++split_to_part_count));
                }
                };
            item.visibility_callback = GLToolbarItem::Default_Visibility_Callback;
            item.enabling_callback = []()->bool { return wxGetApp().plater()->can_split_to_volumes(); };
            p_main_toolbar->add_item(item);
        }
    };

    if (m_gizmos.is_enabled()) {
        m_gizmos.add_toolbar_items(p_main_toolbar, sprite_id, do_add_other_items);
    }
    else if (m_canvas_type == ECanvasType::CanvasView3D) {
        do_add_other_items(sprite_id);
    }

    if (m_canvas_type == ECanvasType::CanvasView3D) {

        GLToolbarItem::Data more_item;
        more_item.name = "More";
        more_item.icon_filename_callback = [](bool is_dark_mode)->std::string {
            return is_dark_mode ? "more_dark.svg" : "more.svg";
            };
        more_item.tooltip = _utf8(L("Click to Extend"));
        more_item.on_hover = [this]()->std::string {
            if (m_main_toolbar) {
                if (m_main_toolbar->is_collapsed()) {
                    return _utf8(L("Click to Extend"));
                }
                else {
                    return _utf8(L("Click to Collapse"));
                }
            }
            return "";
        };
        more_item.sprite_id = sprite_id++;
        more_item.visible = false;
        more_item.left.action_callback = [this]()->void{};
        more_item.visibility_callback = [this]()->bool {
            if (m_main_toolbar) {
                return m_main_toolbar->needs_collapsed();
            }
            return false;
        };
        more_item.enabling_callback = []()->bool { return true; };
        more_item.b_collapsible = false;
        more_item.b_collapse_button = true;
        more_item.continuous_click = true;
        if (!p_main_toolbar->add_item(more_item))
            return false;

        {
            GLToolbarItem::Data sperate_item;
            sperate_item.name = "seperator1";
            sperate_item.icon_filename_callback = [](bool is_dark_mode)->std::string {
                return "seperator.svg";
                };
            sperate_item.sprite_id = sprite_id++;
            sperate_item.left.action_callback = [this]() {};
            // Orio : masqué avec le bouton « Vue d'assemblage » qu'il séparait (voir ci-dessous)
            sperate_item.visible = false;
            sperate_item.visibility_callback = []()->bool { return false; };
            sperate_item.enabling_callback = []()->bool { return true; };
            sperate_item.b_collapsible = false;
            if (!p_main_toolbar->add_item(sperate_item, GLToolbarItem::EType::SeparatorLine))
                return false;
        }

        {
            GLToolbarItem::Data item;
            item.name = "assembly_view";
            item.icon_filename_callback = [](bool is_dark_mode)->std::string {
                return is_dark_mode ? "toolbar_assemble_dark.svg" : "toolbar_assemble.svg";
                };
            item.tooltip = _utf8(L("Overview"));
            item.sprite_id = sprite_id++;
            item.left.toggable = false;
            item.left.action_callback = [this]() {
                if (m_canvas != nullptr) {
                    s_assembly_view_thumbnail_hidden = false; // Orio : la vignette masquée revient
                    wxPostEvent(m_canvas, SimpleEvent(EVT_GLVIEWTOOLBAR_ASSEMBLE)); m_gizmos.reset_all_states(); wxGetApp().plater()->get_assmeble_canvas3D()->get_gizmos_manager().reset_all_states();
                    NetworkAgent* agent = GUI::wxGetApp().getAgent();
                    if (agent) agent->track_update_property("assembly_view", std::to_string(++assembly_view_count));
                }
                };
            item.left.render_callback = GLToolbarItem::Default_Render_Callback;
            // Orio : « Vue d'assemblage » n'est plus dans la barre (outil peu servi) : elle s'ouvre par le menu ▾ de
            // la barre de titre (MainFrame). Le bouton reste déclaré, masqué : la vignette et le retour de la vue
            // d'assemblage le lisent.
            item.visible = false;
            item.visibility_callback = [this]()->bool { return false; };
            item.enabling_callback = [this]()->bool {
                return wxGetApp().plater()->has_assmeble_view();
                };
            item.b_collapsible = false;
            if (!p_main_toolbar->add_item(item))
                return false;
        }
    }

    //Orio: the order of the buttons chosen by the user (a long press then a drag), app config "main_toolbar_order".
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        std::vector<std::string> order;
        const std::string        saved = wxGetApp().app_config->get("main_toolbar_order");
        for (size_t start = 0; start < saved.size();) {
            const size_t end = std::min(saved.find(',', start), saved.size());
            order.push_back(saved.substr(start, end - start));
            start = end + 1;
        }
        p_main_toolbar->set_order(order);
        p_main_toolbar->enable_reordering([](const std::vector<std::string> &names) {
            std::string s;
            for (const std::string &name : names)
                s += (s.empty() ? "" : ",") + name;
            wxGetApp().app_config->set("main_toolbar_order", s);
        });
    }

    p_main_toolbar->update_items_state();
    return true;
}

//BBS: GUI refactor: GLToolbar
bool GLCanvas3D::_init_select_plate_toolbar()
{
    std::string path = resources_dir() + "/images/";
    IMToolbarItem* item = new IMToolbarItem();
    bool result = item->image_texture.load_from_svg_file(path + "im_all_plates_stats.svg", false, false, false, 128);
    result = result && item->image_texture_transparent.load_from_svg_file(path + "im_all_plates_stats_transparent.svg", false, false, false, 128);
    m_sel_plate_toolbar.m_all_plates_stats_item = item;
    return result;
}

bool GLCanvas3D::_init_assembly_view_thumbnail()
{
    m_assembly_view_thumbnail.set_enabled(true);
    return true;
}

void GLCanvas3D::_update_select_plate_toolbar_stats_item(bool force_selected) {
    PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
    if (plate_list.get_nonempty_plate_list().size() > 1)
        m_sel_plate_toolbar.show_stats_item = true;
    else
        m_sel_plate_toolbar.show_stats_item = false;

    if (force_selected && m_sel_plate_toolbar.show_stats_item)
        m_sel_plate_toolbar.m_all_plates_stats_item->selected = true;
}

bool GLCanvas3D::_update_imgui_select_plate_toolbar()
{
    bool result = true;
    if (!m_sel_plate_toolbar.is_enabled()) {
        return false;
    }

    const auto& p_plater = wxGetApp().plater();
    if (!p_plater) {
        return false;
    }

    if (!p_plater->is_plate_toolbar_image_dirty()) {
        return false;
    }

    if (!p_plater->is_gcode_3mf()) {
        p_plater->update_all_plate_thumbnails(true);
    }

    _update_select_plate_toolbar_stats_item();

    m_sel_plate_toolbar.del_all_item();

    PartPlateList& plate_list = p_plater->get_partplate_list();
    for (int i = 0; i < plate_list.get_plate_count(); i++) {
        IMToolbarItem* item = new IMToolbarItem();
        PartPlate* plate = plate_list.get_plate(i);
        if (plate && plate->thumbnail_data.is_valid()) {
            PartPlate* plate = plate_list.get_plate(i);
            item->image_data = plate->thumbnail_data.pixels;
            item->image_width = plate->thumbnail_data.width;
            item->image_height = plate->thumbnail_data.height;
            result = item->generate_texture();
        }
        m_sel_plate_toolbar.m_items.push_back(item);
    }
    p_plater->clear_plate_toolbar_image_dirty();
    m_sel_plate_toolbar.is_display_scrollbar = false;
    return result;
}

bool GLCanvas3D::_update_assembly_view_thumbnail()
{
    if (!m_assembly_view_thumbnail.is_enabled()) { return false; }
    if (!wxGetApp().plater()) { return false; }

    PartPlateList &partplate_list = wxGetApp().plater()->get_partplate_list();
    auto          curr_plate      = partplate_list.get_curr_plate();
    ThumbnailData &thumbnail_assembly_view_data = partplate_list.get_thumbnail_assembly_view_data();
    const size_t   volume_count                 = m_volumes.volumes.size();
    if (m_assembly_view_thumbnail_volume_count != volume_count) {
        partplate_list.reset_thumbnail_assembly_view_data();
        m_assembly_view_thumbnail_volume_count = volume_count;
    }

    if (volume_count > 1) {
        if (thumbnail_assembly_view_data.is_valid()) {
            return false;
        }

        const auto thumbnail_render_begin = std::chrono::steady_clock::now();
        const auto &p_ogl_manager  = wxGetApp().get_opengl_manager();
        bool        b_fxaa_enabled = false;
        if (p_ogl_manager) { b_fxaa_enabled = p_ogl_manager->is_fxaa_enabled(); }

        ThumbnailsParams thumbnail_params        = {{}, false, true, true, true};
        thumbnail_params.use_plate_box           = false;
        thumbnail_params.background_color        = Vec4f(0.0f, 0.0f, 0.0f, 0.0f);
        thumbnail_params.post_processing_enabled = b_fxaa_enabled;
        GLint prev_fbo = 0;
        GLint prev_viewport[4] = {};
        glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo));
        glsafe(::glGetIntegerv(GL_VIEWPORT, prev_viewport));

        const auto render_thumbnail_begin = std::chrono::steady_clock::now();
        render_thumbnail(thumbnail_assembly_view_data, curr_plate->plate_thumbnail_width, curr_plate->plate_thumbnail_height, thumbnail_params, Camera::EType::Ortho,
                         m_assembly_view_preview_angle, false, false, {ThumbnailRenderRype::GLVolumes, ECanvasType::CanvasAssembleView, true});
        const auto render_thumbnail_end = std::chrono::steady_clock::now();

        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo));
        glsafe(::glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]));
        //debug_output_thumbnail(thumbnail_assembly_view_data, "D:/thumbnail_assembly_view_data.png");

        const bool create_new_item = m_assembly_view_thumbnail.m_items.empty();
        IMToolbarItem *item = create_new_item ? new IMToolbarItem() : m_assembly_view_thumbnail.m_items.front();
        item->image_data    = thumbnail_assembly_view_data.pixels;
        item->image_width   = thumbnail_assembly_view_data.width;
        item->image_height  = thumbnail_assembly_view_data.height;
        const auto generate_texture_begin = std::chrono::steady_clock::now();
        auto result = item->generate_texture();
        const auto generate_texture_end = std::chrono::steady_clock::now();
        if (result) {
            if (create_new_item) {
                m_assembly_view_thumbnail.m_items.push_back(item);
            }
        } else {
            if (create_new_item) {
                delete item;
            }
        }

        s_assemble_ratio = s_assemble_candidate_volumes_size == 0 ? 0 : 100 - (int) (s_isolated_volumes.size() * 100 / s_assemble_candidate_volumes_size);

        const auto thumbnail_render_end = std::chrono::steady_clock::now();
        BOOST_LOG_TRIVIAL(info) << (boost::format(
            "assembly thumbnail timing: s_assemble_ratio=%1%, s_assemble_volume_ratio=%2%, total=%3%ms, render_thumbnail=%4%ms, generate_texture=%5%ms")
            % s_assemble_ratio
            % s_assemble_volume_ratio
            % std::chrono::duration_cast<std::chrono::milliseconds>(thumbnail_render_end - thumbnail_render_begin).count()
            % std::chrono::duration_cast<std::chrono::milliseconds>(render_thumbnail_end - render_thumbnail_begin).count()
            % std::chrono::duration_cast<std::chrono::milliseconds>(generate_texture_end - generate_texture_begin).count()).str();

        return result;
    }
    return false;
}

bool GLCanvas3D::_init_return_toolbar()
{
    if (!m_return_toolbar.is_enabled())
        return true;

    return m_return_toolbar.init();
}

// BBS
#if 0
bool GLCanvas3D::_init_view_toolbar()
{
    return wxGetApp().plater()->init_view_toolbar();
}
#endif

bool GLCanvas3D::_init_collapse_toolbar()
{
    return wxGetApp().plater()->init_collapse_toolbar();
}

bool GLCanvas3D::_set_current(bool force_update)
{
    if (!m_context || !m_canvas) {
        return false;
    }
    if (m_dirty_context || force_update) {
        const auto rt = m_canvas->SetCurrent(*m_context);
        if (rt) {
            m_dirty_context = false;
        }
        return rt;
    }
    return true;
}

void GLCanvas3D::_resize(unsigned int w, unsigned int h)
{
    if (m_canvas == nullptr && m_context == nullptr)
        return;

    const std::array<unsigned int, 2> new_size = { w, h };
    if (m_old_size == new_size)
        return;

    m_old_size = new_size;

    auto* imgui = wxGetApp().imgui();
    imgui->set_display_size(static_cast<float>(w), static_cast<float>(h));

    //BBS reduce render
    if (m_last_w == w && m_last_h == h) {
        return;
    }

    m_last_w = w;
    m_last_h = h;

    float font_size = wxGetApp().em_unit();

#ifdef _WIN32
    // On Windows, if manually scaled here, rendering issues can occur when the system's Display
    // scaling is greater than 300% as the font's size gets to be to large. So, use imgui font
    // scaling instead (see: ImGuiWrapper::init_font() and issue #3401)
    font_size *= (font_size > 30.0f) ? 1.0f : 1.5f;
#else
    font_size *= 1.5f;
#endif

#if ENABLE_RETINA_GL
    imgui->set_scaling(font_size, 1.0f, m_retina_helper->get_scale_factor());
#else
    imgui->set_scaling(font_size, m_canvas->GetContentScaleFactor(), 1.0f);
#endif

    this->request_extra_frame();

    // ensures that this canvas is current
    _set_current(true);
}

BoundingBoxf3 GLCanvas3D::_max_bounding_box(bool include_gizmos, bool include_bed_model, bool include_plates, bool volumes_limit_to_expand_plate) const
{
    BoundingBoxf3 bb = volumes_bounding_box(volumes_limit_to_expand_plate);

    // The following is a workaround for gizmos not being taken in account when calculating the tight camera frustrum
    // A better solution would ask the gizmo manager for the bounding box of the current active gizmo, if any
    if (include_gizmos && m_gizmos.is_running())
    {
        const auto& t_aabb = m_gizmos.get_bounding_box();
        bb.merge(t_aabb);
    }

    bb.merge(include_bed_model ? m_bed.extended_bounding_box() : m_bed.build_volume().bounding_volume());
    if (include_plates) {
        bb.merge(wxGetApp().plater()->get_partplate_list().get_bounding_box());
    }
    //Orio: « Hors impression » area, else cut by the depth of the camera.
    if (m_canvas_type == CanvasView3D) {
        PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
        if (!plate_list.is_parking_zone_hidden() && plate_list.has_parked_instances())
            bb.merge(plate_list.get_parking_zone_box());
    }

    if (m_canvas_type == CanvasPreview) {
        const auto& t_gcode_viewer = get_gcode_viewer();
        const BoundingBoxf3& toolpath_bb = t_gcode_viewer.get_max_bounding_box();
        if (toolpath_bb.max_size() > 0.f)
            bb.merge(toolpath_bb);
        else
            bb.merge(t_gcode_viewer.get_shell_bounding_box());
    }

    if ((m_canvas_type == CanvasView3D) && (fff_print()->config().print_sequence == PrintSequence::ByObject)) {
        float height_to_lid, height_to_rod;
        wxGetApp().plater()->get_partplate_list().get_height_limits(height_to_lid, height_to_rod);
        bb.max.z() = std::max(bb.max.z(), (double)height_to_lid);
    }

    return bb;
}

void GLCanvas3D::_zoom_to_box(const BoundingBoxf3& box, double margin_factor)
{
    get_active_camera().zoom_to_box(box, margin_factor);
    m_dirty = true;
}

void GLCanvas3D::_update_camera_zoom(double zoom)
{
    get_active_camera().update_zoom(zoom);
    m_dirty = true;
}

Camera &GLCanvas3D::get_active_camera()
{
    /*if (m_canvas_type == CanvasThumbnailView) {
        return wxGetApp().plater()->get_thumbnail_camera();
    } else {*/
        return wxGetApp().plater()->get_camera(); // global camera
    //}
}

const Camera &GLCanvas3D::get_active_camera() const
{
    /*if (m_canvas_type == CanvasThumbnailView) {
        return wxGetApp().plater()->get_thumbnail_camera();
    } else {*/
    return wxGetApp().plater()->get_camera(); // global camera
    //}
}

std::vector<std::array<float, 4>> GLCanvas3D::get_active_colors() {
    return GUI::wxGetApp().plater()->get_extruders_colors();
}

GLCanvas3D::AssemblyViewButtonInfo GLCanvas3D::get_assembly_view_button_info() const
{
    AssemblyViewButtonInfo info = { 0.0f, 0.0f, 0.0f, 0.0f };

    const auto& p_main_toolbar = get_main_toolbar();
    if (p_main_toolbar && p_main_toolbar->is_enabled()) {
        std::shared_ptr<GLToolbarItem> assembly_item = p_main_toolbar->get_item("assembly_view");
        // Orio : le bouton « Vue d'assemblage » est masqué (menu ▾) : la vignette se cale sous le dernier bouton
        // visible de la barre, là où il était
        if (assembly_item && !assembly_item->is_visible()) {
            std::shared_ptr<GLToolbarItem> last_item;
            for (const std::shared_ptr<GLToolbarItem> &tb_item : p_main_toolbar->get_items())
                if (tb_item && tb_item->is_visible() && (!last_item || tb_item->render_rect[1] > last_item->render_rect[1]))
                    last_item = tb_item;
            assembly_item = last_item;
        }
        if (assembly_item) {
            // Get canvas size
            Size cnv_size = get_canvas_size();
            auto canvas_w = float(cnv_size.get_width());
            auto canvas_h = float(cnv_size.get_height());

            // Get camera zoom
            const auto& t_camera = get_active_camera();
            float zoom = (float)t_camera.get_zoom();

            // Convert from OpenGL coordinates to screen coordinates
            info.x = 0.5f * canvas_w + assembly_item->render_rect[0] * zoom;
            info.y = 0.5f * canvas_h - assembly_item->render_rect[3] * zoom;
            info.width = (assembly_item->render_rect[1] - assembly_item->render_rect[0]) * zoom;
            info.height = (assembly_item->render_rect[3] - assembly_item->render_rect[2]) * zoom;
        }
    }

    return info;
}

void GLCanvas3D::_refresh_if_shown_on_screen()
{
    if (_is_shown_on_screen()) {
        const Size& cnv_size = get_canvas_size();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());

        // Because of performance problems on macOS, where PaintEvents are not delivered
        // frequently enough, we call render() here directly when we can.
        render();
    }
}

void GLCanvas3D::_picking_pass()
{
    if (m_picking_enabled && !m_mouse.dragging && m_mouse.position != Vec2d(DBL_MAX, DBL_MAX)) {

        // Render the object for picking.
        const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
        {
            OpenGLManager::FrameBufferModifier picking_frame(*p_ogl_manager, "pickingframe", EMSAAType::Disabled);
        }

        auto& picking_camera = wxGetApp().plater()->get_picking_camera();
        picking_camera.set_type(Camera::EType::Ortho);
        picking_camera.apply_viewport(0, 0, 1, 1);

        float pick_eye_z = 0.0f;
        const Camera& camera = get_active_camera();
        Vec3d pick_eye = _mouse_to_3d(camera, { m_mouse.position(0), m_mouse.position(1) }, &pick_eye_z);

        float pick_target_z = 1.0f;
        Vec3d pick_target = _mouse_to_3d(camera, { m_mouse.position(0), m_mouse.position(1) }, &pick_target_z);
        picking_camera.look_at(pick_eye, pick_target, camera.get_dir_up());
        picking_camera.apply_projection(_max_bounding_box(true, true, true,false));//need full volumes boundingbox,not limit_to_expand_plate

        picking_camera.update_frustum();

        glsafe(::glDisable(GL_BLEND));
        glsafe(::glEnable(GL_DEPTH_TEST));

        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

        //BBS: only render plate in view 3D
        if (m_canvas_type == ECanvasType::CanvasView3D && !m_gizmos.is_paint_gizmo()) {
            _render_plates_for_picking();
        }

        m_camera_clipping_plane = m_gizmos.get_clipping_plane();
        _render_volumes_for_picking();

        //BBS: remove the bed picking logic
        //_render_bed_for_picking(!get_active_camera().is_looking_downward());

        m_gizmos.render_current_gizmo_for_picking_pass();

        const auto gpu_picking_callback = [this]()->void {
            std::vector<int>* hover_volume_idxs = const_cast<std::vector<int>*>(&m_hover_volume_idxs);
            std::vector<int>* hover_plate_idxs = const_cast<std::vector<int>*>(&m_hover_plate_idxs);

            hover_volume_idxs->clear();
            hover_plate_idxs->clear();
            if (m_tooltip.is_in_imgui()) {
                return;
            }
            // Render the object for picking.
            const Size& cnv_size = get_canvas_size();

            const auto& p_ogl_manager = wxGetApp().get_opengl_manager();

            int volume_id = -1;
            int gizmo_id = -1;

            GLubyte color[4] = { 0, 0, 0, 0 };
            bool inside = 0 <= m_mouse.position(0) && m_mouse.position(0) < cnv_size.get_width() && 0 <= m_mouse.position(1) && m_mouse.position(1) < cnv_size.get_height();
            if (inside) {
                p_ogl_manager->read_pixel(OpenGLManager::s_picking_frame, 0, 0, 1, 1, EPixelFormat::RGBA, EPixelDataType::UByte, (void*)color);
                if (picking_checksum_alpha_channel(color[0], color[1], color[2]) == color[3]) {
                    // Only non-interpolated colors are valid, those have their lowest three bits zeroed.
                    // we reserve color = (0,0,0) for occluders (as the printbed)
                    // volumes' id are shifted by 1
                    // see: _render_volumes_for_picking()
                    //BBS: remove the bed picking logic
                    //volume_id = color[0] + (color[1] << 8) + (color[2] << 16) - 1;
                    volume_id = color[0] + (color[1] << 8) + (color[2] << 16);
                    // gizmos' id are instead properly encoded by the color
                    gizmo_id = color[0] + (color[1] << 8) + (color[2] << 16);
                }
            }
            else
                m_gizmos.set_hover_id(inside && (unsigned int)gizmo_id <= GLGizmoBase::BASE_ID ? ((int)GLGizmoBase::BASE_ID - gizmo_id) : -1);

            //BBS: add plate picking logic
            int plate_hover_id = PartPlate::PLATE_BASE_ID - volume_id;
            if (plate_hover_id >= 0 && plate_hover_id < PartPlateList::MAX_PLATES_COUNT * PartPlate::GRABBER_COUNT) {
                wxGetApp().plater()->get_partplate_list().set_hover_id(plate_hover_id);
                hover_plate_idxs->emplace_back(plate_hover_id);
                const_cast<GLGizmosManager*>(&m_gizmos)->set_hover_id(-1);
            }
            else {
                wxGetApp().plater()->get_partplate_list().reset_hover_id();
                if (0 <= volume_id && volume_id < (int)m_volumes.volumes.size()) {
                    // do not add the volume id if any gizmo is active and CTRL is pressed
                    if (m_gizmos.is_allow_multi_select_parts_or_objects() || !wxGetKeyState(WXK_CONTROL)) {
                        hover_volume_idxs->emplace_back(volume_id);
                    }
                    const_cast<GLGizmosManager*>(&m_gizmos)->set_hover_id(-1);
                }
                else
                    const_cast<GLGizmosManager*>(&m_gizmos)->set_hover_id(inside && (unsigned int)volume_id <= GLGizmoBase::BASE_ID ? ((int)GLGizmoBase::BASE_ID - volume_id) : -1);
            }
            _update_volumes_hover_state();
        };

        _append_to_frame_callback(gpu_picking_callback);
    }
}

void GLCanvas3D::_rectangular_selection_picking_pass()
{
    m_gizmos.set_hover_id(-1);

    int viewport_width = 0;
    int viewport_height = 0;
    if (m_picking_enabled) {

        Vec2f center;
        center.x() = (m_rectangle_selection.get_left() + m_rectangle_selection.get_right()) * 0.5f;
        center.y() = (m_rectangle_selection.get_bottom() + m_rectangle_selection.get_top()) * 0.5f;

        const int viewport_x = static_cast<int>(m_rectangle_selection.get_left());
        const int viewport_y = static_cast<int>(get_canvas_size().get_height() - m_rectangle_selection.get_top());
        viewport_width = static_cast<int>(m_rectangle_selection.get_width());
        viewport_width = viewport_width > 1 ? viewport_width : 1;
        viewport_width = viewport_width;
        viewport_height = static_cast<int>(m_rectangle_selection.get_height());
        viewport_height = viewport_height > 1 ? viewport_height : 1;
        viewport_height = viewport_height;

        const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
        {
            OpenGLManager::FrameBufferModifier picking_frame(*p_ogl_manager, "rectangular_selection_pickingframe", EMSAAType::Disabled);
        }

        const auto& camera = get_active_camera();
        const auto main_camera_type = camera.get_type();

        auto& picking_camera = wxGetApp().plater()->get_picking_camera();
        if (main_camera_type == Camera::EType::Ortho) {

            picking_camera.set_zoom(camera.get_zoom());

            float pick_eye_z = 0.0f;
            Vec3d pick_eye = _mouse_to_3d(camera, { center.x(), center.y() }, &pick_eye_z);
            float pick_target_z = 1.0f;
            Vec3d pick_target = _mouse_to_3d(camera, { center.x(), center.y() }, &pick_target_z);
            picking_camera.look_at(pick_eye, pick_target, camera.get_dir_up());

            picking_camera.apply_viewport(viewport_x, viewport_y, viewport_width, viewport_height);
            picking_camera.set_type(Camera::EType::Ortho);

            picking_camera.apply_projection(_max_bounding_box(true, true, true,false));//need full volumes boundingbox,not limit_to_expand_plate
            picking_camera.update_frustum();
        }
        else {
            picking_camera = camera;
            picking_camera.enable_update_config_on_type_change(false);
        }

        glsafe(::glEnable(GL_SCISSOR_TEST));
        glsafe(::glScissor(viewport_x, viewport_y, viewport_width, viewport_height));

        glsafe(::glDisable(GL_BLEND));
        glsafe(::glEnable(GL_DEPTH_TEST));

        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

        _render_volumes_for_picking();
        //BBS: remove the bed picking logic
        //_render_bed_for_picking(!get_active_camera().is_looking_downward());

        glsafe(::glDisable(GL_SCISSOR_TEST));
    }

    const auto gpu_picking_callback = [this, viewport_width, viewport_height]()->void {
        std::set<int> idxs;

        if (m_picking_enabled) {
            int px_count = viewport_width * viewport_height;

            int left = (int)m_rectangle_selection.get_left();
            int top = get_canvas_size().get_height() - (int)m_rectangle_selection.get_top();
            if (left >= 0 && top >= 0 && px_count) {
#define USE_PARALLEL 1
#if USE_PARALLEL
                struct Pixel
                {
                    std::array<GLubyte, 4> data;
                    // Only non-interpolated colors are valid, those have their lowest three bits zeroed.
                    bool valid() const { return picking_checksum_alpha_channel(data[0], data[1], data[2]) == data[3]; }
                    // we reserve color = (0,0,0) for occluders (as the printbed)
                    // volumes' id are shifted by 1
                    // see: _render_volumes_for_picking()
                    //BBS: remove the bed picking logic
                    int id() const { return data[0] + (data[1] << 8) + (data[2] << 16); }
                    //int id() const { return data[0] + (data[1] << 8) + (data[2] << 16) - 1; }
                };

                std::vector<Pixel> frame(px_count);
                const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
                p_ogl_manager->read_pixel("rectangular_selection_pickingframe", left, top, viewport_width, viewport_height, EPixelFormat::RGBA, EPixelDataType::UByte, (void*)frame.data());
                tbb::spin_mutex mutex;
                tbb::parallel_for(tbb::blocked_range<size_t>(0, frame.size(), (size_t)viewport_width),
                    [this, &frame, &idxs, &mutex](const tbb::blocked_range<size_t>& range) {
                        for (size_t i = range.begin(); i < range.end(); ++i)
                            if (frame[i].valid()) {
                                int volume_id = frame[i].id();
                                if (0 <= volume_id && volume_id < (int)m_volumes.volumes.size()) {
                                    mutex.lock();
                                    idxs.insert(volume_id);
                                    mutex.unlock();
                                }
                            }
                    });
#else
                std::vector<GLubyte> frame(4 * px_count);
                p_ogl_manager->read_pixel(left, top, viewport_width, viewport_height, EPixelFormat::RGBA, EPixelDataType::UByte, (void*)frame.data());

                for (int i = 0; i < px_count; ++i)
                {
                    int px_id = 4 * i;
                    int volume_id = frame[px_id] + (frame[px_id + 1] << 8) + (frame[px_id + 2] << 16);
                    if (0 <= volume_id && volume_id < (int)m_volumes.volumes.size())
                        idxs.insert(volume_id);
                }
#endif // USE_PARALLEL
            }
        }

        m_hover_volume_idxs.assign(idxs.begin(), idxs.end());
        _update_volumes_hover_state();
    };

    _append_to_frame_callback(gpu_picking_callback);
}

void GLCanvas3D::_render_background() const
{
    bool use_error_color = false;
    if (wxGetApp().is_editor()) {
        use_error_color = m_dynamic_background_enabled &&
        (current_printer_technology() != ptSLA || !m_volumes.empty());

        if (!m_volumes.empty())
            use_error_color &= _is_any_volume_outside();
        else {
            //BBS: use current plater's bounding box
            //BoundingBoxf3 test_volume = (m_config != nullptr) ? print_volume(*m_config) : BoundingBoxf3();
            BoundingBoxf3 test_volume = (const_cast<GLCanvas3D*>(this))->_get_current_partplate_print_volume();
            const auto& t_gcode_viewer = get_gcode_viewer();
            const BoundingBoxf3& path_bounding_box = t_gcode_viewer.get_paths_bounding_box();
            if (empty(path_bounding_box))
                use_error_color = false;
            else
                //BBS: use previous result
                use_error_color = (test_volume.radius() > 0.0) ? m_toolpath_outside : false;
            //use_error_color &= (test_volume.radius() > 0.0) ? !test_volume.contains(path_bounding_box) : false;
        }
    }

    // Draws a bottom to top gradient over the complete screen.
    glsafe(::glDisable(GL_DEPTH_TEST));

    _init_fullscreen_mesh();

    float* background_color = m_is_dark ? DEFAULT_BG_LIGHT_COLOR_DARK : DEFAULT_BG_LIGHT_COLOR;
    float* error_background_color = m_is_dark ? ERROR_BG_LIGHT_COLOR_DARK : ERROR_BG_LIGHT_COLOR;

    const auto final_bg_color = use_error_color ? error_background_color : background_color;

    const auto& p_background_shader = wxGetApp().get_shader("background");
    if (!p_background_shader)
    {
        BOOST_LOG_TRIVIAL(error) << "Invalid Shader: background";
        return;
    }
    wxGetApp().bind_shader(p_background_shader);

    s_full_screen_mesh.set_color({ final_bg_color[0], final_bg_color[1], final_bg_color[2], 1.0f});

    s_full_screen_mesh.render_geometry();

    wxGetApp().unbind_shader();

    glsafe(::glEnable(GL_DEPTH_TEST));
}

void GLCanvas3D::_render_bed(bool bottom, bool show_axes)
{
    float scale_factor = 1.0;
#if ENABLE_RETINA_GL
    scale_factor = m_retina_helper->get_scale_factor();
#endif // ENABLE_RETINA_GL
    //bool show_texture = true;
    //BBS set axes mode
    const auto& p_main_toolbar = get_main_toolbar();
    bool b_main_toolbar_enabled = p_main_toolbar && p_main_toolbar->is_enabled();
    m_bed.set_axes_mode(b_main_toolbar_enabled && !m_gizmos.is_show_only_active_plate());
    m_bed.render(*this, bottom, scale_factor, show_axes);
}

void GLCanvas3D::_render_bed_for_picking(bool bottom)
{
    float scale_factor = 1.0;
#if ENABLE_RETINA_GL
    scale_factor = m_retina_helper->get_scale_factor();
#endif // ENABLE_RETINA_GL

    //m_bed.render_for_picking(*this, bottom, scale_factor);
}

void GLCanvas3D::_render_platelist(bool bottom, bool only_current, bool only_body, int hover_id, bool render_cali, bool show_grid) const
{
    wxGetApp().plater()->get_partplate_list().render(bottom, only_current, only_body, hover_id, render_cali, show_grid,
                                                     wxGetApp().app_config->get_bool("enable_opengl_multi_instance"));
    //Orio: « Hors impression » area, in the Prepare view.
    if (m_canvas_type == ECanvasType::CanvasView3D && !only_current)
        wxGetApp().plater()->get_partplate_list().render_parking_zone(bottom);
}

void GLCanvas3D::_render_plates_for_picking() const
{
    wxGetApp().plater()->get_partplate_list().render_for_picking_pass();
}

void GLCanvas3D::_render_plane() const
{
    ;//TODO render assemble plane
}

void GLCanvas3D::_render_bvh_primary_bounds()
{
    if (!GUI::wxGetApp().app_config->get_bool("enable_bvh")) {
        return;
    }
    if (GUI::wxGetApp().app_config->get("enable_assemble_view_preview") == "Close") {
        return;
    }
    if (!wxGetApp().app_config->get_bool("show_assembly_bvh_bounds"))
        return;
#if !BBL_RELEASE_TO_PUBLIC
    if (m_canvas_type == CanvasAssembleView) {
        constexpr double assembly_vol_expand_mm = 3.0;
        float            cyan_color[3]          = { 0.0f, 1.0f, 1.0f };
        for (GLVolume *vol : m_volumes.volumes) {
            if (vol->is_wipe_tower)
                continue;
            const BoundingBoxf3 &bb = vol->transformed_bounding_box();
            if (!bb.defined)
                continue;
            BoundingBoxf3 expanded = bb;
            expanded.min -= Vec3d(assembly_vol_expand_mm, assembly_vol_expand_mm, assembly_vol_expand_mm);
            expanded.max += Vec3d(assembly_vol_expand_mm, assembly_vol_expand_mm, assembly_vol_expand_mm);
            expanded.defined = true;
            m_selection.render_bounding_box(expanded, cyan_color, 1.0);
        }
    }
#endif
    if (!s_bvh_primary_bounds.defined || s_assemble_candidate_volumes_size == 0) return;


#if ENABLE_RETINA_GL
    const float scale_factor = m_retina_helper->get_scale_factor();
#else
    const float scale_factor = 1.0f;
#endif
    float primary_color[3] = { 0.2f, 0.8f, 0.2f };
    m_selection.render_bounding_box(s_bvh_primary_bounds, primary_color, scale_factor);

    if (s_bvh_expanded_bounds.defined) {
        float expanded_color[3] = { 1.0f, 0.0f, 0.0f };
        m_selection.render_bounding_box(s_bvh_expanded_bounds, expanded_color, scale_factor);
    }
    if (s_first_primary_bounds.defined) {
        float first_primary_color[3] = { 0.0f, 0.0f, 1.0f };
        m_selection.render_bounding_box(s_first_primary_bounds, first_primary_color, scale_factor);
        /*if ((s_first_primary_bounds.min - s_bvh_primary_bounds.min).norm() < 0.1f && (s_first_primary_bounds.max - s_bvh_primary_bounds.max).norm() < 0.1f) {
            std::cout << "";//for debug
        }*/
    }
}

//Orio: flags the instances linked to the selection, i.e. the other instances of a selected object. They are
// tinted (GLVolume::set_render_color()), the edits of the object apply to them too.
static void flag_linked_instances(GLVolumeCollection &volumes, const Selection &selection, const Model &model, bool enabled)
{
    std::map<int, const Selection::InstanceIdxsList*> selected_instances;
    if (enabled)
        for (const auto &[obj_idx, inst_idxs] : selection.get_content())
            if (0 <= obj_idx && obj_idx < int(model.objects.size()) && inst_idxs.size() < model.objects[obj_idx]->instances.size())
                selected_instances.emplace(obj_idx, &inst_idxs);
    for (GLVolume *volume : volumes.volumes) {
        const auto it = selected_instances.find(volume->object_idx());
        volume->linked_instance = it != selected_instances.end() && it->second->count(volume->instance_idx()) == 0;
    }
}

//BBS: add outline drawing logic
void GLCanvas3D::_render_objects(GLVolumeCollection &cur_volumes, GLVolumeCollection::ERenderType type, bool with_outline, bool in_paint_gizmo)
{
    if (cur_volumes.empty())
        return;

    if (&cur_volumes == &m_volumes && m_model != nullptr)
        flag_linked_instances(cur_volumes, m_selection, *m_model, m_canvas_type == CanvasView3D);

    glsafe(::glEnable(GL_DEPTH_TEST));

    m_camera_clipping_plane = m_gizmos.get_clipping_plane();

    if (m_picking_enabled && !in_paint_gizmo)
        // Update the layer editing selection to the first object selected, update the current object maximum Z.
        m_layers_editing.select_object(*m_model, this->is_layers_editing_enabled() ? m_selection.get_object_idx() : -1);

    //BBS: variable print speed along Z.
    if (m_picking_enabled && !in_paint_gizmo)
        m_speed_editing.select_object(*m_model, this->is_speed_editing_enabled() ? m_selection.get_object_idx() : -1);

    //Orio: variable sparse infill density / wall count along Z.
    if (m_picking_enabled && !in_paint_gizmo)
        m_infill_walls_editing.select_object(*m_model, this->is_infill_walls_editing_enabled() ? m_selection.get_object_idx() : -1);

    if (const BuildVolume &build_volume = m_bed.build_volume(); build_volume.valid()) {
        switch (build_volume.type()) {
        case BuildVolume::Type::Rectangle: {
            const BoundingBox3Base<Vec3d> bed_bb = build_volume.bounding_volume().inflated(BuildVolume::SceneEpsilon);
            cur_volumes.set_print_volume({0, // Rectangle
                { float(bed_bb.min.x()), float(bed_bb.min.y()), float(bed_bb.max.x()), float(bed_bb.max.y()) },
                { 0.0f, float(build_volume.printable_height()) } });
            break;
        }
        case BuildVolume::Type::Circle: {
            cur_volumes.set_print_volume({1, // Circle
                { unscaled<float>(build_volume.circle().center.x()), unscaled<float>(build_volume.circle().center.y()), unscaled<float>(build_volume.circle().radius + BuildVolume::SceneEpsilon), 0.0f },
                { 0.0f, float(build_volume.printable_height() + BuildVolume::SceneEpsilon) } });
            break;
        }
        default:
        case BuildVolume::Type::Convex:
        case BuildVolume::Type::Custom: {
            cur_volumes.set_print_volume({static_cast<int>(type),
                { -FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX },
                { -FLT_MAX, FLT_MAX } }
            );
        }
        }
        if (m_requires_check_outside_state) {
            cur_volumes.check_outside_state(build_volume, nullptr, nullptr, *m_model);
            m_requires_check_outside_state = false;
        }
    }

    if (m_use_clipping_planes)
        cur_volumes.set_z_range(-m_clipping_planes[0].get_data()[3], m_clipping_planes[1].get_data()[3]);
    else
        cur_volumes.set_z_range(-FLT_MAX, FLT_MAX);

    GLGizmosManager& gm = get_gizmos_manager();
    GLGizmoBase* current_gizmo = gm.get_current();
    if (m_canvas_type == CanvasAssembleView) {
        cur_volumes.set_clipping_plane(m_gizmos.get_assemble_view_clipping_plane().get_data());
    }
    else if (current_gizmo && !current_gizmo->apply_clipping_plane()) {
        cur_volumes.set_clipping_plane(ClippingPlane::ClipsNothing().get_data());
    }
    else {
        cur_volumes.set_clipping_plane(m_camera_clipping_plane.get_data());
    }
    if (m_canvas_type == CanvasAssembleView)
        cur_volumes.set_show_sinking_contours(false);
    else
        cur_volumes.set_show_sinking_contours(!m_gizmos.is_hiding_instances());

    const auto& shader = wxGetApp().get_shader("gouraud");
    ECanvasType canvas_type = this->m_canvas_type;
    std::array<float, 4> body_color  = canvas_type == ECanvasType::CanvasAssembleView ? std::array<float, 4>({1.0f, 1.0f, 0.0f, 1.0f}) ://yellow
                                                                                        std::array<float, 4>({1.0f, 1.0f, 1.0f, 1.0f});//white
    bool                 partly_inside_enable = canvas_type == ECanvasType::CanvasAssembleView ? false : true;
    auto printable_height_option = GUI::wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloatsNullable>("extruder_printable_height");
    const GUI::ERenderPipelineStage render_pipeline_stage = _get_current_render_stage();

    const auto& camera = get_active_camera();
    std::vector<std::array<float, 4>> colors = get_active_colors();
    if ((GUI::ERenderPipelineStage::Silhouette == render_pipeline_stage) || shader != nullptr) {
        if (GUI::ERenderPipelineStage::Silhouette != render_pipeline_stage)
        {
            wxGetApp().bind_shader(shader);
        }
        //Orio: cut of the Prepare tab, in the shader of the objects (gouraud) or of the selection silhouette;
        // always set, these shaders are shared with the other canvases.
        const std::shared_ptr<GLShaderProgram> clip_shader = wxGetApp().get_current_shader();
        if (clip_shader)
            m_prepare_cut.apply_uniforms(*clip_shader, &camera);
        switch (type)
        {
        default:
        case GLVolumeCollection::ERenderType::Opaque:
        {
            const GLGizmosManager& gm = get_gizmos_manager();
            if (in_paint_gizmo) {
                cur_volumes.render(
                    render_pipeline_stage, type, m_picking_enabled, camera, colors, *m_model,
                    [this](const GLVolume &volume) {
                        return true;
                    },
                    with_outline, body_color, partly_inside_enable,  nullptr);
            }
            if (dynamic_cast<GLGizmoPainterBase*>(gm.get_current()) == nullptr){
                if (m_picking_enabled && m_layers_editing.is_enabled() && (m_layers_editing.last_object_id != -1) && (m_layers_editing.object_max_z() > 0.0f) &&
                    GUI::ERenderPipelineStage::Silhouette != render_pipeline_stage) {
                    int object_id = m_layers_editing.last_object_id;
                    cur_volumes.render(
                        render_pipeline_stage, type, false, camera, colors, *m_model,[this, object_id](const GLVolume &volume) {
                        // Which volume to paint without the layer height profile shader? Orio: not the other objects
                        // when the tool hides them.
                        return volume.is_active && (volume.is_modifier || volume.composite_id.object_id != object_id) && ! m_layers_editing.hides(volume);
                        });
                    m_layers_editing.render_volumes(*this, cur_volumes);
                }
                //BBS: variable print speed along Z, colors the object by the painted speed.
                else if (m_picking_enabled && m_speed_editing.is_enabled() && (m_speed_editing.last_object_id != -1) && (m_speed_editing.object_max_z() > 0.0f) &&
                    GUI::ERenderPipelineStage::Silhouette != render_pipeline_stage) {
                    int object_id = m_speed_editing.last_object_id;
                    cur_volumes.render(
                        render_pipeline_stage, type, false, camera, colors, *m_model, [this, object_id](const GLVolume &volume) {
                        // Which volume to paint without the speed profile shader? Orio: not the other objects when
                        // the tool hides them.
                        return volume.is_active && (volume.is_modifier || volume.composite_id.object_id != object_id) && ! m_speed_editing.hides(volume);
                        });
                    m_speed_editing.render_volumes(*this, cur_volumes);
                }
                //Orio: variable sparse infill density / wall count along Z, colors the object by the painted value.
                else if (m_picking_enabled && m_infill_walls_editing.is_enabled() && (m_infill_walls_editing.last_object_id != -1) && (m_infill_walls_editing.object_max_z() > 0.0f) &&
                    GUI::ERenderPipelineStage::Silhouette != render_pipeline_stage) {
                    int object_id = m_infill_walls_editing.last_object_id;
                    cur_volumes.render(
                        render_pipeline_stage, type, false, camera, colors, *m_model, [this, object_id](const GLVolume &volume) {
                        // Which volume to paint without the infill / walls profile shader? Not the other objects when the
                        // tool hides them.
                        return volume.is_active && (volume.is_modifier || volume.composite_id.object_id != object_id) && ! m_infill_walls_editing.hides(volume);
                        });
                    m_infill_walls_editing.render_volumes(*this, cur_volumes);
                }
                else {
                    /*if (wxGetApp().plater()->is_wireframe_enabled()) {
                        if (wxGetApp().plater()->is_show_wireframe())
                            shader->set_uniform("show_wireframe", true);
                        else
                            shader->set_uniform("show_wireframe", false);
                    }*/
                    //BBS:add assemble view related logic
                    // do not cull backfaces to show broken geometry, if any
                    //Orio: nor with the cut of the Prepare tab: the back faces seen through the cut are drawn as hatched cut faces
                    // (enough with one cut plane). Then the cut faces and the sections of the modifiers with the stencil (see
                    // PrepareCut::render_caps), also in the selection silhouette; not with the stencil outline.
                    const bool silhouette = GUI::ERenderPipelineStage::Silhouette == render_pipeline_stage;
                    const bool cut_caps = m_canvas_type == ECanvasType::CanvasView3D && &cur_volumes == &m_volumes && !with_outline &&
                                          clip_shader != nullptr && m_prepare_cut.can_render_caps();
                    cur_volumes.render(
                        render_pipeline_stage, type, m_picking_enabled || m_prepare_cut.is_active(), camera, colors,
                        *m_model,[this, canvas_type](const GLVolume &volume) {
                        if (canvas_type == ECanvasType::CanvasAssembleView) {
                            return !volume.is_modifier && !volume.is_wipe_tower;
                        }
                        else {
                            return (m_render_sla_auxiliaries || volume.composite_id.volume_id >= 0);
                        }
                        },
                        with_outline, body_color, partly_inside_enable, printable_height_option ? &printable_height_option->values : nullptr);
                    if (cut_caps)
                        m_prepare_cut.render_caps(*clip_shader, camera, cur_volumes,
                            [silhouette](const GLVolume &volume) { return !silhouette || volume.selected; },
                            [&](GLVolume &target, size_t) {
                                // without the sinking contours: drawn with another shader, they would be counted
                                cur_volumes.set_show_sinking_contours(false);
                                cur_volumes.render(
                                    render_pipeline_stage, GLVolumeCollection::ERenderType::All, true, camera, colors, *m_model,
                                    [&target](const GLVolume &volume) { return &volume == &target; },
                                    false, body_color, partly_inside_enable, printable_height_option ? &printable_height_option->values : nullptr);
                                cur_volumes.set_show_sinking_contours(!m_gizmos.is_hiding_instances());
                            },
                            [&](const GLVolume &volume, size_t idx, const Transform3d &box) {
                                if (silhouette)
                                    clip_shader->set_uniform("u_model_matrix", box);
                                else {
                                    clip_shader->set_uniform("view_model_matrix", camera.get_view_matrix() * box);
                                    clip_shader->set_uniform("volume_world_matrix", box);
                                }
                                return cur_volumes.get_volume_render_color((unsigned int) idx, volume.render_color);
                            });
                }
            }
            else if (GUI::ERenderPipelineStage::Silhouette != render_pipeline_stage) {
                // In case a painting gizmo is open, it should render the painted triangles
                // before transparent objects are rendered. Otherwise they would not be
                // visible when inside modifier meshes etc.
//                GLGizmosManager::EType type = gm.get_current_type();
                if (dynamic_cast<GLGizmoPainterBase*>(gm.get_current())) {
                    wxGetApp().unbind_shader();
                    gm.render_painter_gizmo();
                    wxGetApp().bind_shader(shader);
                }
            }

            break;
        }
        case GLVolumeCollection::ERenderType::Transparent:
        {
            /*if (wxGetApp().plater()->is_wireframe_enabled()) {
                if (wxGetApp().plater()->is_show_wireframe())
                    shader->set_uniform("show_wireframe", true);
                else
                    shader->set_uniform("show_wireframe", false);
            }*/
            //BBS:add assemble view related logic
            if (in_paint_gizmo) {
                cur_volumes.render(
                    render_pipeline_stage, type, false, camera, colors, *m_model,
                    [this, canvas_type](const GLVolume &volume) {
                        return true;
                    },
                    with_outline, body_color, partly_inside_enable, nullptr);
            } else {
                //Orio: visual priority to the modifiers (and to the other transparent volumes) of the Prepare tab: a face
                // lying on a face of a part is drawn in front of it instead of fighting with it, as the painted triangles do
                // (not with a painting tool: its painted triangles already have this offset, they would fight again).
                const bool transparent_ahead = canvas_type == ECanvasType::CanvasView3D && GUI::ERenderPipelineStage::Silhouette != render_pipeline_stage &&
                                               clip_shader != nullptr && clip_shader == shader && dynamic_cast<GLGizmoPainterBase*>(current_gizmo) == nullptr;
                if (transparent_ahead)
                    clip_shader->set_uniform("offset_depth_buffer", true);
                cur_volumes.render(
                    render_pipeline_stage, type, false, camera, colors, *m_model,
                    [this, canvas_type](const GLVolume &volume) {
                        //Orio: the other objects hidden by the variable layer height tool.
                        if (m_layers_editing.hides(volume))
                            return false;
                        if (canvas_type == ECanvasType::CanvasAssembleView) {
                            return !volume.is_modifier;
                        } else {
                            //Orio: the other objects hidden by the variable infill / walls tool (each tool tests its
                            // own volumes on its own lines: the sessions do not change the same ones).
                            if (m_infill_walls_editing.hides(volume))
                                return false;
                            //Orio: the other objects hidden by the variable speed tool.
                            return ! m_speed_editing.hides(volume);
                        }
                    },
                    with_outline, body_color, partly_inside_enable, printable_height_option ? &printable_height_option->values : nullptr);
                if (transparent_ahead && wxGetApp().get_current_shader() == clip_shader)
                    clip_shader->set_uniform("offset_depth_buffer", false);
            }
            if (m_canvas_type == CanvasAssembleView && m_gizmos.m_assemble_view_data->model_objects_clipper()->get_position() > 0 && GUI::ERenderPipelineStage::Silhouette != render_pipeline_stage) {
                const GLGizmosManager& gm = get_gizmos_manager();
                wxGetApp().unbind_shader();
                gm.render_painter_assemble_view();
                wxGetApp().bind_shader(shader);
            }
            if (m_canvas_type == CanvasView3D && m_gizmos.is_paint_gizmo()) {
                cur_volumes.only_render_sinking(
                    render_pipeline_stage, type, false, camera, colors, *m_model,
                    [this, canvas_type](const GLVolume &volume) {
                        if (canvas_type == ECanvasType::CanvasAssembleView) {
                            return !volume.is_modifier;
                        } else {
                            return true;
                        }
                    },
                    with_outline, body_color, partly_inside_enable, printable_height_option ? &printable_height_option->values : nullptr);
            }
            break;
        }
        }

        /*if (wxGetApp().plater()->is_wireframe_enabled()) {
            shader->set_uniform("show_wireframe", false);
        }*/

        //Orio: no cut for the other users of these shaders (only while the same program is still bound).
        if (clip_shader && m_prepare_cut.is_active() && wxGetApp().get_current_shader() == clip_shader)
            PrepareCut::disable_uniforms(*clip_shader);

        if (GUI::ERenderPipelineStage::Silhouette != render_pipeline_stage) {
            wxGetApp().unbind_shader();
        }
    }

    m_camera_clipping_plane = ClippingPlane::ClipsNothing();
}

//BBS: GUI refactor: add canvas size as parameters
void GLCanvas3D::_render_gcode(int canvas_width, int canvas_height)
{
    auto& t_gcode_viewer = get_gcode_viewer();
    //Orio: the vertical slider on the left of the view, opposite to the settings column docked on the right
    // (overlays_on_left()): right of the plates, above the "Fit camera" button; the legend keeps at the right the room of the
    // collapse button only.
    float right_margin = SLIDER_RIGHT_MARGIN;
    if (overlays_on_left(m_canvas_type)) {
        const IMSlider *layers_slider = t_gcode_viewer.get_layers_slider();
        const float     slider_scale  = layers_slider != nullptr ? layers_slider->m_scale : 1.0f;
        t_gcode_viewer.set_vertical_sliders_view(int(m_sel_plate_toolbar_width + SLIDER_RIGHT_MARGIN * slider_scale),
                                                 int(fit_camera_button_top() - 4.0f * get_scale()));
        right_margin = SLIDER_DEFAULT_RIGHT_MARGIN + get_collapse_toolbar_width() / get_scale();
    } else
        t_gcode_viewer.set_vertical_sliders_view(0, 0);
    t_gcode_viewer.render(canvas_width, canvas_height, int(right_margin));
    IMSlider* layers_slider = t_gcode_viewer.get_layers_slider();
    IMSlider* moves_slider = t_gcode_viewer.get_moves_slider();

    if (layers_slider->is_need_post_tick_event()) {
        auto evt = new wxCommandEvent(EVT_CUSTOMEVT_TICKSCHANGED, m_canvas->GetId());
        evt->SetInt((int)layers_slider->get_post_tick_event_type());
        wxPostEvent(m_canvas, *evt);
        layers_slider->reset_post_tick_event();
    }

    if (layers_slider->is_dirty()) {
        set_volumes_z_range({layers_slider->GetLowerValueD(), layers_slider->GetHigherValueD()});
        if (t_gcode_viewer.has_data()) {
            t_gcode_viewer.set_layers_z_range({static_cast<unsigned int>(layers_slider->GetLowerValue()), static_cast<unsigned int>(layers_slider->GetHigherValue())});
        }
        layers_slider->set_as_dirty(false);
        post_event(SimpleEvent(EVT_GLCANVAS_UPDATE));
        t_gcode_viewer.update_marker_curr_move();
    }

    // Orio : coupe X/Y de l'apercu. La coupe est active des qu'un des deux curseurs
    // n'est pas sur sa plage complete ; elle se combine avec la coupe Z (filtrage par couche).
    IMSlider* x_slider = t_gcode_viewer.get_x_slider();
    IMSlider* y_slider = t_gcode_viewer.get_y_slider();
    if (x_slider && y_slider && (x_slider->is_dirty() || y_slider->is_dirty())) {
        if (t_gcode_viewer.has_data()) {
            const bool enabled = !x_slider->is_full_span() || !y_slider->is_full_span();
            t_gcode_viewer.set_xy_clip_range(x_slider->GetLowerValueD(), x_slider->GetHigherValueD(),
                                             y_slider->GetLowerValueD(), y_slider->GetHigherValueD(), enabled);
        }
        x_slider->set_as_dirty(false);
        y_slider->set_as_dirty(false);
        post_event(SimpleEvent(EVT_GLCANVAS_UPDATE));
    }

    if (moves_slider->is_dirty()) {
        moves_slider->set_as_dirty(false);
        t_gcode_viewer.update_sequential_view_current((moves_slider->GetLowerValueD() - 1.0), static_cast<unsigned int>(moves_slider->GetHigherValueD() - 1.0));
        post_event(SimpleEvent(EVT_GLCANVAS_UPDATE));
        t_gcode_viewer.update_marker_curr_move();
    }
}

void GLCanvas3D::_render_selection() const
{
    float scale_factor = 1.0;
#if ENABLE_RETINA_GL
    scale_factor = m_retina_helper->get_scale_factor();
#endif // ENABLE_RETINA_GL

    if (!m_gizmos.is_running())
        m_selection.render(scale_factor);
}

void GLCanvas3D::_render_sequential_clearance()
{
    if (m_gizmos.is_dragging())
        return;
    auto type = m_gizmos.get_current_type();
    if (type == GLGizmosManager::EType::Undefined
        || can_sequential_clearance_show_in_gizmo()) {
        m_sequential_print_clearance.render();
    }
}

#if ENABLE_RENDER_SELECTION_CENTER
void GLCanvas3D::_render_selection_center() const
{
    m_selection.render_center(m_gizmos.is_dragging());
}
#endif // ENABLE_RENDER_SELECTION_CENTER

void GLCanvas3D::_check_and_update_toolbar_icon_scale()
{
    // Don't update a toolbar scale, when we are on a Preview
    if (wxGetApp().plater()->is_preview_shown())
    {

#if ENABLE_RETINA_GL
        auto& t_gcode_viewer = get_gcode_viewer();
        IMSlider* m_layers_slider = t_gcode_viewer.get_layers_slider();
        IMSlider* m_moves_slider = t_gcode_viewer.get_moves_slider();
        const float sc = m_retina_helper->get_scale_factor();
        m_layers_slider->set_scale(sc);
        m_moves_slider->set_scale(sc);
        // Orio : curseurs de coupe X/Y
        if (IMSlider* x_slider = t_gcode_viewer.get_x_slider())
            x_slider->set_scale(sc);
        if (IMSlider* y_slider = t_gcode_viewer.get_y_slider())
            y_slider->set_scale(sc);
        t_gcode_viewer.set_scale(sc);

        auto* m_notification = wxGetApp().plater()->get_notification_manager();
        m_notification->set_scale(sc);

#endif
        return;
    }

    float scale = 1.0f;

    const auto& p_main_toolbar = get_main_toolbar();
    if (p_main_toolbar) {
        const bool auto_scale = p_main_toolbar->get_rendering_mode() == GLToolbar::EToolbarRenderingMode::Auto;
        scale = wxGetApp().toolbar_icon_scale(auto_scale);
    }
    Size cnv_size = get_canvas_size();

    //BBS: GUI refactor: GLToolbar
    float size = GLToolbar::Default_Icons_Size * scale;
    //float main_size = GLGizmosManager::Default_Icons_Size * scale;

    //Orio: the main toolbar laid out vertically against a side (moved by the user) gets smaller icons to stay in one
    // column, between the top of the view (or the collapse button of that side) and the "Fit camera" button.
    //Orio: and the size chosen by the user, dragging its sides (render_main_toolbar_sides()).
    float user_factor = 1.0f;
    if (p_main_toolbar && m_canvas_type == CanvasView3D) {
        if (!s_main_toolbar_place.loaded)
            main_toolbar_place_load();
        user_factor = s_main_toolbar_place.factor;
    }
    float vertical_ratio = 1.0f;
    if (p_main_toolbar && m_canvas_type == CanvasView3D && s_main_toolbar_place.custom && s_main_toolbar_place.vertical) {
        float band_top = 0.f, band_bottom = 0.f;
        main_toolbar_vertical_band(band_top, band_bottom);
        vertical_ratio = main_toolbar_vertical_icons_size(*p_main_toolbar, size * user_factor, band_bottom - band_top) / (size * user_factor);
    }

    // Set current size for all top toolbars. It will be used for next calculations
    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
#if ENABLE_RETINA_GL
    const float sc = m_retina_helper->get_scale_factor() * scale;
    //BBS: GUI refactor: GLToolbar
    if (p_main_toolbar) {
        p_main_toolbar->set_scale(sc * vertical_ratio * user_factor);
    }

    collapse_toolbar.set_scale(sc);
    size *= m_retina_helper->get_scale_factor();

    auto* m_notification = wxGetApp().plater()->get_notification_manager();
    m_notification->set_scale(sc);
#else
    //BBS: GUI refactor: GLToolbar
    if (p_main_toolbar) {
        p_main_toolbar->set_icons_size(size * vertical_ratio * user_factor);
    }
    collapse_toolbar.set_icons_size(wxGetApp().plater()->get_collapse_toolbar_size());
#endif // ENABLE_RETINA_GL
    // Update collapse toolbar
    collapse_toolbar.set_enabled(wxGetApp().plater()->get_sidebar_docking_state() != Sidebar::None);

    //BBS: GUI refactor: GLToolbar
#if BBS_TOOLBAR_ON_TOP
    float collapse_toolbar_width = collapse_toolbar.is_enabled() ? collapse_toolbar.get_width() : GLToolbar::Default_Icons_Size;

    float top_tb_width = collapse_toolbar_width;
    int items_cnt = collapse_toolbar.get_visible_items_cnt();
    int main_items_cnt = 0;
    if (p_main_toolbar) {
        top_tb_width += p_main_toolbar->get_width();
        main_items_cnt = p_main_toolbar->get_visible_items_cnt();
        items_cnt += main_items_cnt;
    }

    //Orio: the icons of the main toolbar are at size * user_factor (size chosen by dragging its sides): without them the
    // automatic scale would bring a toolbar filling the width of the view back to that width, the drag doing nothing.
    float noitems_width = top_tb_width - size * (items_cnt - main_items_cnt) - size * user_factor * main_items_cnt; // width of separators and borders in top toolbars

    // calculate scale needed for items in all top toolbars
#ifdef __WINDOWS__
    cnv_size.set_width(cnv_size.get_width() + collapse_toolbar_width);
#endif
    float new_h_scale = (cnv_size.get_width() - noitems_width) / (items_cnt * GLToolbar::Default_Icons_Size);

    //for protect
    if (new_h_scale <= 0) {
        new_h_scale = 1;
    }

    //use the same value as horizon
    float new_v_scale = new_h_scale;
#else
    float top_tb_width = = collapse_toolbar.get_width();
    int   items_cnt = collapse_toolbar.get_visible_items_cnt();
    float noitems_width = top_tb_width - size * items_cnt; // width of separators and borders in top toolbars

    // calculate scale needed for items in all top toolbars
    float new_h_scale = (cnv_size.get_width() - noitems_width) / (items_cnt * GLToolbar::Default_Icons_Size);

    //items_cnt = m_main_toolbar.get_visible_items_cnt() + m_gizmos.get_selectable_icons_cnt() + 3; // +3 means a place for top and view toolbars and separators in gizmos toolbar

    // calculate scale needed for items in the gizmos toolbar
    if (p_main_toolbar) {
        items_cnt += p_main_toolbar->get_visible_items_cnt();
    }
    float new_v_scale = cnv_size.get_height() / (items_cnt * GLGizmosManager::Default_Icons_Size);
#endif

    // set minimum scale as a auto scale for the toolbars
    float new_scale = std::min(new_h_scale, new_v_scale);
#if ENABLE_RETINA_GL
    new_scale /= m_retina_helper->get_scale_factor();
#endif
    if (fabs(new_scale - scale) > 0.01) // scale is changed by 1% and more
        wxGetApp().set_auto_toolbar_icon_scale(new_scale);
}

void GLCanvas3D::_render_overlays()
{
     glsafe(::glDisable(GL_DEPTH_TEST));

     _check_and_update_toolbar_icon_scale();

     _render_assemble_control();

     _render_toolbar();

     //Orio: button showing / hiding the supports of the last slicing.
     _render_sliced_supports_toggle();

     //Orio: label of the « Hors impression » area.
     _render_parking_zone_toggle();

     //Orio: card of the Preview when the plate is not sliced with the current settings.
     _render_stale_slice_card();

     //Orio: eye of the Preview hiding the hovered object.
     _render_object_hide_eye();

    //Orio: cut X/Y/Z of the Prepare tab, at the right of the canvas, below the main toolbar. On the left
    // (overlays_on_left()): drawn as in a view ending at its right side (IMSlider anchors it there), above the "Fit camera"
    // button.
    if (m_canvas_type == CanvasView3D) {
        const Size cnv_size = get_canvas_size();
        const float band_top = float(get_main_toolbar_height()) + 8.0f * get_scale();
        int width  = cnv_size.get_width();
        int height = cnv_size.get_height();
        if (overlays_on_left(m_canvas_type)) {
            width  = int(SLIDER_RIGHT_MARGIN * get_scale());
            height = int(fit_camera_button_top() - 4.0f * get_scale());
        }
        if (m_prepare_cut.render(width, height, band_top, get_scale(), m_is_dark))
            m_dirty = true;
    }

    if (m_layers_editing.last_object_id >= 0 && m_layers_editing.object_max_z() > 0.0f)
        m_layers_editing.render_overlay(*this);

    //BBS: variable print speed along Z.
    if (m_speed_editing.last_object_id >= 0 && m_speed_editing.object_max_z() > 0.0f)
        m_speed_editing.render_overlay(*this);

    //Orio: variable sparse infill density / wall count along Z.
    if (m_infill_walls_editing.last_object_id >= 0 && m_infill_walls_editing.object_max_z() > 0.0f)
        m_infill_walls_editing.render_overlay(*this);

	auto curr_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    auto curr_print_seq = curr_plate->get_real_print_seq();
    bool sequential_print = (curr_print_seq == PrintSequence::ByObject);
    std::vector<const ModelInstance*> sorted_instances;
    if (sequential_print) {
        const Print* print = fff_print();
        if (print) {
            for (const PrintObject *print_object : print->objects())
            {
                for (const PrintInstance &instance : print_object->instances())
                {
                    sorted_instances.emplace_back(instance.model_instance);
                }
            }
        }
        /*for (ModelObject* model_object : m_model->objects)
            for (ModelInstance* model_instance : model_object->instances) {
                sorted_instances.emplace_back(model_instance);
            }*/
    }
    m_labels.render(sorted_instances);
    _render_3d_navigator();
}

void GLCanvas3D::_render_style_editor()
{
    bool show_style_editor = true;
    ImGui::Begin("ImGui Style Editor", &show_style_editor);
    // You can pass in a reference ImGuiStyle structure to compare to, revert to and save to
    // (without a reference style pointer, we will use one compared locally as a reference)

    ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.50f);
    ImGui::ShowFontSelector("Fonts##Selector");
    ImGui::Separator();

    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Colors"))
        {
            static int output_dest = 0;
            static bool output_only_modified = false;
            if (ImGui::Button("Export"))
            {
                if (output_dest == 0)
                    ImGui::LogToClipboard();
                else
                    ImGui::LogToTTY();

                ImGui::LogText("RenderColors:" IM_NEWLINE);
                for (int i = 0; i < RenderCol_Count; i++)
                {
                    const ImVec4& col = RenderColor::colors[i];
                    const char* name = GetRenderColName(i);
                    if (!output_only_modified || memcmp(&col, &RenderColor::colors[i], sizeof(ImVec4)) != 0)
                        ImGui::LogText("RenderColor::colors[%s]%*s= ImVec4(%.2ff, %.2ff, %.2ff, %.2ff);" IM_NEWLINE,
                            name, 23 - (int)strlen(name), "", col.x, col.y, col.z, col.w);
                }
                ImGui::LogFinish();
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::Combo("##output_type", &output_dest, "To Clipboard\0To TTY\0");
            ImGui::SameLine(); ImGui::Checkbox("Only Modified Colors", &output_only_modified);

            static ImGuiTextFilter filter;
            filter.Draw("Filter colors", ImGui::GetFontSize() * 16);

            static ImGuiColorEditFlags alpha_flags = 0;
            if (ImGui::RadioButton("Opaque", alpha_flags == ImGuiColorEditFlags_None)) { alpha_flags = ImGuiColorEditFlags_None; } ImGui::SameLine();
            if (ImGui::RadioButton("Alpha", alpha_flags == ImGuiColorEditFlags_AlphaPreview)) { alpha_flags = ImGuiColorEditFlags_AlphaPreview; } ImGui::SameLine();
            if (ImGui::RadioButton("Both", alpha_flags == ImGuiColorEditFlags_AlphaPreviewHalf)) { alpha_flags = ImGuiColorEditFlags_AlphaPreviewHalf; } ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted("In the color list:\n"
                "Left-click on color square to open color picker,\n"
                    "Right-click to open edit options menu.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::BeginChild("##colors", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NavFlattened);
            ImGui::PushItemWidth(-160);
            for (int i = 0; i < RenderCol_Count; i++)
            {
                const char* name = GetRenderColName(i);
                if (!filter.PassFilter(name))
                    continue;
                ImGui::PushID(i);
                ImGui::ColorEdit4("##color", (float*)&RenderColor::colors[i], ImGuiColorEditFlags_AlphaBar | alpha_flags);
                // Tips: in a real user application, you may want to merge and use an icon font into the main font,
                // so instead of "Save"/"Revert" you'd use icons!
                // Read the FAQ and docs/FONTS.md about using icon fonts. It's really easy and super convenient!
                ImGui::SameLine(0.0f, 3.0f);
                if (ImGui::Button("Set")) {
                    GLVolume::update_render_colors();
                    PartPlate::update_render_colors();
                    GLGizmoBase::update_render_colors();
                    GLCanvas3D::update_render_colors();
                    Bed3D::update_render_colors();
                }
                ImGui::SameLine(0.0f, 3.0f);
                ImGui::TextUnformatted(name);
                ImGui::PopID();
            }
            ImGui::PopItemWidth();
            ImGui::EndChild();

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::PopItemWidth();
    ImGui::End();
}

void GLCanvas3D::_render_volumes_for_picking() const
{
    //Orio: with the cut of the Prepare tab, a hidden part is no more pickable and a cut face picks its part
    // (shader flat_clip, optional: plain flat without it). As displayed (see _render_objects()), the cut faces take the
    // depth of the cut plane and the sections of the modifiers are drawn in front of them: a click picks what is seen.
    std::shared_ptr<GLShaderProgram> shader = m_prepare_cut.is_active() ? wxGetApp().get_shader("flat_clip") : nullptr;
    const bool clip = shader != nullptr;
    if (!shader)
        shader = wxGetApp().get_shader("flat");
    if (!shader)
        return;

    Camera& camera = wxGetApp().plater()->get_picking_camera();
    wxGetApp().bind_shader(shader);
    if (clip)
        m_prepare_cut.apply_uniforms(*shader, &camera);

    static const GLfloat INV_255 = 1.0f / 255.0f;

    // do not cull backfaces to show broken geometry, if any
    glsafe(::glDisable(GL_CULL_FACE));

    const auto& view_matrix = camera.get_view_matrix();
    const auto& proj_matrix = camera.get_projection_matrix();
    shader->set_uniform("projection_matrix", proj_matrix);
    std::vector<std::array<float, 4>> colors;
    auto picking_color = [](unsigned int id) {
        // Object picking mode. Render the object with a color encoding the object index.
        // we reserve color = (0,0,0) for occluders (as the printbed)
        // so we shift volumes' id by 1 to get the proper color
        //BBS: remove the bed picking logic
        //unsigned int id = 1 + volume.second.first;
        unsigned int r = (id & (0x000000FF << 0)) << 0;
        unsigned int g = (id & (0x000000FF << 8)) >> 8;
        unsigned int b = (id & (0x000000FF << 16)) >> 16;
        unsigned int a = picking_checksum_alpha_channel(r, g, b);
        return std::array<float, 4>{ (GLfloat)r * INV_255, (GLfloat)g * INV_255, (GLfloat)b * INV_255, (GLfloat)a * INV_255 };
    };
    auto render_for_picking = [&](GLVolume &volume, unsigned int id) {
        shader->set_uniform("view_model_matrix", view_matrix * volume.world_matrix());
        if (clip)
            shader->set_uniform("volume_world_matrix", volume.world_matrix());
        shader->set_uniform("uniform_color", picking_color(id));
        volume.picking = true;
        volume.render(camera, colors, *m_model); // colors is no use here
        volume.picking = false;
    };
    const bool cut_caps = clip && m_prepare_cut.can_render_caps();
    for (size_t type = 0; type < 2; ++ type) {
        GLVolumeWithIdAndZList to_render = volumes_to_render(m_volumes.volumes, (type == 0) ? GLVolumeCollection::ERenderType::Opaque : GLVolumeCollection::ERenderType::Transparent, view_matrix);
        for (const GLVolumeWithIdAndZ& volume : to_render)
	        if (!volume.first->disabled && (volume.first->composite_id.volume_id >= 0 || m_render_sla_auxiliaries) &&
                //Orio: nor the other objects hidden by the variable speed tool.
                ! m_speed_editing.hides(*volume.first)) {
                //Orio: nor the other objects hidden by the variable infill / walls tool (each tool tests its own
                // volumes on its own lines: the sessions do not change the same ones).
                if (m_infill_walls_editing.hides(*volume.first))
                    continue;
                auto world_box = volume.first->transformed_bounding_box();
                if (!camera.getFrustum().intersects(world_box)) {
                    continue;
                }
                //Orio: nor the other objects hidden by the variable layer height tool.
                if (m_layers_editing.hides(*volume.first))
                    continue;
                render_for_picking(*volume.first, volume.second.first);
            }
        if (type == 0 && cut_caps)
            m_prepare_cut.render_caps(*shader, camera, m_volumes,
                [](const GLVolume &volume) { return !volume.disabled; },
                [&](GLVolume &volume, size_t idx) { render_for_picking(volume, (unsigned int) idx); },
                [&](const GLVolume &, size_t idx, const Transform3d &box) {
                    shader->set_uniform("view_model_matrix", view_matrix * box);
                    shader->set_uniform("volume_world_matrix", box);
                    return picking_color((unsigned int) idx);
                });
    }

    wxGetApp().unbind_shader();

    glsafe(::glEnable(GL_CULL_FACE));
}

void GLCanvas3D::_render_current_gizmo() const
{
    //BBS update inv_zoom
    GLGizmoBase::INV_ZOOM = (float)get_active_camera().get_inv_zoom();
    m_gizmos.render_current_gizmo();
}

//BBS: GUI refactor: GLToolbar adjust
//when rendering, {0, 0} is at the center, left-up is -0.5, 0.5, right-up is 0.5, -0.5
void GLCanvas3D::_render_main_toolbar()
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return;
    }
    if (!p_main_toolbar->is_enabled())
        return;

    const auto& t_camera = get_active_camera();
    //Orio: the band of a vertical toolbar, below the collapse button of its side, above the "Fit camera" button;
    // what is drawn over the toolbar (ImGui), that it keeps off (MainToolbarPlace::obstacles): the view cube and that button
    // (bottom left), the cut slider (on a side; not for a toolbar at the top of the view: the slider goes below it).
    float band_top = 0.f, band_bottom = 0.f;
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        main_toolbar_vertical_band(band_top, band_bottom);
        const Size           cnv_size  = get_canvas_size();
        const float          cnv_w     = (float)cnv_size.get_width();
        const float          cnv_h     = (float)cnv_size.get_height();
        const float          nav_top   = fit_camera_button_top() - 4.0f * get_scale();
        std::vector<ImRect> &obstacles = s_main_toolbar_place.obstacles;
        obstacles.clear();
        if (fit_camera_button_top() < cnv_h)
            obstacles.emplace_back(0.f, nav_top, 128.f * m_sc + 4.0f * get_scale(), cnv_h);
        if (_is_prepare_cut_allowed() && get_main_toolbar_height() == 0) {
            const float slider_w   = SLIDER_RIGHT_MARGIN * get_scale();
            const float slider_top = 8.0f * get_scale();
            if (overlays_on_left(m_canvas_type))
                obstacles.emplace_back(0.f, slider_top, slider_w, nav_top);
            else
                obstacles.emplace_back(cnv_w - slider_w, slider_top, cnv_w, cnv_h);
        }
    }

    if (m_canvas_type == ECanvasType::CanvasAssembleView) {
        float inv_zoom = (float)t_camera.get_inv_zoom();
        const auto& t_viewport = t_camera.get_viewport();
        float top = 0.5f * (float)t_viewport[3] * inv_zoom;
        float left = 0.5f * m_paint_toolbar_width * inv_zoom;
        p_main_toolbar->set_position(top, left);
    }
    //Orio: at the place chosen by the user (moved by its grip), else at the top, in the middle.
    else if (m_canvas_type != ECanvasType::CanvasView3D || !main_toolbar_apply_place(*p_main_toolbar, t_camera, band_top, band_bottom)) {
        float inv_zoom = (float)t_camera.get_inv_zoom();
        const auto& t_viewport = t_camera.get_viewport();
        float left = get_main_toolbar_left(t_viewport[2], inv_zoom);
        if (is_collapse_toolbar_on_left()) {
            p_main_toolbar->set_offset(get_collapse_toolbar_width() * 2.0f);
        }
        else if (get_collapse_toolbar_width() > 0.0f &&
                 (float)t_viewport[2] >= p_main_toolbar->get_width() + 2.0f * get_collapse_toolbar_width()) {
            // Orio : bouton « ‹› » à droite : la barre se décale vers la gauche d'autant
            p_main_toolbar->set_offset(-get_collapse_toolbar_width() * 2.0f);
        }
        else {
            p_main_toolbar->set_offset(0.0f);
        }
    }
    p_main_toolbar->render(t_camera);
    //Orio: where it is now (for the next frame: its place, the windows of the tools beside it), and its grip.
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        MainToolbarPlace &place = s_main_toolbar_place;
        ImVec2            min, max;
        if (main_toolbar_rect(*p_main_toolbar, t_camera, min, max)) {
            place.x0            = min.x;
            place.y0            = min.y;
            place.x1            = max.x;
            place.y1            = max.y;
            place.rect_vertical = place.custom && place.vertical;
            render_main_toolbar_grip(*this, *p_main_toolbar);
            render_main_toolbar_sides(*this, *p_main_toolbar);
        } else
            place.x1 = place.x0;
    }
    // Orio : « Assembler » n'a plus de bouton dans la barre (menu ▾ de la barre de titre). La barre ne dessine la
    // fenêtre d'un outil ouvert que sous son bouton visible : celle d'Assembler est dessinée ici, à sa place par défaut sous
    // le dernier bouton visible de la barre (là où était le bouton « Vue d'assemblage »).
    if (m_gizmos.get_current_type() == GLGizmosManager::Assembly) {
        if (GLGizmoBase *assembly = m_gizmos.get_current()) {
            float left  = 0.0f;
            float top   = 0.0f;
            float right = -FLT_MAX;
            for (const std::shared_ptr<GLToolbarItem> &item : p_main_toolbar->get_items())
                if (item && item->is_visible() && item->render_rect[1] > right) {
                    right = item->render_rect[1];
                    left  = item->render_rect[0];
                    top   = item->render_rect[3];
                }
            assembly->render_input_window(left, main_toolbar_window_top(top), (float)get_canvas_size().get_height());
        }
    }
    if (m_toolbar_highlighter.m_render_arrow){
        p_main_toolbar->render_arrow(m_toolbar_highlighter.m_toolbar_item);
    }
}

//BBS: GUI refactor: GLToolbar adjust
//when rendering, {0, 0} is at the center, {-0.5, 0.5} at the left-up
void GLCanvas3D::_render_imgui_select_plate_toolbar()
{
    if (!m_sel_plate_toolbar.is_enabled()) {
        if (!m_render_preview)
            m_render_preview = true;
        m_sel_plate_toolbar_width = 0.f; //Orio
        return;
    }

    _update_imgui_select_plate_toolbar();

    IMToolbarItem* all_plates_stats_item = m_sel_plate_toolbar.m_all_plates_stats_item;

    PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
    for (int i = 0; i < plate_list.get_plate_count(); i++) {
        if (i < m_sel_plate_toolbar.m_items.size()) {
            if (i == plate_list.get_curr_plate_index() && !all_plates_stats_item->selected)
                m_sel_plate_toolbar.m_items[i]->selected = true;
            else
                m_sel_plate_toolbar.m_items[i]->selected = false;

            m_sel_plate_toolbar.m_items[i]->percent = plate_list.get_plate(i)->get_slicing_percent();

            if (plate_list.get_plate(i)->is_slice_result_valid()) {
                if (plate_list.get_plate(i)->is_slice_result_ready_for_print())
                    m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::SLICED;
                else
                    m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::SLICE_FAILED;
            }
            else {
                if ((plate_list.get_plate(i)->has_printable_instances() && !plate_list.get_plate(i)->can_slice())
                    || plate_list.get_plate(i)->has_outside_object()
                    || wxGetApp().plater()->sidebar().has_broken_mixed_filament(plate_list.get_plate(i)))
                    m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::SLICE_FAILED;
                else {
                    if (plate_list.get_plate(i)->get_slicing_percent() < 0.0f)
                        m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::UNSLICED;
                    else
                        m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::SLICING;
                }
            }
        }
    }
    if (m_sel_plate_toolbar.show_stats_item) {
        all_plates_stats_item->percent = 0.0f;

        size_t sliced_plates_cnt = 0;
        for (auto plate : plate_list.get_nonempty_plate_list()) {
            if (plate->is_slice_result_valid() && plate->is_slice_result_ready_for_print())
                sliced_plates_cnt++;
        }
        all_plates_stats_item->percent = (float)(sliced_plates_cnt) / (float)(plate_list.get_nonempty_plate_list().size()) * 100.0f;

        if (all_plates_stats_item->percent == 0.0f)
            all_plates_stats_item->slice_state = IMToolbarItem::SliceState::UNSLICED;
        else if (sliced_plates_cnt == plate_list.get_nonempty_plate_list().size())
            all_plates_stats_item->slice_state = IMToolbarItem::SliceState::SLICED;
        else if (all_plates_stats_item->percent < 100.0f)
            all_plates_stats_item->slice_state = IMToolbarItem::SliceState::SLICING;

        for (auto toolbar_item : m_sel_plate_toolbar.m_items) {
            if(toolbar_item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
                all_plates_stats_item->slice_state = IMToolbarItem::SliceState::SLICE_FAILED;
                all_plates_stats_item->selected = false;
                break;
            }
        }

        // Changing parameters does not invalid all plates, need extra logic to validate
        bool gcode_result_valid = true;
        for (auto gcode_result : plate_list.get_nonempty_plates_slice_results()) {
            if (gcode_result->moves.size() == 0) {
                gcode_result_valid = false;
            }
        }
        auto& t_gcode_viewer = get_gcode_viewer();
        if (all_plates_stats_item->selected && all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICED && gcode_result_valid) {
            t_gcode_viewer.render_all_plates_stats(plate_list.get_nonempty_plates_slice_results());
            m_can_show_navigator = false;
            m_render_preview = false;
        }
        else{
            t_gcode_viewer.render_all_plates_stats(plate_list.get_nonempty_plates_slice_results(), false);
            m_render_preview = true;
        }
    }else
        m_render_preview = true;

    // places the toolbar on the top_left corner of the 3d scene
#if ENABLE_RETINA_GL
    float f_scale  = m_retina_helper->get_scale_factor();
#else
    float f_scale  = 1.0;
#endif
    Size cnv_size = get_canvas_size();
    auto canvas_w = float(cnv_size.get_width());
    auto canvas_h = float(cnv_size.get_height());

    m_sel_plate_toolbar.set_icon_size(100.0f * f_scale, 100.0f * f_scale);

    float button_width = m_sel_plate_toolbar.icon_width;
    float button_height = m_sel_plate_toolbar.icon_height;

    float frame_padding = 1.0f * f_scale;
    float margin_size = 4.0f * f_scale;
    float button_margin = frame_padding;

    const float y_offset = is_collapse_toolbar_on_left() ? (get_collapse_toolbar_height() + 5) : 0;
    // Make sure the window does not overlap the 3d navigator
    auto window_height_max = canvas_h - y_offset;
    if (wxGetApp().show_3d_navigator()) {
        float sc = get_scale();
#ifdef WIN32
        const int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        sc *= (float) dpi / (float) DPI_DEFAULT;
#endif // WIN32
        window_height_max -= (128 * sc + 5);
    }

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    int item_count = m_sel_plate_toolbar.m_items.size() + (m_sel_plate_toolbar.show_stats_item ? 1 : 0);
    bool show_scroll = item_count * (button_height + frame_padding * 2.0f + button_margin) - button_margin + 22.0f * f_scale > window_height_max ? true : false;
    show_scroll = m_sel_plate_toolbar.is_display_scrollbar && show_scroll;
    float window_height = std::min(item_count * (button_height + (frame_padding + margin_size) * 2.0f + button_margin) - button_margin + 28.0f * f_scale, window_height_max);
    float window_width = m_sel_plate_toolbar.icon_width + margin_size * 2 + (show_scroll ? 28.0f * f_scale : 20.0f * f_scale);
    m_sel_plate_toolbar_width = window_width; //Orio: the vertical sliders of the preview on the left go right of it

    ImVec4 window_bg = ImVec4(0.82f, 0.82f, 0.82f, 0.5f);
    ImVec4 button_active = ImVec4(0.12f, 0.56f, 0.92, 1.0f);
    ImVec4 button_hover = ImVec4(0.67f, 0.67f, 0.67, 1.0f);
    ImVec4 scroll_col = ImVec4(0.77f, 0.77f, 0.77f, 1.0f);
    //ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 1.0f));
    //use white text as the background switch to black
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, window_bg);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, window_bg);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, scroll_col);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, scroll_col);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, scroll_col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_active);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_hover);

    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

    imgui.set_next_window_pos(canvas_w * 0, canvas_h * 0 + y_offset, ImGuiCond_Always, 0, 0);
    imgui.set_next_window_size(window_width, window_height, ImGuiCond_Always);

    if (show_scroll)
        imgui.begin(_L("Select Plate"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
    else
        imgui.begin(_L("Select Plate"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowFontScale(1.2f);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * f_scale);

    ImVec2 size = ImVec2(button_width, button_height); // Size of the image we want to make visible
    ImVec4 bg_col = ImVec4(128.0f, 128.0f, 128.0f, 0.0f);
    ImVec4 tint_col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);               // No tint
    ImVec2 margin = ImVec2(margin_size, margin_size);

    if(m_sel_plate_toolbar.show_stats_item)
    {
        // draw image
        ImVec2 button_start_pos = ImGui::GetCursorScreenPos();

        if (all_plates_stats_item->selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, button_active);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_active);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_active);
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(128.0f, 128.0f, 128.0f, 0.0f));
            if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_hover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_hover);
            }
        }

        ImVec4 text_clr;
        ImTextureID btn_texture_id;
        if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::UNSLICED || all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICING || all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED)
        {
            text_clr = ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 0.2f);
            btn_texture_id = (ImTextureID)(intptr_t)(all_plates_stats_item->image_texture_transparent.get_id());
        }
        else
        {
            text_clr = ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1);
            btn_texture_id = (ImTextureID)(intptr_t)(all_plates_stats_item->image_texture.get_id());
        }
        if (ImGui::ImageButton2(btn_texture_id, size, {0,0}, {1,1}, frame_padding, bg_col, tint_col, margin)) {
            if (all_plates_stats_item->slice_state != IMToolbarItem::SliceState::SLICE_FAILED) {
                if (m_process && !m_process->running()) {
                    for (int i = 0; i < m_sel_plate_toolbar.m_items.size(); i++) {
                        m_sel_plate_toolbar.m_items[i]->selected = false;
                    }
                    all_plates_stats_item->selected = true;
                    wxGetApp().plater()->update(true, true);
                    wxCommandEvent evt = wxCommandEvent(EVT_GLTOOLBAR_SLICE_ALL);
                    wxPostEvent(wxGetApp().plater(), evt);
                }
            }
        }
        if (!all_plates_stats_item->selected) {
            m_can_show_navigator = true;
        }
        ImGui::PopStyleColor(3);

        ImVec2 start_pos = ImVec2(button_start_pos.x + frame_padding + margin.x, button_start_pos.y + frame_padding + margin.y);
        if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::UNSLICED) {
            ImVec2 size = ImVec2(button_width, button_height);
            ImVec2 end_pos = ImVec2(start_pos.x + size.x, start_pos.y + size.y);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, end_pos, IM_COL32(0, 0, 0, 80));
        }
        else if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICING) {
            ImVec2 size = ImVec2(button_width, button_height * all_plates_stats_item->percent / 100.0f);
            ImVec2 rect_start_pos = ImVec2(start_pos.x, start_pos.y + size.y);
            ImVec2 rect_end_pos = ImVec2(start_pos.x + button_width, start_pos.y + button_height);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, rect_end_pos, IM_COL32(0, 0, 0, 10));
            ImGui::GetWindowDrawList()->AddRectFilled(rect_start_pos, rect_end_pos, IM_COL32(0, 0, 0, 80));
        }
        else if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
            ImVec2 size = ImVec2(button_width, button_height);
            ImVec2 end_pos = ImVec2(start_pos.x + size.x, start_pos.y + size.y);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, end_pos, IM_COL32(40, 1, 1, 64));
            ImGui::GetWindowDrawList()->AddRect(start_pos, end_pos, IM_COL32(208, 27, 27, 255), 0.0f, 0, 1.0f);
        }
        else if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICED) {
            ImVec2 size = ImVec2(button_width, button_height);
            ImVec2 end_pos = ImVec2(start_pos.x + size.x, start_pos.y + size.y);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, end_pos, IM_COL32(0, 0, 0, 10));
        }

        // draw text
        GImGui->FontSize = 15.0f;
        ImGui::PushStyleColor(ImGuiCol_Text, text_clr);
        ImVec2 text_size = ImGui::CalcTextSize(("All Plates"));
        ImVec2 text_start_pos = ImVec2(start_pos.x + (button_width - text_size.x) / 2, start_pos.y + 3.0f * button_height / 5.0f);
        ImGui::RenderText(text_start_pos, ("All Plates"));
        text_size = ImGui::CalcTextSize(("Stats"));
        text_start_pos = ImVec2(start_pos.x + (button_width - text_size.x) / 2, text_start_pos.y + ImGui::GetTextLineHeight());
        ImGui::RenderText(text_start_pos, ("Stats"));
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.2f);
    }

    ImVec4 error_text_clr = ImVec4(1, 0, 0, 1);
    for (int i = 0; i < m_sel_plate_toolbar.m_items.size(); i++) {
        IMToolbarItem* item = m_sel_plate_toolbar.m_items[i];

        // draw image
        ImVec2 button_start_pos = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        ImVec2 uv0 = ImVec2(0.0f, 1.0f);    // UV coordinates for lower-left
        ImVec2 uv1 = ImVec2(1.0f, 0.0f);    // UV coordinates in our texture

        auto button_pos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(button_pos + margin);

        ImGui::Image(item->texture_id, size, uv0, uv1, tint_col);

        ImGui::SetCursorPos(button_pos);

        // invisible button
        auto button_size = size + margin + margin + ImVec2(2 * frame_padding, 2 * frame_padding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f * f_scale);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));
        if (item->selected) {
            ImGui::PushStyleColor(ImGuiCol_Border, button_active);
        }
        else {
            if (ImGui::IsMouseHoveringRect(button_start_pos, button_start_pos + button_size)) {
                ImGui::PushStyleColor(ImGuiCol_Border, button_hover);
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.0f, .0f, .0f, .0f));
            }
        }
        if(ImGui::Button("##invisible_button", button_size)){
            if (m_process && !m_process->running()) {
                all_plates_stats_item->selected = false;
                item->selected = true;
                // begin to slicing plate
                if (item->slice_state != IMToolbarItem::SliceState::SLICED)
                    wxGetApp().plater()->update(true, true);
                wxCommandEvent* evt = new wxCommandEvent(EVT_GLTOOLBAR_SELECT_SLICED_PLATE);
                evt->SetInt(i);
                wxQueueEvent(wxGetApp().plater(), evt);
            }
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        ImVec2 start_pos = ImVec2(button_start_pos.x + frame_padding + margin.x, button_start_pos.y + frame_padding + margin.y);
        if (item->slice_state == IMToolbarItem::SliceState::UNSLICED) {
            ImVec2 size = ImVec2(button_width, button_height);
            ImVec2 end_pos = ImVec2(start_pos.x + size.x, start_pos.y + size.y);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, end_pos, IM_COL32(0, 0, 0, 80));
        } else if (item->slice_state == IMToolbarItem::SliceState::SLICING) {
            ImVec2 size = ImVec2(button_width, button_height * item->percent / 100.0f);
            ImVec2 rect_start_pos = ImVec2(start_pos.x, start_pos.y + size.y);
            ImVec2 rect_end_pos = ImVec2(start_pos.x + button_width, start_pos.y + button_height);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, rect_end_pos, IM_COL32(0, 0, 0, 10));
            ImGui::GetWindowDrawList()->AddRectFilled(rect_start_pos, rect_end_pos, IM_COL32(0, 0, 0, 80));
        } else if (item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
            ImVec2 size    = ImVec2(button_width, button_height);
            ImVec2 end_pos = ImVec2(start_pos.x + size.x, start_pos.y + size.y);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, end_pos, IM_COL32(250, 0, 0, 64));
            ImGui::GetWindowDrawList()->AddRect(start_pos, end_pos, IM_COL32(208, 27, 27, 255), 0.0f, 0, 1.0f);
        } else if (item->slice_state == IMToolbarItem::SliceState::SLICED) {
            ImVec2 size = ImVec2(button_width, button_height);
            ImVec2 end_pos = ImVec2(start_pos.x + size.x, start_pos.y + size.y);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, end_pos, IM_COL32(0, 0, 0, 10));
        }
        // draw text
        if (item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
            ImGui::PushStyleColor(ImGuiCol_Text, error_text_clr);
            ImVec2 text_start_pos = ImVec2(start_pos.x + 10.0f, start_pos.y + 8.0f);
            ImGui::RenderText(text_start_pos, std::to_string(i + 1).c_str());
            ImGui::PopStyleColor();

        } else {
            ImVec2 text_start_pos = ImVec2(start_pos.x + 10.0f, start_pos.y + 8.0f);
            ImGui::RenderText(text_start_pos, std::to_string(i + 1).c_str());
        }
        ImGui::PopID();
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(5);

    ImVec2 window_pos  = ImGui::GetWindowPos();
    ImVec2 window_size = ImGui::GetWindowSize();
    ImVec2 window_max  = ImVec2(window_pos.x + window_size.x + 25, window_pos.y + window_size.y);
    ImRect window_rect(window_pos, window_max);
    if (ImGui::IsWindowHovered() || window_rect.Contains(ImGui::GetMousePos())) {
        m_sel_plate_toolbar.is_display_scrollbar = true;
    } else {
        m_sel_plate_toolbar.is_display_scrollbar = false;
    }

    imgui.end();
}

void GLCanvas3D::_render_assembly_view_thumbnail_toolbar()
{
    // Orio : la vignette n'apparaît dans Préparation qu'une fois la vue d'assemblage ouverte (par son bouton, le menu
    // ou un raccourci) ; fermée par sa croix, elle revient à la prochaine visite de la vue d'assemblage
    if (m_canvas_type == ECanvasType::CanvasAssembleView)
        s_assembly_view_thumbnail_hidden = false;
    auto close_assembly_notifications = []() {
        auto *notify_mgr = wxGetApp().plater() ? wxGetApp().plater()->get_notification_manager() : nullptr;
        if (notify_mgr) {
            notify_mgr->close_notification_of_type(NotificationType::BBLIsolatedVolumeInfo);
            notify_mgr->close_notification_of_type(NotificationType::BBLAssemblyFarFromOrigin);
            notify_mgr->close_notification_of_type(NotificationType::BBLIntersectsVolumeInfo);
        }
    };
    const std::string  current_enable_assemble_view_preview = GUI::wxGetApp().app_config->get("enable_assemble_view_preview");
    if (current_enable_assemble_view_preview == "Close" || m_volumes.volumes.size() < 2 || m_layers_editing.is_enabled() || m_speed_editing.is_enabled() || m_infill_walls_editing.is_enabled()) {
        close_assembly_notifications();
        return;
    }
    static ECanvasType last_canvas_type = ECanvasType::CanvasView3D;
    if (m_canvas_type == ECanvasType::CanvasView3D || m_canvas_type == ECanvasType::CanvasAssembleView) {
        if (!s_isolated_volumes.empty() && !s_isolated_notification_shown) {
            _show_isolated_volumes_notification();
            s_isolated_notification_shown = true;
        } else if (s_isolated_volumes.empty()) {
            auto *notify_mgr            = wxGetApp().plater() ? wxGetApp().plater()->get_notification_manager() : nullptr;
            if (notify_mgr) {
                notify_mgr->close_notification_of_type(NotificationType::BBLIsolatedVolumeInfo);
            }
        }

        if (!s_far_from_origin_notification_shown) {
            _check_assembly_far_from_origin();
        }
        if (last_canvas_type == ECanvasType::CanvasView3D && m_canvas_type == ECanvasType::CanvasAssembleView) {//enter CanvasAssembleView
            _show_isolated_volumes_notification();
            _check_assembly_far_from_origin();
        }
        last_canvas_type = m_canvas_type;
    }
    if (m_canvas_type == ECanvasType::CanvasAssembleView && !s_bvh_primary_bounds.defined) {
        close_assembly_notifications();
    }
    if (m_canvas_type != ECanvasType::CanvasView3D) {
        return;
    }
    if (s_enable_bvh != GUI::wxGetApp().app_config->get_bool("enable_bvh")) {
        s_enable_bvh = GUI::wxGetApp().app_config->get_bool("enable_bvh");
        wxGetApp().plater()->get_partplate_list().reset_thumbnail_assembly_view_data();
    }
    _update_assembly_view_thumbnail();
    bool auto_mode_should_close = GUI::wxGetApp().app_config->get("enable_assemble_view_preview") == "Auto" && s_assemble_ratio < 80;
    if (auto_mode_should_close) {
        if (m_canvas_type != ECanvasType::CanvasAssembleView) {
            close_assembly_notifications();
        }
        return;
    }
    if (!s_enable_bvh) {//should show thumbnail and not return
        close_assembly_notifications();
    }
    if (m_gizmos.is_running() || m_assembly_view_thumbnail.m_items.empty() || s_assemble_candidate_volumes_size < 2) {
        return;
    }
    if (s_assembly_view_thumbnail_hidden)
        return;

    //UI setting
    const float sc = get_scale();
    const int window_width  = int(148 * sc);
    const int window_height = int(148 * sc);
    const int thumb_width   = int(140 * sc);
    const int thumb_height  = int(140 * sc);
    const int thumb_pos_x = int(4 * sc);
    const int thumb_pos_y = int(4 * sc);
    const int btn_pos_x = int(7 * sc);
    const int btn_pos_y = int(122 * sc);
    const int choose_btn_pos_y = int(118 * sc);
    const int btn_size  = int(20 * sc);
    const int choose_btn_size = int(24 * sc);
    const int help_btn_pos_x = window_width - btn_size - int(7 * sc);
    const int help_btn_pos_y = btn_pos_y;
    // Orio : croix de fermeture en haut à droite
    const int close_btn_size  = int(20 * sc);
    const int close_btn_pos_x = window_width - close_btn_size - int(7 * sc);
    const int close_btn_pos_y = int(7 * sc);
    //get_assembly_view_button_info
    auto btn_info = get_assembly_view_button_info();
    //create imgui window
    ImGuiWrapper &imgui         = *wxGetApp().imgui();
    Size cnv_size              = get_canvas_size();
    auto open_assembly_view = [this]() {
        if (m_canvas != nullptr) {
            wxPostEvent(m_canvas, SimpleEvent(EVT_GLVIEWTOOLBAR_ASSEMBLE));
            m_gizmos.reset_all_states();
            wxGetApp().plater()->get_assmeble_canvas3D()->get_gizmos_manager().reset_all_states();
            NetworkAgent* agent = GUI::wxGetApp().getAgent();
            if (agent)
                agent->track_update_property("assembly_view", std::to_string(++assembly_view_count));
        }
    };

    const float window_pos_y = btn_info.y + btn_info.height + 10.0f * sc;
    const float max_window_x = (float)std::max(5, cnv_size.get_width() - window_width - 5);
    const float window_pos_x = std::max(5.0f, std::min(btn_info.x, max_window_x));
    imgui.set_next_window_pos((int)window_pos_x, (int)window_pos_y, ImGuiCond_Always, 0, 0);
    imgui.set_next_window_size(window_width, window_height, ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_is_dark ? ImVec4(57 / 255.0f, 60 / 255.0f, 60 / 255.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 77.0f / 255.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * sc);

    imgui.begin(std::string("RenderAssemblyViewThumb"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse); //

    // Render thumbnails from m_assembly_view_thumbnail.m_items
    auto item_iso = m_assembly_view_thumbnail.m_items[0];//different index for different thumb
    auto item     = item_iso;
    bool image_hovered = false;
    bool choose_button_hovered = false;
    if (item && item->texture_id) {
        ImVec2 size     = ImVec2((float)thumb_width, (float)thumb_height);
        ImVec2 uv0      = ImVec2(0.0f, 1.0f);
        ImVec2 uv1      = ImVec2(1.0f, 0.0f);
        ImVec4 tint_col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImU32  bg_col   = m_is_dark ? IM_COL32(68, 68, 70, 255) : IM_COL32(212, 212, 212, 212);

        // Center the image in the window
        ImGui::SetCursorPos(ImVec2((float)thumb_pos_x, (float)thumb_pos_y));
        ImVec2 image_pos = ImGui::GetCursorScreenPos();
        ImRect choose_button_rect(image_pos + ImVec2(float(btn_pos_x - thumb_pos_x), float(choose_btn_pos_y - thumb_pos_y)),
                                  image_pos + ImVec2(float(btn_pos_x - thumb_pos_x + choose_btn_size), float(choose_btn_pos_y - thumb_pos_y + choose_btn_size)));
        ImRect help_button_rect(image_pos + ImVec2(float(help_btn_pos_x - thumb_pos_x), float(help_btn_pos_y - thumb_pos_y)),
                                image_pos + ImVec2(float(help_btn_pos_x - thumb_pos_x + btn_size), float(help_btn_pos_y - thumb_pos_y + btn_size)));
        ImRect close_button_rect(image_pos + ImVec2(float(close_btn_pos_x - thumb_pos_x), float(close_btn_pos_y - thumb_pos_y)),
                                 image_pos + ImVec2(float(close_btn_pos_x - thumb_pos_x + close_btn_size), float(close_btn_pos_y - thumb_pos_y + close_btn_size)));
        ImGui::GetWindowDrawList()->AddRectFilled(image_pos, ImVec2(image_pos.x + size.x, image_pos.y + size.y), bg_col);
        ImGui::Image(item->texture_id, size, uv0, uv1, tint_col);
        ImGui::SetCursorPos(ImVec2((float)thumb_pos_x, (float)thumb_pos_y));
        if (ImGui::InvisibleButton("assembly_view_thumbnail_click", size)) {
            if (!choose_button_rect.Contains(ImGui::GetIO().MousePos) && !help_button_rect.Contains(ImGui::GetIO().MousePos) &&
                !close_button_rect.Contains(ImGui::GetIO().MousePos)) {
                open_assembly_view();
            }
        }
        image_hovered = ImGui::IsItemHovered() && !choose_button_rect.Contains(ImGui::GetIO().MousePos) && !help_button_rect.Contains(ImGui::GetIO().MousePos) &&
                        !close_button_rect.Contains(ImGui::GetIO().MousePos);
        ImGui::SetItemAllowOverlap();
    }

    // Render view menu button at bottom left.
    GLGizmosManager::MENU_ICON_NAME selected_view_icon = m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_ISO_DARK : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_ISO;
    switch (m_assembly_view_preview_angle) {
    case Camera::ViewAngleType::Top:    selected_view_icon = m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_TOP_DARK    : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_TOP; break;
    case Camera::ViewAngleType::Bottom: selected_view_icon = m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_BOTTOM_DARK : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_BOTTOM; break;
    case Camera::ViewAngleType::Front:  selected_view_icon = m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_FRONT_DARK  : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_FRONT; break;
    case Camera::ViewAngleType::Rear:   selected_view_icon = m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_REAR_DARK   : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_REAR; break;
    case Camera::ViewAngleType::Left:   selected_view_icon = m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_LEFT_DARK   : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_LEFT; break;
    case Camera::ViewAngleType::Right:  selected_view_icon = m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_RIGHT_DARK  : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_RIGHT; break;
    case Camera::ViewAngleType::Iso:
    default:                            selected_view_icon = m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_ISO_DARK    : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_ISO; break;
    }
    ImTextureID normal_id = m_gizmos.get_icon_texture_id(selected_view_icon);
    ImTextureID help_id   = m_gizmos.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_VIEW_HELP);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0});
    ImVec2 button_icon_size = ImVec2((float)btn_size, (float)btn_size);
    ImVec2 choose_icon_size   = ImVec2((float)choose_btn_size, (float)choose_btn_size);
    int    choose_icon_margin = int(2 * sc);
    ImGui::SetCursorPos(ImVec2((float)btn_pos_x, (float)choose_btn_pos_y));
    ImVec2 choose_button_pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(choose_button_pos, ImVec2(choose_button_pos.x + choose_icon_size.x, choose_button_pos.y + choose_icon_size.y),
                                              m_is_dark ? IM_COL32(68, 68, 70, 255) : IM_COL32(0, 0, 0, 26), 4.0f * sc);
    ImGui::GetWindowDrawList()->AddImage(normal_id, ImVec2(choose_button_pos.x + choose_icon_margin, choose_button_pos.y + choose_icon_margin),
                                         ImVec2(choose_button_pos.x + choose_btn_size - choose_icon_margin, choose_button_pos.y + choose_btn_size - choose_icon_margin));
    if (ImGui::InvisibleButton("assembly_view_preview_menu_button", choose_icon_size)) {
        m_show_assembly_view_preview_menu = !m_show_assembly_view_preview_menu;
    }
    choose_button_hovered = ImGui::IsItemHovered();
    ImGui::SetItemAllowOverlap();

    bool help_button_hovered = false;
    ImGui::SetCursorPos(ImVec2(help_btn_pos_x, help_btn_pos_y));
    if (ImGui::ImageButton3(help_id, help_id, button_icon_size, ImVec2(0, 0), ImVec2(1, 1), -1, ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1), ImVec2(0, 0))) {
        bool is_zh = wxGetApp().app_config->get("language") == "zh_CN";
        if (is_zh) {
            wxLaunchDefaultBrowser("https://wiki.bambulab.com/zh/bambu-studio/assembly-view");
        } else {
            wxLaunchDefaultBrowser("https://wiki.bambulab.com/en/bambu-studio/assembly-view");
        }
    }
    help_button_hovered = ImGui::IsItemHovered();

    // Orio : croix qui masque la vignette pour la session (le bouton « vue d'assemblage » la réaffiche)
    bool close_button_hovered = false;
    bool close_clicked        = false;
    {
        ImGui::SetCursorPos(ImVec2((float)close_btn_pos_x, (float)close_btn_pos_y));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        close_clicked        = ImGui::InvisibleButton("assembly_view_thumbnail_close", ImVec2((float)close_btn_size, (float)close_btn_size));
        close_button_hovered = ImGui::IsItemHovered();
        ImGui::SetItemAllowOverlap();
        ImDrawList  *dl = ImGui::GetWindowDrawList();
        const ImVec2 c(p0.x + 0.5f * close_btn_size, p0.y + 0.5f * close_btn_size);
        dl->AddCircleFilled(c, 0.5f * close_btn_size, close_button_hovered ? (m_is_dark ? IM_COL32(90, 90, 94, 240) : IM_COL32(0, 0, 0, 60)) :
                                                                             (m_is_dark ? IM_COL32(68, 68, 70, 220) : IM_COL32(0, 0, 0, 26)));
        const float r   = 0.22f * close_btn_size;
        const ImU32 col = m_is_dark ? IM_COL32(230, 230, 230, 255) : IM_COL32(60, 60, 60, 255);
        dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 1.6f * sc);
        dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, 1.6f * sc);
    }
    if (close_button_hovered) {
        auto temp_tooltip = _L("Hide (the assembly view button shows it again)");
        auto width        = ImGui::CalcTextSize(temp_tooltip.c_str()).x + imgui.scaled(2.0f);
        imgui.tooltip(temp_tooltip, width);
    }
    else if (choose_button_hovered) {
        auto temp_tooltip = _L("Change camera perspective");
        auto width        = ImGui::CalcTextSize(temp_tooltip.c_str()).x + imgui.scaled(2.0f);
        imgui.tooltip(temp_tooltip, width);
    }
    else if (help_button_hovered) {
        auto temp_tooltip = _L("Go to Wiki");
        auto width        = ImGui::CalcTextSize(temp_tooltip.c_str()).x + imgui.scaled(2.0f);
        imgui.tooltip(temp_tooltip, width);
    }
#if !BBL_RELEASE_TO_PUBLIC
    else if (image_hovered) {
        if (!s_isolated_volumes.empty()) {
            auto temp_tooltip = (boost::format(_u8L("Current assembly rate: %1%%%, volume rate: %2%%%")) % s_assemble_ratio % s_assemble_volume_ratio).str();
            auto width = ImGui::CalcTextSize(temp_tooltip.c_str()).x + imgui.scaled(2.0f);
            imgui.tooltip(temp_tooltip, width);
        }
    }
#endif

    if (image_hovered)
        set_cursor(Hand);
    else if (m_cursor_type == Hand)
        set_cursor(Standard);
    ImGui::PopStyleVar(2);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    imgui.end();

    if (close_clicked) {
        s_assembly_view_thumbnail_hidden   = true;
        m_show_assembly_view_preview_menu = false;
        if (m_cursor_type == Hand)
            set_cursor(Standard);
        set_as_dirty();
        request_extra_frame();
        return;
    }

    _render_assembly_view_preview_menu(window_pos_x, window_pos_y, (float)window_width, (float)window_height);
}

void GLCanvas3D::_render_assembly_view_preview_menu(float anchor_x, float anchor_y, float anchor_width, float anchor_height)
{
    if (!m_show_assembly_view_preview_menu) {
        return;
    }

    struct PreviewMenuItem {
        Camera::ViewAngleType             angle;
        GLGizmosManager::MENU_ICON_NAME   icon;
        std::string                       label;
    };

    const PreviewMenuItem items[] = {
        { Camera::ViewAngleType::Top,    m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_TOP_DARK    : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_TOP,    _utf8(_CTX(L_CONTEXT("Top", "Camera"), "Camera")) },
        { Camera::ViewAngleType::Bottom, m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_BOTTOM_DARK : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_BOTTOM, _utf8(_CTX(L_CONTEXT("Bottom", "Camera"), "Camera")) },
        { Camera::ViewAngleType::Front,  m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_FRONT_DARK  : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_FRONT,  _utf8(_CTX(L_CONTEXT("Front", "Camera"), "Camera")) },
        { Camera::ViewAngleType::Rear,   m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_REAR_DARK   : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_REAR,   _utf8(_CTX(L_CONTEXT("Back", "Camera"), "Camera")) },
        { Camera::ViewAngleType::Left,   m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_LEFT_DARK   : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_LEFT,   _utf8(_CTX(L_CONTEXT("Left", "Camera"), "Camera")) },
        { Camera::ViewAngleType::Right,  m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_RIGHT_DARK  : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_RIGHT,  _utf8(_CTX(L_CONTEXT("Right", "Camera"), "Camera")) },
        { Camera::ViewAngleType::Iso,    m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_VIEW_ISO_DARK    : GLGizmosManager::MENU_ICON_NAME::IC_VIEW_ISO,    _utf8(_CTX(L_CONTEXT("Isometric", "Camera"), "Camera")) },
    };

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    Size          cnv_size = get_canvas_size();
    const float   sc = get_scale();
    const float   row_height = 28.0f * sc;
    const float   row_spacing = 2.0f * sc;
    const float   win_padding = 12.0f * sc;
    const ImVec2  icon_size(20.0f * sc, 20.0f * sc);
    const float   icon_gap = 6.0f * sc;
    const float   row_pad_x = 2.0f * sc;
    const float   row_inset = 24.0f * sc;
    const float   text_right_margin = 8.0f * sc;

    float max_text_width = 0.0f;
    for (const PreviewMenuItem& item : items)
        max_text_width = std::max(max_text_width, ImGui::CalcTextSize(item.label.c_str()).x);

    const float   dynamic_width = row_pad_x + icon_size.x + icon_gap + icon_size.x + icon_gap + max_text_width + text_right_margin + row_inset;
    const float   menu_width = std::max(168.0f * sc, dynamic_width);
    const float   menu_height = win_padding * 2.0f + row_height * 7.0f + row_spacing * 6.0f + sc * 12;
    float         menu_pos_x = anchor_x + anchor_width - menu_width;
    float         menu_pos_y = anchor_y + anchor_height + 6.0f * sc;
    if (menu_pos_x + menu_width > cnv_size.get_width() - 5.0f)
        menu_pos_x = cnv_size.get_width() - menu_width - 5.0f;
    if (menu_pos_y + menu_height > cnv_size.get_height() - 5.0f)
        menu_pos_y = std::max(5.0f, anchor_y - menu_height - 6.0f * sc);
    menu_pos_x = std::max(5.0f, menu_pos_x);

    imgui.set_next_window_pos(menu_pos_x, menu_pos_y, ImGuiCond_Always, 0, 0);
    imgui.set_next_window_size(menu_width, menu_height, ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_is_dark ? ImVec4(32 / 255.0f, 32 / 255.0f, 36 / 255.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 77.0f / 255.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, m_is_dark ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(38.0f / 255.0f, 46.0f / 255.0f, 48.0f / 255.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(win_padding, win_padding));

    imgui.begin(std::string("AssemblyViewPreviewMenu"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);

    ImTextureID ok_id = m_gizmos.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_VIEW_OK);
    for (const PreviewMenuItem& item : items) {
        const bool selected = item.angle == m_assembly_view_preview_angle;
        ImGui::PushID((int)item.angle);
        ImVec2 row_pos = ImGui::GetCursorScreenPos();
        const float row_content_w = menu_width - row_inset;
        if (ImGui::InvisibleButton("##assembly_view_preview_item", ImVec2(row_content_w, row_height))) {
            if (m_assembly_view_preview_angle != item.angle) {
                m_assembly_view_preview_angle     = item.angle;
                m_show_assembly_view_preview_menu = false;
                wxGetApp().plater()->get_partplate_list().reset_thumbnail_assembly_view_data();
                set_as_dirty();
                request_extra_frame();
            }
        }
        bool hovered = ImGui::IsItemHovered();
        if (hovered || selected) {
            ImU32 bg = m_is_dark ? (hovered ? IM_COL32(55, 55, 59, 255) : IM_COL32(10, 10, 10, 255))
                                  : (hovered ? IM_COL32(240, 240, 240, 255) : IM_COL32(248, 248, 248, 255));
            ImGui::GetWindowDrawList()->AddRectFilled(row_pos, ImVec2(row_pos.x + row_content_w, row_pos.y + row_height), bg, 4.0f * sc);
        }
        ImGui::SetCursorScreenPos(ImVec2(row_pos.x + row_pad_x, row_pos.y + (row_height - icon_size.y) * 0.5f));
        if (selected)
            ImGui::Image(ok_id, icon_size);
        else
            ImGui::Dummy(icon_size);
        ImGui::SameLine(0.0f, icon_gap);
        ImGui::Image(m_gizmos.get_icon_texture_id(item.icon), icon_size);
        ImGui::SameLine(0.0f, icon_gap);
        {
            const char *label_begin = item.label.c_str();
            const char *label_end   = label_begin + item.label.size();
            const ImVec2 text_pos   = ImGui::GetCursorScreenPos();
            const float  text_max_x = row_pos.x + row_content_w - text_right_margin;
            const ImVec2 text_size  = ImGui::CalcTextSize(label_begin, label_end);
            const ImVec2 text_box_size(std::max(0.0f, text_max_x - text_pos.x), icon_size.y);
            const bool   is_truncated = text_size.x > text_box_size.x;

            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), text_pos, ImVec2(text_pos.x + text_box_size.x, text_pos.y + text_box_size.y),
                                      text_max_x, text_max_x, label_begin, label_end, &text_size);
            ImGui::Dummy(text_box_size);

            if (is_truncated && ImGui::IsMouseHoveringRect(text_pos, ImVec2(text_pos.x + text_box_size.x, text_pos.y + text_box_size.y))) {
                auto width = ImGui::CalcTextSize(item.label.c_str()).x + imgui.scaled(2.0f);
                imgui.tooltip(item.label, width);
            }
        }
        ImGui::PopID();
        if (item.angle != Camera::ViewAngleType::Iso)
            ImGui::Dummy(ImVec2(0.0f, row_spacing));
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    imgui.end();
}

void GLCanvas3D::_render_return_toolbar()
{
    m_return_button_bottom = 0.f;
    if (!m_return_toolbar.is_enabled())
        return;

    float font_size = ImGui::GetFontSize();
    ImVec2 real_size = ImVec2(font_size * 4, font_size * 1.7);
    ImVec2 button_icon_size = ImVec2(font_size * 1.3, font_size * 1.3);

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    Size cnv_size = get_canvas_size();
    auto canvas_w = float(cnv_size.get_width());
    auto canvas_h = float(cnv_size.get_height());
    float window_width = real_size.x + button_icon_size.x + imgui.scaled(2.0f);
    float window_height = button_icon_size.y + imgui.scaled(2.0f);
    float window_pos_x  = 30.0f + (is_collapse_toolbar_on_left() ? (get_collapse_toolbar_width() + 5.f) : 0);
    float window_pos_y = 14.0f;
    {//solve ui overlap issue
        const MainToolbarPlace &place = s_main_toolbar_place;
        if (m_canvas_type == ECanvasType::CanvasView3D && place.custom) {
            //Orio: main toolbar moved by the user: off its rectangle and its grip as drawn at this frame
            // (_render_main_toolbar() is called before): right of a vertical one, under a horizontal one. Bambu's rule below
            // takes the toolbar at the top (a vertical one is as high as the view: it put the button on "Fit camera").
            const auto &p_main_toolbar = get_main_toolbar();
            const float grip           = p_main_toolbar ? main_toolbar_grip(*p_main_toolbar) : 0.f;
            const float margin         = 5.f;
            if (place.x1 > place.x0 && window_pos_x < place.x1 + grip && place.x0 - grip < window_pos_x + window_width &&
                window_pos_y < place.y1 + grip && place.y0 - grip < window_pos_y + window_height) {
                if (place.vertical)
                    window_pos_x = place.x1 + margin;
                else
                    window_pos_y = place.y1 + margin;
            }
        }
        else if (m_canvas_type == ECanvasType::CanvasView3D) {
            float       zoom      = (float) get_active_camera().get_zoom();
            float       left_pos = 0.0f;
            const auto& p_main_toolbar = get_main_toolbar();
            if (p_main_toolbar) {
                left_pos = p_main_toolbar->get_item("add")->render_rect[0];
            }
            const float toolbar_x = 0.5 * canvas_w + left_pos * zoom;
            const float margin    = 5;
            if (toolbar_x < window_width + margin * 3) {
                window_pos_x = 5.0f;
                window_pos_y = 2.0f;
                if (p_main_toolbar) {
                    window_pos_y += p_main_toolbar->get_height();
                }
            }
        }
    }
    m_return_button_bottom = window_pos_y + window_height;
    imgui.set_next_window_pos(window_pos_x, window_pos_y, ImGuiCond_Always, 0, 0);
#ifdef __WINDOWS__
    imgui.set_next_window_size(window_width, window_height, ImGuiCond_Always);
#endif

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.149f, 0.180f, 0.188f, 0.3f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.149f, 0.180f, 0.188f, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.149f, 0.180f, 0.188f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

    imgui.begin(_L("Assembly Return"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);

    float button_width = 20;
    float button_height = 20;
    ImVec2 size = ImVec2(button_width, button_height); // Size of the image we want to make visible
    ImVec2 uv0 = ImVec2(0.0f, 0.0f);
    ImVec2 uv1 = ImVec2(1.0f, 1.0f);

    ImVec4 bg_col = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    ImVec4 tint_col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec2 margin = ImVec2(10.0f, 5.0f);

    if (ImGui::ImageTextButton(real_size,_utf8(L("return")).c_str(), m_return_toolbar.get_return_texture_id(), button_icon_size, uv0, uv1, -1, bg_col, tint_col, margin)) {
        if (m_canvas_type == ECanvasType::CanvasView3D) {
            deselect_all();
        } else if (m_canvas_type == ECanvasType::CanvasAssembleView) {
            if (m_canvas != nullptr)
                wxPostEvent(m_canvas, SimpleEvent(EVT_GLVIEWTOOLBAR_3D));
            const_cast<GLGizmosManager *>(&m_gizmos)->reset_all_states();
            wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager().reset_all_states();
            GLVolume::explosion_ratio  = 1.0;//in 3D view  GLVolume::explosion_ratio  = 1.0
            wxGetApp().plater()->get_view3D_canvas3D()->reload_scene(true);
            {
                GLCanvas3D *                          view_3d       = wxGetApp().plater()->get_view3D_canvas3D();
                const auto& p_main_toolbar = view_3d->get_main_toolbar();
                if (!p_main_toolbar) {
                    return;
                }
                std::shared_ptr<GLToolbarItem> assembly_item = p_main_toolbar->get_item("assembly_view");
                std::chrono::system_clock::time_point end           = std::chrono::system_clock::now();
                std::chrono::duration<int>            duration      = std::chrono::duration_cast<std::chrono::duration<int>>(end - assembly_item->get_start_time_point());
                int                                   times         = duration.count();

                NetworkAgent *agent = GUI::wxGetApp().getAgent();
                if (agent) {
                    std::string name          = assembly_item->get_name() + "_duration";
                    std::string value         = "";
                    int         existing_time = 0;

                    agent->track_get_property(name, value);
                    try {
                        if (value != "") { existing_time = std::stoi(value); }
                    } catch (...) {}

                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " tool name:" << name << " duration: " << times + existing_time;
                    agent->track_update_property(name, std::to_string(times + existing_time));
                }
            }
        }
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(1);

    imgui.end();
}

float GLCanvas3D::fit_camera_button_top()
{
    const float cnv_h = (float)get_canvas_size().get_height();
    if (!wxGetApp().show_3d_navigator() || !can_show_3d_navigator())
        return cnv_h;
    // the scale of _render_3d_navigator() (m_sc), the size of _render_fit_camera_toolbar()
    float sc = get_scale();
#ifdef WIN32
    const int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
    sc *= (float) dpi / (float) DPI_DEFAULT;
#endif // WIN32
    const float window_height = 2.5f * ImGui::GetFontSize() + wxGetApp().imgui()->scaled(2.0f);
    return cnv_h - 128.f * sc - window_height - 2.f * sc;
}

void GLCanvas3D::main_toolbar_vertical_band(float &top, float &bottom)
{
    const MainToolbarPlace &p       = s_main_toolbar_place;
    const auto              docking = wxGetApp().plater()->get_sidebar_docking_state();
    // the collapse button of the column is at the top of the side where the column is docked
    const bool collapse_here = (p.edge < 0 && docking == Sidebar::Left) || (p.edge > 0 && docking == Sidebar::Right);
    top    = collapse_here ? get_collapse_toolbar_height() + 4.0f * get_scale() : 0.f;
    bottom = std::max(top + 1.f, fit_camera_button_top() - 4.0f * get_scale());
}

void GLCanvas3D::_render_fit_camera_toolbar()
{
    float  font_size        = ImGui::GetFontSize();
    ImVec2 button_icon_size = ImVec2(font_size * 2.5, font_size * 2.5);

    ImGuiWrapper &imgui         = *wxGetApp().imgui();
    float         window_width  = button_icon_size.x + imgui.scaled(2.0f);
    float         window_height = button_icon_size.y + imgui.scaled(2.0f);

    Size cnv_size              = get_canvas_size();
    // Orio : au-dessus du cube de vue, centré sur lui (avant : à sa droite, en bas). Le cube occupe un carré de
    // 128 × m_sc en bas à gauche (_render_3d_navigator) : la bande du bas reste libre à sa droite (barre X / Y des outils
    // de peinture le long des axes, etc.).
    const float cube_size      = 128.f * m_sc;
    m_fit_camrea_button_pos[0] = int(std::max(0.f, 0.5f * (cube_size - window_width)));
    m_fit_camrea_button_pos[1] = int(cnv_size.get_height() - cube_size - window_height - 2.f * m_sc);
    imgui.set_next_window_pos(m_fit_camrea_button_pos[0], m_fit_camrea_button_pos[1], ImGuiCond_Always, 0, 0);
#ifdef __WINDOWS__
    imgui.set_next_window_size(window_width, window_height, ImGuiCond_Always);
#endif

    imgui.begin(_L("Fit camera"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);//

    ImTextureID normal_id = m_gizmos.get_icon_texture_id(m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_FIT_CAMERA_DARK : GLGizmosManager::MENU_ICON_NAME::IC_FIT_CAMERA);
    ImTextureID hover_id  = m_gizmos.get_icon_texture_id(m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_FIT_CAMERA_DARK_HOVER : GLGizmosManager::MENU_ICON_NAME::IC_FIT_CAMERA_HOVER);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0});

    if (ImGui::ImageButton3(normal_id, hover_id, button_icon_size, ImVec2(0, 0), ImVec2(1, 1),  -1,
                           ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1), ImVec2(10, 0))) {
        zoom_to_fit();
    }
    if (ImGui::IsItemHovered()) {
        auto temp_tooltip = _L("Fit camera to scene or selected object.");
        auto width        = ImGui::CalcTextSize(temp_tooltip.c_str()).x + imgui.scaled(2.0f);
        imgui.tooltip(temp_tooltip, width);
    }
    ImGui::PopStyleVar(2);

    imgui.end();
}

void GLCanvas3D::_render_collapse_toolbar() const
{
    auto &     plater              = *wxGetApp().plater();
    const auto sidebar_docking_dir = plater.get_sidebar_docking_state();
    // Orio : colonne flottante → bouton intégré en haut de la colonne (Plater, EVT_ORIO_SIDEBAR_FLOATING)
    if (sidebar_docking_dir == Sidebar::None) {
        return;
    }
    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();

    const auto& t_camera = get_active_camera();
    // Orio : le bouton suit la colonne — en haut à gauche de la vue si elle est ancrée à gauche, en haut à
    // droite si elle est ancrée à droite
    if (sidebar_docking_dir == Sidebar::Left)
        collapse_toolbar.set_position_mode(ToolbarLayout::EPositionMode::TopLeft);
    else {
        const float  inv_zoom   = (float)t_camera.get_inv_zoom();
        const auto  &t_viewport = t_camera.get_viewport();
        collapse_toolbar.set_position_mode(ToolbarLayout::EPositionMode::Custom);
        collapse_toolbar.set_position(0.5f * (float)t_viewport[3] * inv_zoom,
                                      (0.5f * (float)t_viewport[2] - collapse_toolbar.get_width()) * inv_zoom);
    }
    collapse_toolbar.render(t_camera);
}

//BBS reander assemble toolbar
void GLCanvas3D::_render_paint_toolbar() const
{
    if (m_canvas_type != ECanvasType::CanvasAssembleView)
        return;
#if ENABLE_RETINA_GL
    float f_scale = m_retina_helper->get_scale_factor();
#else
    float f_scale = 1.0f;
#endif
    int em_unit = wxGetApp().em_unit() / 10;

    std::vector<std::string> colors = wxGetApp().plater()->get_extruder_colors_from_plater_config();
    int extruder_num = colors.size();
    std::vector<std::string> filament_text_first_line;
    std::vector<std::string> filament_text_second_line;
    {
        auto preset_bundle = wxGetApp().preset_bundle;
        for (auto filament_name : preset_bundle->filament_presets) {
            for (auto iter = preset_bundle->filaments.lbegin(); iter != preset_bundle->filaments.end(); iter++) {
                if (filament_name.compare(iter->name) == 0) {
                    std::string display_filament_type;
                    iter->config.get_filament_type(display_filament_type);
                    auto pos = display_filament_type.find(' ');
                    if (pos != std::string::npos) {
                        filament_text_first_line.push_back(display_filament_type.substr(0, pos));
                        filament_text_second_line.push_back(display_filament_type.substr(pos + 1));
                    }
                    else {
                        filament_text_first_line.push_back(display_filament_type);
                        filament_text_second_line.push_back("");
                    }
                }
            }
        }
        while ((int)filament_text_first_line.size() < extruder_num) {
            filament_text_first_line.push_back("");
            filament_text_second_line.push_back("");
        }
    }

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const float canvas_w = float(get_canvas_size().get_width());
    const ImVec2 button_size = ImVec2(64.0f, 48.0f) * f_scale * em_unit;
    const float spacing = 4.0f * em_unit * f_scale;
    const float return_button_margin = 130.0f * em_unit * f_scale;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(spacing, spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, { 0.f, 0.f, 0.f, 0.4f });

    imgui.set_next_window_pos(0.5f * canvas_w, 0, ImGuiCond_Always, 0.5f, 0.0f);
    float constraint_window_width = canvas_w - 2 * return_button_margin;
    ImGui::SetNextWindowSizeConstraints({ 0, 0 }, { constraint_window_width, FLT_MAX });
    imgui.begin(_L("Paint Toolbar"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float cursor_y = ImGui::GetCursorPosY();
    const ImVec2 arrow_button_size = ImVec2(0.375f * button_size.x, ImGui::GetWindowHeight());
    const ImRect left_arrow_button = ImRect(ImGui::GetCurrentWindow()->Pos, ImGui::GetCurrentWindow()->Pos + arrow_button_size);
    const ImRect right_arrow_button = ImRect(ImGui::GetCurrentWindow()->Pos + ImGui::GetWindowSize() - arrow_button_size, ImGui::GetCurrentWindow()->Pos + ImGui::GetWindowSize());
    ImU32 left_arrow_button_color = IM_COL32(0, 0, 0, 0.4f * 255);
    ImU32 right_arrow_button_color = IM_COL32(0, 0, 0, 0.4f * 255);
    ImU32 arrow_color = IM_COL32(255, 255, 255, 255);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGuiContext& context = *GImGui;
    bool disabled = !wxGetApp().plater()->can_fillcolor();
    unsigned char rgb[3];

    auto gradient_info = wxGetApp().plater()->get_filament_gradient_info();

    for (int i = 0; i < extruder_num; i++) {
        if (i > 0)
            ImGui::SameLine();
        Slic3r::GUI::BitmapCache::parse_color(colors[i], rgb);
        ImGui::PushStyleColor(ImGuiCol_Button, ImColor(rgb[0], rgb[1], rgb[2]).Value);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImColor(rgb[0], rgb[1], rgb[2]).Value);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImColor(rgb[0], rgb[1], rgb[2]).Value);
        if (disabled)
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        if (ImGui::Button(("##filament_button" + std::to_string(i)).c_str(), button_size)) {
            if (!ImGui::IsMouseHoveringRect(left_arrow_button.Min, left_arrow_button.Max) && !ImGui::IsMouseHoveringRect(right_arrow_button.Min, right_arrow_button.Max))
                wxPostEvent(m_canvas, IntEvent(EVT_GLTOOLBAR_FILLCOLOR, i + 1));
        }
        if (i < (int)gradient_info.size() && gradient_info[i].is_gradient) {
            ImVec2 r_min = ImGui::GetItemRectMin();
            ImVec2 r_max = ImGui::GetItemRectMax();
            auto& gf = gradient_info[i].color_from;
            auto& gt = gradient_info[i].color_to;
            ImU32 col_from = IM_COL32(uint8_t(gf[0]*255.f), uint8_t(gf[1]*255.f), uint8_t(gf[2]*255.f), 255);
            ImU32 col_to   = IM_COL32(uint8_t(gt[0]*255.f), uint8_t(gt[1]*255.f), uint8_t(gt[2]*255.f), 255);
            draw_list->AddRectFilledMultiColor(r_min, r_max, col_from, col_to, col_to, col_from);
        }
        if (ImGui::IsItemHovered() && i < 9) {
            if (!ImGui::IsMouseHoveringRect(left_arrow_button.Min, left_arrow_button.Max) && !ImGui::IsMouseHoveringRect(right_arrow_button.Min, right_arrow_button.Max)) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 20.0f * f_scale, 10.0f * f_scale });
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f * f_scale);
                imgui.tooltip(_L("Shortcut Key ") + std::to_string(i + 1), ImGui::GetFontSize() * 20.0f);
                ImGui::PopStyleVar(2);
            }
        }
        ImGui::PopStyleColor(3);
        if (disabled)
            ImGui::PopItemFlag();
    }

    const float text_offset_y = 4.0f * em_unit * f_scale;
    for (int i = 0; i < extruder_num; i++){
        Slic3r::GUI::BitmapCache::parse_color(colors[i], rgb);
        float gray = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
        ImVec4 text_color = gray < 80 ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0, 0, 0, 1.0f);

        imgui.push_bold_font();
        ImVec2 number_label_size = ImGui::CalcTextSize(std::to_string(i + 1).c_str());
        ImGui::SetCursorPosY(cursor_y + text_offset_y);
        ImGui::SetCursorPosX(spacing + i * (spacing + button_size.x) + (button_size.x - number_label_size.x) / 2);
        ImGui::TextColored(text_color, std::to_string(i + 1).c_str());
        imgui.pop_bold_font();

        ImVec2 filament_first_line_label_size = ImGui::CalcTextSize(filament_text_first_line[i].c_str());
        ImGui::SetCursorPosY(cursor_y + text_offset_y + number_label_size.y);
        ImGui::SetCursorPosX(spacing + i * (spacing + button_size.x) + (button_size.x - filament_first_line_label_size.x) / 2);
        ImGui::TextColored(text_color, filament_text_first_line[i].c_str());

        ImVec2 filament_second_line_label_size = ImGui::CalcTextSize(filament_text_second_line[i].c_str());
        ImGui::SetCursorPosY(cursor_y + text_offset_y + number_label_size.y + filament_first_line_label_size.y);
        ImGui::SetCursorPosX(spacing + i * (spacing + button_size.x) + (button_size.x - filament_second_line_label_size.x) / 2);
        ImGui::TextColored(text_color, filament_text_second_line[i].c_str());
    }

    if (ImGui::GetWindowWidth() == constraint_window_width) {
        if (ImGui::IsMouseHoveringRect(left_arrow_button.Min, left_arrow_button.Max)) {
            left_arrow_button_color = IM_COL32(0, 0, 0, 0.64f * 255);
            if (context.IO.MouseClicked[ImGuiMouseButton_Left]) {
                ImGui::SetScrollX(ImGui::GetScrollX() - button_size.x);
                imgui.set_requires_extra_frame();
            }
        }
        draw_list->AddRectFilled(left_arrow_button.Min, left_arrow_button.Max, left_arrow_button_color);
        ImGui::BBLRenderArrow(draw_list, left_arrow_button.GetCenter() - ImVec2(draw_list->_Data->FontSize, draw_list->_Data->FontSize) * 0.5f, arrow_color, ImGuiDir_Left, 2.0f);

        if (ImGui::IsMouseHoveringRect(right_arrow_button.Min, right_arrow_button.Max)) {
            right_arrow_button_color = IM_COL32(0, 0, 0, 0.64f * 255);
            if (context.IO.MouseClicked[ImGuiMouseButton_Left]) {
                ImGui::SetScrollX(ImGui::GetScrollX() + button_size.x);
                imgui.set_requires_extra_frame();
            }
        }
        draw_list->AddRectFilled(right_arrow_button.Min, right_arrow_button.Max, right_arrow_button_color);
        ImGui::BBLRenderArrow(draw_list, right_arrow_button.GetCenter() - ImVec2(draw_list->_Data->FontSize, draw_list->_Data->FontSize) * 0.5f, arrow_color, ImGuiDir_Right, 2.0f);
    }

    m_paint_toolbar_width = (ImGui::GetWindowWidth() + 50.0f * em_unit * f_scale);
    imgui.end();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

float GLCanvas3D::_show_assembly_tooltip_information(float caption_max, float x, float y) const
{
    ImGuiWrapper *imgui     = wxGetApp().imgui();
    ImTextureID normal_id = m_gizmos.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_gizmos.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += imgui->calc_text_size(": ").x + 35.f;

    float  font_size   = ImGui::GetFontSize();
    ImVec2 button_size = ImVec2(font_size * 1.8, font_size * 1.3);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &imgui, & caption_max](const wxString &caption, const wxString &text) {
            imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 3>{"object_selection", "part_selection", "number_key"}) {
            draw_text_with_caption(m_assembly_view_desc.at(t + "_caption") + ": ", m_assembly_view_desc.at(t));
        }
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
    auto same_line_size = button_size.x * 1.8;//with an space size
    ImGui::SameLine(same_line_size);
    same_line_size = imgui->calc_text_size("|").x + same_line_size + imgui->calc_text_size("  ").x;
    imgui->text_colored(ImGuiWrapper::COL_ACTIVE, "|");
    ImGui::SameLine(same_line_size);
    return same_line_size;
}

//BBS
void GLCanvas3D::_render_assemble_control()
{
    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        GLVolume::explosion_ratio = m_explosion_ratio = 1.0;
        return;
    }
    if (m_gizmos.get_current_type() == GLGizmosManager::EType::MmuSegmentation) {
        m_gizmos.m_assemble_view_data->model_objects_clipper()->set_position(0.0, true);
        return;
    }

    ImGuiWrapper* imgui = wxGetApp().imgui();

    ImGuiWrapper::push_toolbar_style(get_scale());

    auto canvas_w = float(get_canvas_size().get_width());
    auto canvas_h = float(get_canvas_size().get_height());

    const float text_padding = 7.0f;
    const float text_size_x = std::max(imgui->calc_text_size(_L("Reset direction")).x + 2 * ImGui::GetStyle().FramePadding.x,
        std::max(imgui->calc_text_size(_L("Explosion Ratio")).x, imgui->calc_text_size(_L("Section View")).x));
    const float slider_width = 60.0f;
    const float value_size = imgui->calc_text_size("3.00").x + text_padding * 2;
    const float item_spacing = imgui->get_item_spacing().x;
    ImVec2 window_padding = ImGui::GetStyle().WindowPadding;

    imgui->set_next_window_pos(canvas_w / 2, canvas_h - 10.0f * get_scale(), ImGuiCond_Always, 0.5f, 1.0f);
    imgui->begin(_L("Assemble Control"), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::AlignTextToFramePadding();
    float tip_icon_size;
    {
        float caption_max = 0.f;
        for (const auto &t : std::array<std::string, 3>{"object_selection", "part_selection", "number_key"}) {
            caption_max = std::max(caption_max, imgui->calc_text_size(m_assembly_view_desc.at(t + "_caption")).x);
        }
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float text_y =imgui->calc_text_size(_L("part selection")).y;
        float get_cur_x = pos.x;
        float get_cur_y = pos.y - ImGui::GetFrameHeight() - 4 * text_y;
        tip_icon_size =_show_assembly_tooltip_information(caption_max, get_cur_x, get_cur_y);
    }
    float same_line_width = tip_icon_size;
    {
        float clp_dist = m_gizmos.m_assemble_view_data->model_objects_clipper()->get_position();
        if (clp_dist == 0.f) {
            ImGui::AlignTextToFramePadding();
            imgui->text(_L("Section View"));
        }
        else {
            if (imgui->button(_L("Reset direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_gizmos.m_assemble_view_data->model_objects_clipper()->set_position(-1., false);
                    });
            }
        }
        same_line_width += (text_size_x + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(slider_width);
        bool view_slider_changed = imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);

        same_line_width += (slider_width + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(value_size);
        bool view_input_changed = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (view_slider_changed || view_input_changed)
            m_gizmos.m_assemble_view_data->model_objects_clipper()->set_position(clp_dist, true);

        same_line_width += (value_size + item_spacing * 2);
    }
    {
        auto temp_x = imgui->calc_text_size(_L("Explosion Ratio")).x;
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(temp_x);
        imgui->text(_L("Explosion Ratio"));

        same_line_width +=   (temp_x + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(slider_width);
        bool explosion_slider_changed = imgui->bbl_slider_float_style("##ratio_slider", &m_explosion_ratio, 1.0f, 3.0f, "%1.2f");

        same_line_width +=  (slider_width + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(value_size);
        bool explosion_input_changed = ImGui::BBLDragFloat("##ratio_input", &m_explosion_ratio, 0.1f, 1.0f, 3.0f, "%1.2f");
        same_line_width    +=   (value_size + item_spacing*2);
    }
    {
        ImGui::SameLine(same_line_width);
        // input
        std::vector<std::string> modes = {_u8L("Object"), _u8L("Part")};
        int selection_idx = m_selection.get_volume_selection_mode() == Selection::Instance ? 0 : 1;
        auto label         = _u8L("Selection Mode") + ":" ;
        auto label_width   = imgui->calc_text_size(label).x ;
        auto item_width   = imgui->calc_text_size(_L("Object")).x * 2.5 + imgui->calc_text_size("x").x+imgui->scaled(2);

        //render imgui
        ImGui::AlignTextToFramePadding();
        ImGui::PushItemWidth(label_width);
        imgui->text(label);
        same_line_width += (label_width + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(item_width);
        size_t selection_out = selection_idx;
        const char *selected_str = (selection_idx >= 0 && selection_idx < int(modes.size())) ? modes[selection_idx].c_str() : "";
        ImGuiWrapper::push_combo_style(get_scale());
        if (ImGui::BBLBeginCombo(("##" + label).c_str(), selected_str, 0)) {
            for (size_t line_idx = 0; line_idx < modes.size(); ++line_idx) {
                ImGui::PushID(int(line_idx));
                if (ImGui::Selectable("", line_idx == selection_idx))
                    selection_out = line_idx;

                ImGui::SameLine();
                ImGui::Text("%s", modes[line_idx].c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGuiWrapper::pop_combo_style();
        if (selection_idx != selection_out) {//do
            if (selection_out == 0) { m_selection.unlock_volume_selection_mode(); }
            m_selection.set_volume_selection_mode(selection_out == 1 ? Selection::Volume : Selection::Instance);
            if (selection_out == 1) { m_selection.lock_volume_selection_mode(); }
        }
        same_line_width += (label_width + item_width);
    }
    imgui->end();

    ImGuiWrapper::pop_toolbar_style();

    //BBS check ratio changed
    if (m_explosion_ratio != GLVolume::explosion_ratio) {
        for (GLVolume* volume : m_volumes.volumes) {
            volume->set_bounding_boxes_as_dirty();
        }
        GLVolume::explosion_ratio = m_explosion_ratio;
    }
}
void GLCanvas3D::_render_assemble_info() const
{
    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        return;
    }

    if (m_selection.is_empty()) {
        return;
    }

    ImGuiWrapper* imgui = wxGetApp().imgui();
    auto canvas_w = float(get_canvas_size().get_width());
    auto canvas_h = float(get_canvas_size().get_height());
    float space_size = imgui->get_style_scaling() * 8.0f;
    float caption_max = imgui->calc_text_size(_L("Total Volume:")).x + 3 * space_size;

    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = io.Fonts->Fonts[0];
    float origScale = font->Scale;
    font->Scale = 1.2;
    ImGui::PushFont(font);
    ImGui::PopFont();
    float margin = 10.0f * get_scale();
    imgui->set_next_window_pos(canvas_w - margin, canvas_h - margin, ImGuiCond_Always, 1.0f, 1.0f);
    ImGuiWrapper::push_toolbar_style(get_scale());
    imgui->begin(_L("Assembly Info"), ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    font->Scale = origScale;
    ImGui::PushFont(font);
    ImGui::PopFont();

    double size0 = m_selection.get_bounding_box().size()(0);
    double size1 = m_selection.get_bounding_box().size()(1);
    double size2 = m_selection.get_bounding_box().size()(2);
    if (!m_selection.is_empty()) {
        ImGui::Text(_L("Volume:").ToUTF8()); ImGui::SameLine(caption_max);
        ImGui::Text("%.2f", size0 * size1 * size2);
        ImGui::Text(_L("Size:").ToUTF8()); ImGui::SameLine(caption_max);
        ImGui::Text("%.2f x %.2f x %.2f", size0, size1, size2);
    }
    imgui->end();
    ImGuiWrapper::pop_toolbar_style();
}

#if ENABLE_SHOW_CAMERA_TARGET
void GLCanvas3D::_render_camera_target() const
{
    const auto& p_flat_shader = wxGetApp().get_shader("flat");
    if (!p_flat_shader)
        return;

    double half_length = 5.0;

    if (!m_camera_target_mark.is_initialized()) {
        GLModel::Geometry geo;
        geo.format.type = GLModel::PrimitiveType::Lines;
        geo.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3;

        // x
        geo.add_vertex(Vec3f{ -0.5f, 0.0f, 0.0f });
        geo.add_vertex(Vec3f{ 0.5f, 0.0f, 0.0f });

        geo.add_line(0, 1);

        m_camera_target_mark.init_from(std::move(geo));
    }

    wxGetApp().bind_shader(p_flat_shader);

    const Camera& camera = get_active_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& proj_matrix = camera.get_projection_matrix();

    glsafe(::glDisable(GL_DEPTH_TEST));

    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    p_ogl_manager.set_line_width(2.0f);

    const Vec3d& target = get_active_camera().get_target();

    const float scale = 2.0f * half_length;
    Transform3d model_matrix{ Transform3d::Identity() };
    model_matrix.data()[3 * 4 + 0] = target.x();
    model_matrix.data()[3 * 4 + 1] = target.y();
    model_matrix.data()[3 * 4 + 2] = target.z();
    model_matrix.data()[0 * 4 + 0] = scale;
    model_matrix.data()[1 * 4 + 1] = 1.0f;
    model_matrix.data()[2 * 4 + 2] = 1.0f;

    p_flat_shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    p_flat_shader->set_uniform("projection_matrix", proj_matrix);
    m_camera_target_mark.set_color({ 1.0f, 0.0f, 0.0f, 1.0f });
    m_camera_target_mark.render_geometry();

    model_matrix = Geometry::translation_transform(target.cast<double>())
        * Geometry::rotation_transform({ 0.0f, 0.0f, 0.5 * PI })
        * Geometry::scale_transform({ scale, 1.0f, 1.0f });

    p_flat_shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    m_camera_target_mark.set_color({ 0.0f, 1.0f, 0.0f, 1.0f });
    m_camera_target_mark.render_geometry();

    model_matrix = Geometry::translation_transform(target.cast<double>())
        * Geometry::rotation_transform({ 0.0f, -0.5 * PI, 0.0f })
        * Geometry::scale_transform({ scale, 1.0f, 1.0f });

    p_flat_shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    m_camera_target_mark.set_color({ 0.0f, 0.0f, 1.0f, 1.0f });
    m_camera_target_mark.render_geometry();

    wxGetApp().unbind_shader();
}
#endif // ENABLE_SHOW_CAMERA_TARGET

void GLCanvas3D::_render_sla_slices()
{
    if (!m_use_clipping_planes || current_printer_technology() != ptSLA)
        return;

    const auto& shader = wxGetApp().get_shader("flat");
    if (!shader) {
        return;
    }

    const SLAPrint* print = this->sla_print();
    const PrintObjects& print_objects = print->objects();
    if (print_objects.empty())
        // nothing to render, return
        return;

    wxGetApp().bind_shader(shader);

    double clip_min_z = -m_clipping_planes[0].get_data()[3];
    double clip_max_z = m_clipping_planes[1].get_data()[3];
    for (unsigned int i = 0; i < (unsigned int)print_objects.size(); ++i) {
        const SLAPrintObject* obj = print_objects[i];

        if (!obj->is_step_done(slaposSliceSupports))
            continue;

        SlaCap::ObjectIdToModelsMap::iterator it_caps_bottom = m_sla_caps[0].triangles.find(i);
        SlaCap::ObjectIdToModelsMap::iterator it_caps_top = m_sla_caps[1].triangles.find(i);
        {
            if (it_caps_bottom == m_sla_caps[0].triangles.end())
                it_caps_bottom = m_sla_caps[0].triangles.emplace(i, SlaCap::Triangles()).first;
            if (!m_sla_caps[0].matches(clip_min_z)) {
                m_sla_caps[0].z = clip_min_z;
                it_caps_bottom->second.object.reset();
                it_caps_bottom->second.supports.reset();
            }
            if (it_caps_top == m_sla_caps[1].triangles.end())
                it_caps_top = m_sla_caps[1].triangles.emplace(i, SlaCap::Triangles()).first;
            if (!m_sla_caps[1].matches(clip_max_z)) {
                m_sla_caps[1].z = clip_max_z;
                it_caps_top->second.object.reset();
                it_caps_top->second.supports.reset();
            }
        }
        GLModel& bottom_obj_triangles = it_caps_bottom->second.object;
        GLModel& bottom_sup_triangles = it_caps_bottom->second.supports;
        GLModel& top_obj_triangles = it_caps_top->second.object;
        GLModel& top_sup_triangles = it_caps_top->second.supports;

        auto init_model = [](GLModel& model, const Pointf3s& triangles, const ColorRGBA& color) {
            GLModel::Geometry init_data;
            init_data.format = { GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
            init_data.reserve_vertices(triangles.size());
            init_data.reserve_indices(triangles.size() / 3);
            init_data.color = color;

            unsigned int vertices_count = 0;
            for (const Vec3d& v : triangles) {
                init_data.add_vertex((Vec3f)v.cast<float>());
                ++vertices_count;
                if (vertices_count % 3 == 0) {
                    init_data.add_triangle(vertices_count - 3, vertices_count - 2, vertices_count - 1);
                }
            }

            if (!init_data.is_empty())
                model.init_from(std::move(init_data));
            };

        if ((!bottom_obj_triangles.is_initialized() || !bottom_sup_triangles.is_initialized() ||
            !top_obj_triangles.is_initialized() || !top_sup_triangles.is_initialized()) && !obj->get_slice_index().empty()) {
            double layer_height = print->default_object_config().layer_height.value;
            double initial_layer_height = print->material_config().initial_layer_height.value;
            bool   left_handed = obj->is_left_handed();

            coord_t key_zero = obj->get_slice_index().front().print_level();
            // Slice at the center of the slab starting at clip_min_z will be rendered for the lower plane.
            coord_t key_low = coord_t((clip_min_z - initial_layer_height + layer_height) / SCALING_FACTOR) + key_zero;
            // Slice at the center of the slab ending at clip_max_z will be rendered for the upper plane.
            coord_t key_high = coord_t((clip_max_z - initial_layer_height) / SCALING_FACTOR) + key_zero;

            const SliceRecord& slice_low = obj->closest_slice_to_print_level(key_low, coord_t(SCALED_EPSILON));
            const SliceRecord& slice_high = obj->closest_slice_to_print_level(key_high, coord_t(SCALED_EPSILON));

            // Offset to avoid OpenGL Z fighting between the object's horizontal surfaces and the triangluated surfaces of the cuts.
            double plane_shift_z = 0.002;

            if (slice_low.is_valid()) {
                const ExPolygons& obj_bottom = slice_low.get_slice(soModel);
                const ExPolygons& sup_bottom = slice_low.get_slice(soSupport);
                // calculate model bottom cap
                // calculate model bottom cap
                if (!bottom_obj_triangles.is_initialized() && !obj_bottom.empty())
                    init_model(bottom_obj_triangles, triangulate_expolygons_3d(obj_bottom, clip_min_z - plane_shift_z, !left_handed), { 1.0f, 0.37f, 0.0f, 1.0f });
                // calculate support bottom cap
                if (!bottom_sup_triangles.is_initialized() && !sup_bottom.empty())
                    init_model(bottom_sup_triangles, triangulate_expolygons_3d(sup_bottom, clip_min_z - plane_shift_z, !left_handed), { 1.0f, 0.0f, 0.37f, 1.0f });
            }

            if (slice_high.is_valid()) {
                const ExPolygons& obj_top = slice_high.get_slice(soModel);
                const ExPolygons& sup_top = slice_high.get_slice(soSupport);
                // calculate model top cap
                // calculate model top cap
                if (!top_obj_triangles.is_initialized() && !obj_top.empty())
                    init_model(top_obj_triangles, triangulate_expolygons_3d(obj_top, clip_max_z + plane_shift_z, left_handed), { 1.0f, 0.37f, 0.0f, 1.0f });
                // calculate support top cap
                if (!top_sup_triangles.is_initialized() && !sup_top.empty())
                    init_model(top_sup_triangles, triangulate_expolygons_3d(sup_top, clip_max_z + plane_shift_z, left_handed), { 1.0f, 0.0f, 0.37f, 1.0f });
            }
        }

        for (const SLAPrintObject::Instance& inst : obj->instances()) {
            const Camera& camera = get_active_camera();
            const Transform3d view_model_matrix = camera.get_view_matrix() *
                Geometry::assemble_transform(Vec3d(unscale<double>(inst.shift.x()), unscale<double>(inst.shift.y()), 0.0),
                    inst.rotation * Vec3d::UnitZ(), Vec3d::Ones(),
                    obj->is_left_handed() ? Vec3d(-1.0f, 1.0f, 1.0f) : Vec3d::Ones());

            shader->set_uniform("view_model_matrix", view_model_matrix);
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());

            bottom_obj_triangles.render_geometry();
            top_obj_triangles.render_geometry();
            bottom_sup_triangles.render_geometry();
            top_sup_triangles.render_geometry();

        }
    }

    wxGetApp().unbind_shader();
}

//Orio: areas of a support layer for the display: its islands, else the areas of its extrusions
// (the BBS tree supports fill the islands of the brim / skirt layers only).
static ExPolygons sliced_support_areas(const SupportLayer &layer)
{
    if (! layer.support_islands.empty())
        return layer.support_islands;
    Polygons polygons = layer.support_fills.polygons_covered_by_width(float(SCALED_EPSILON));
    return polygons.empty() ? ExPolygons() : closing_ex(polygons, float(scale_(0.4)));
}

//Orio: CIELAB coordinates of an sRGB color, to compare the colors as they are perceived.
static Vec3f sliced_supports_lab(const ColorRGB &color)
{
    auto linear = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    auto f      = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    const float r  = linear(color.r());
    const float g  = linear(color.g());
    const float b  = linear(color.b());
    // XYZ relative to the D65 white.
    const float fx = f((0.4124f * r + 0.3576f * g + 0.1805f * b) / 0.95047f);
    const float fy = f(0.2126f * r + 0.7152f * g + 0.0722f * b);
    const float fz = f((0.0193f * r + 0.1192f * g + 0.9505f * b) / 1.08883f);
    return Vec3f(116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz));
}

//Orio: perceived colors (CIELAB) of the filaments of an object: parts, painted areas, filaments of the supports.
// While a gizmo is open (painting), the painted filaments are those known by the volumes: not computed again at each frame.
static std::vector<Vec3f> object_filaments_lab(const ModelObject &model_object, const DynamicPrintConfig *config, bool gizmo_running)
{
    // Filaments of the object, numbered from 1.
    std::vector<int> filaments;
    for (const ModelVolume *volume : model_object.volumes) {
        if (volume->is_model_part())
            filaments.push_back(volume->extruder_id());
        append(filaments, gizmo_running ? volume->mmuseg_extruders : volume->get_extruders());
    }
    for (const char *key : { "support_filament", "support_interface_filament" }) {
        const ConfigOption *opt = model_object.config.option(key);
        if (opt == nullptr && config != nullptr)
            opt = config->option(key);
        // 0: the filament of the object.
        if (opt != nullptr && opt->getInt() > 0)
            filaments.push_back(opt->getInt());
    }

    std::vector<Vec3f> used;
    if (const ConfigOptionStrings *colors = config != nullptr ? config->option<ConfigOptionStrings>("filament_colour") : nullptr)
        for (int filament : filaments) {
            // As in the 3D scene, a filament out of range is shown with the color of the first one.
            const size_t idx = filament >= 1 && size_t(filament) <= colors->values.size() ? size_t(filament - 1) : 0;
            ColorRGB     color;
            if (idx < colors->values.size() && decode_color(colors->values[idx], color))
                used.push_back(sliced_supports_lab(color));
        }
    return used;
}

//Orio: the first preferred color far enough from all the used colors (CIELAB), else the farthest one.
static ColorRGB contrasting_color(const std::vector<ColorRGB> &preferred, const std::vector<Vec3f> &used)
{
    // Perceived difference (CIE76) from which two colors are easily told apart and, for a colored filament (not white, grey
    // or black), the hue difference also required: a dark green and a light green are both green.
    const float min_difference     = 40.f;
    const float min_chroma         = 20.f;
    const float min_hue_difference = float(0.25 * M_PI);

    auto far_enough = [&](const Vec3f &lab, const Vec3f &filament_lab) {
        if ((lab - filament_lab).norm() < min_difference)
            return false;
        if (std::hypot(filament_lab.y(), filament_lab.z()) < min_chroma)
            return true;
        const float hue_difference = std::remainder(std::atan2(lab.z(), lab.y()) - std::atan2(filament_lab.z(), filament_lab.y()), float(2. * M_PI));
        return std::abs(hue_difference) >= min_hue_difference;
    };

    const ColorRGB *best            = &preferred.front();
    float           best_difference = -1.f;
    for (const ColorRGB &candidate : preferred) {
        const Vec3f lab = sliced_supports_lab(candidate);
        if (std::all_of(used.begin(), used.end(), [&](const Vec3f &filament_lab) { return far_enough(lab, filament_lab); }))
            return candidate;
        // Too many filaments: the color with the largest perceived difference.
        float difference = std::numeric_limits<float>::max();
        for (const Vec3f &filament_lab : used)
            difference = std::min(difference, (lab - filament_lab).norm());
        if (difference > best_difference) {
            best_difference = difference;
            best            = &candidate;
        }
    }
    return *best;
}

//Orio: color of the supports of an object in the Prepare view, which stands out from the colors of its filaments
// (parts, painted areas, filaments of the supports).
static ColorRGB sliced_supports_color(const ModelObject &model_object, const DynamicPrintConfig *config, bool gizmo_running)
{
    static const std::vector<ColorRGB> preferred = {
        ColorRGB(0.00f, 0.68f, 0.26f), // green
        ColorRGB(0.90f, 0.10f, 0.80f), // magenta
        ColorRGB(0.00f, 0.80f, 1.00f), // cyan
        ColorRGB(1.00f, 0.55f, 0.00f), // orange
        ColorRGB(0.20f, 0.40f, 1.00f), // blue
        ColorRGB(1.00f, 0.90f, 0.10f), // yellow
        ColorRGB(0.60f, 0.25f, 1.00f), // violet
        ColorRGB(0.95f, 0.15f, 0.15f)  // red
    };
    return contrasting_color(preferred, object_filaments_lab(model_object, config, gizmo_running));
}

//Orio: colors of the modifiers, in order of preference.
static const std::vector<ColorRGB> MODIFIERS_COLORS = {
    ColorRGB(1.00f, 1.00f, 0.00f), // yellow, as GLVolume::MODEL_MIDIFIER_COL
    ColorRGB(0.90f, 0.10f, 0.80f), // magenta
    ColorRGB(0.00f, 0.80f, 1.00f), // cyan
    ColorRGB(1.00f, 0.55f, 0.00f), // orange
    ColorRGB(0.60f, 0.25f, 1.00f), // violet
    ColorRGB(0.00f, 0.68f, 0.26f), // green
    // last: the colors of the support blockers / enforcers
    ColorRGB(0.20f, 0.40f, 1.00f), // blue
    ColorRGB(0.95f, 0.15f, 0.15f)  // red
};

//Orio: color of the modifiers of an object in the Prepare view: their usual yellow if it stands out from the
// filaments of the object, else another color standing out, chosen as the one of the sliced supports; never the color of
// the sliced supports of this object (nor a close one), shown or not.
static ColorRGB modifiers_color(const ModelObject &model_object, const DynamicPrintConfig *config, bool gizmo_running)
{
    const ColorRGB      supports = sliced_supports_color(model_object, config, gizmo_running);
    std::vector<Vec3f>  used     = object_filaments_lab(model_object, config, gizmo_running);
    used.push_back(sliced_supports_lab(supports));
    const ColorRGB color = contrasting_color(MODIFIERS_COLORS, used);
    if (color.r() != supports.r() || color.g() != supports.g() || color.b() != supports.b())
        return color;
    // Only if every candidate is the color of a filament: the first one which is not the color of the supports.
    for (const ColorRGB &candidate : MODIFIERS_COLORS)
        if (candidate.r() != supports.r() || candidate.g() != supports.g() || candidate.b() != supports.b())
            return candidate;
    return color;
}

//Orio: the own color of a modifier, chosen at its creation or in the Modifiers tool (ModifierPlacement::color).
static std::optional<ColorRGB> modifier_own_color(const ModelVolume &volume)
{
    ColorRGB color;
    if (volume.modifier_placement.has_value() && ! volume.modifier_placement->color.empty() && decode_color(volume.modifier_placement->color, color))
        return color;
    return std::nullopt;
}

ColorRGB GLCanvas3D::color_standing_out(const ModelObject &model_object, const std::vector<ColorRGB> &preferred) const
{
    return contrasting_color(preferred, object_filaments_lab(model_object, m_config, m_gizmos.is_running()));
}

//Orio: as the color of the modifiers (see modifiers_color()), also standing out from the colors of the modifiers of
// the object already there (their own colors, else the usual one).
ColorRGB GLCanvas3D::new_modifier_color(const ModelObject &model_object) const
{
    const bool         gizmo_running = m_gizmos.is_running();
    const ColorRGB     usual         = modifiers_color(model_object, m_config, gizmo_running);
    std::vector<Vec3f> used          = object_filaments_lab(model_object, m_config, gizmo_running);
    used.push_back(sliced_supports_lab(sliced_supports_color(model_object, m_config, gizmo_running)));
    for (const ModelVolume *volume : model_object.volumes)
        if (volume->is_modifier())
            used.push_back(sliced_supports_lab(modifier_own_color(*volume).value_or(usual)));
    return contrasting_color(MODIFIERS_COLORS, used);
}

//Orio: modifiers of the Prepare view in their own color (chosen at their creation or in the Modifiers tool), else
// in the color standing out from the filaments of their object (see modifiers_color()), updated at each frame as the color
// of the sliced supports: it follows the filaments of the parts, their painting and the colors of the filaments. Support
// blockers / enforcers and negative volumes keep their usual colors.
//Orio: support enforcers generating tree supports in green (as the tree painting), the others in their usual blue,
// updated at each frame: they follow the kind of supports chosen for them and the support type of their object.
void GLCanvas3D::_update_support_enforcers_colors()
{
    if (m_model == nullptr)
        return;
    static const std::array<float, 4> tree_enforcer_color { 0.3f, 0.8f, 0.3f, 0.4f };
    for (GLVolume *volume : m_volumes.volumes) {
        if (volume == nullptr || ! volume->is_modifier)
            continue;
        const int object_idx = volume->object_idx();
        const int volume_idx = volume->volume_idx();
        if (object_idx < 0 || object_idx >= int(m_model->objects.size()))
            continue;
        const ModelObject *model_object = m_model->objects[object_idx];
        if (volume_idx < 0 || volume_idx >= int(model_object->volumes.size()) || ! model_object->volumes[volume_idx]->is_support_enforcer())
            continue;
        const ConfigOption *support_type = model_object->config.get().option("support_type");
        if (support_type == nullptr && m_config != nullptr)
            support_type = m_config->option("support_type");
        const bool tree = model_object->volumes[volume_idx]->generates_tree_supports(support_type != nullptr ? SupportType(support_type->getInt()) : stNormalAuto);
        volume->set_color(tree ? tree_enforcer_color : GLVolume::SUPPORT_ENFORCER_COL);
    }
}

void GLCanvas3D::_update_modifiers_colors()
{
    if (m_model == nullptr)
        return;
    std::map<int, std::array<float, 4>> object_colors;
    for (GLVolume *volume : m_volumes.volumes) {
        if (volume == nullptr || ! volume->is_modifier)
            continue;
        const int object_idx = volume->object_idx();
        const int volume_idx = volume->volume_idx();
        if (object_idx < 0 || object_idx >= int(m_model->objects.size()))
            continue;
        const ModelObject *model_object = m_model->objects[object_idx];
        if (volume_idx < 0 || volume_idx >= int(model_object->volumes.size()) || ! model_object->volumes[volume_idx]->is_modifier())
            continue;
        if (const std::optional<ColorRGB> own = modifier_own_color(*model_object->volumes[volume_idx])) {
            volume->set_color({ own->r(), own->g(), own->b(), GLVolume::MODEL_MIDIFIER_COL[3] });
            continue;
        }
        auto it = object_colors.find(object_idx);
        if (it == object_colors.end()) {
            const ColorRGB color = modifiers_color(*model_object, m_config, m_gizmos.is_running());
            it = object_colors.emplace(object_idx, std::array<float, 4>{ color.r(), color.g(), color.b(), GLVolume::MODEL_MIDIFIER_COL[3] }).first;
        }
        volume->set_color(it->second);
    }
}

//Orio: color of the contour drawn on the bed around an object sunk below it (white before, hardly seen on a light
// filament): chosen as the one of the sliced supports, the first color of the list standing out from the filaments of the
// object. Bright colors first: the contour is also seen against the bed.
static ColorRGB sinking_contour_color(const ModelObject &model_object, const DynamicPrintConfig *config, bool gizmo_running)
{
    static const std::vector<ColorRGB> preferred = {
        ColorRGB(0.00f, 0.80f, 1.00f), // cyan
        ColorRGB(0.90f, 0.10f, 0.80f), // magenta
        ColorRGB(1.00f, 0.55f, 0.00f), // orange
        ColorRGB(0.00f, 0.68f, 0.26f), // green
        ColorRGB(1.00f, 0.90f, 0.10f), // yellow
        ColorRGB(0.95f, 0.15f, 0.15f), // red
        ColorRGB(0.60f, 0.25f, 1.00f), // violet
        ColorRGB(0.20f, 0.40f, 1.00f)  // blue
    };
    return contrasting_color(preferred, object_filaments_lab(model_object, config, gizmo_running));
}

//Orio: updated at each frame as the colors of the modifiers: they follow the filaments, their painting and colors.
void GLCanvas3D::_update_sinking_contours_colors()
{
    if (m_model == nullptr)
        return;
    std::map<int, std::array<float, 4>> object_colors;
    for (GLVolume *volume : m_volumes.volumes) {
        if (volume == nullptr || ! volume->is_sinking())
            continue;
        const int object_idx = volume->object_idx();
        if (object_idx < 0 || object_idx >= int(m_model->objects.size()))
            continue;
        auto it = object_colors.find(object_idx);
        if (it == object_colors.end()) {
            const ColorRGB color = sinking_contour_color(*m_model->objects[object_idx], m_config, m_gizmos.is_running());
            it = object_colors.emplace(object_idx, std::array<float, 4>{ color.r(), color.g(), color.b(), 1.f }).first;
        }
        volume->sinking_contour_color = it->second;
    }
}

//Orio: color of the interface of the supports, another shade of their color: lighter, darker if it is already light.
static ColorRGB sliced_supports_interface_color(const ColorRGB &color)
{
    const float luminance = 0.299f * color.r() + 0.587f * color.g() + 0.114f * color.b();
    return luminance > 0.6f ? color * 0.55f : color * 0.5f + ColorRGB(0.5f, 0.5f, 0.5f);
}

//Orio: segments of an extrusion of the supports, see GLCanvas3D::SlicedSupports::Object::segments.
static void sliced_support_path_segments(const ExtrusionPath &path, float top_z, std::vector<float> &segments)
{
    // Simplified: the display does not need details smaller than the width of the extrusions.
    const Points points = MultiPoint::_douglas_peucker(path.polyline.points, scale_(0.05));
    // The interface is marked by a negative width.
    const float  width  = path.role() == erSupportMaterialInterface ? - path.width : path.width;
    for (size_t i = 1; i < points.size(); ++ i)
        segments.insert(segments.end(), { float(unscale<double>(points[i - 1].x())), float(unscale<double>(points[i - 1].y())), top_z, width,
                                          float(unscale<double>(points[i].x())), float(unscale<double>(points[i].y())), top_z, path.height });
}

static void sliced_support_segments(const ExtrusionEntity &entity, float top_z, std::vector<float> &segments)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            sliced_support_segments(*child, top_z, segments);
    } else if (const auto *multi_path = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath &path : multi_path->paths)
            sliced_support_path_segments(path, top_z, segments);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            sliced_support_path_segments(path, top_z, segments);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity))
        sliced_support_path_segments(*path, top_z, segments);
}

GLCanvas3D::SlicedSupports::Object::~Object()
{
    if (segments_vbo > 0)
        glsafe(::glDeleteBuffers(1, &segments_vbo));
}

bool GLCanvas3D::_read_sliced_supports()
{
    for (std::unique_ptr<SlicedSupports::Object> &object : m_sliced_supports.objects)
        m_sliced_supports.released.emplace_back(std::move(object));
    m_sliced_supports.objects.clear();

    // Each plate keeps the result of its last slicing, as long as the changes since do not invalidate the supports.
    PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
    for (int plate_idx = 0; plate_idx < plate_list.get_plate_count(); ++ plate_idx) {
        const Print *print = plate_list.get_plate(plate_idx)->fff_print();
        if (print == nullptr)
            continue;
        for (const PrintObject *print_object : print->objects()) {
            if (! print_object->is_step_done(posSupportMaterial) || print_object->support_layers().empty())
                continue;
            auto object = std::make_unique<SlicedSupports::Object>();
            object->model_object_id = print_object->model_object()->id();
            object->plate_idx       = plate_idx;
            // The objects stand on the print bed in the Prepare view: the raft is left out.
            const double z_min = print_object->slicing_parameters().object_print_z_min;
            for (const SupportLayer *support_layer : print_object->support_layers()) {
                if (support_layer->print_z < z_min + EPSILON)
                    continue;
                const float top_z   = float(support_layer->print_z - z_min);
                ExPolygons  islands = sliced_support_areas(*support_layer);
                if (! islands.empty())
                    object->layers.push_back({ float(std::max(0., support_layer->bottom_z() - z_min)), top_z, std::move(islands) });
                sliced_support_segments(support_layer->support_fills, top_z, object->segments);
            }
            if (object->layers.empty() && object->segments.empty())
                continue;
            // Matrices of the model of the print, in line with the shifts of its instances.
            const ModelVolume *reference = nullptr;
            for (const ModelVolume *volume : print_object->model_object()->volumes)
                if (volume->is_model_part()) {
                    reference = volume;
                    break;
                }
            if (reference != nullptr)
                object->reference_volume_id = reference->id();
            for (const PrintInstance &instance : print_object->instances())
                object->instances.push_back({ instance.model_instance->id(),
                                              reference != nullptr ? Transform3d(instance.model_instance->get_matrix() * reference->get_matrix()) :
                                                                     instance.model_instance->get_matrix(),
                                              Vec2d(unscale<double>(instance.shift.x()), unscale<double>(instance.shift.y())) });
            m_sliced_supports.objects.emplace_back(std::move(object));
        }
    }
    set_as_dirty();
    return ! m_sliced_supports.objects.empty();
}

void GLCanvas3D::reset_sliced_supports()
{
    // Turned off by each slicing, read again when turned on.
    m_sliced_supports.show = false;
    for (std::unique_ptr<SlicedSupports::Object> &object : m_sliced_supports.objects)
        m_sliced_supports.released.emplace_back(std::move(object));
    m_sliced_supports.objects.clear();
    // It can be turned on if a plate has sliced supports.
    m_sliced_supports.available = false;
    PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
    for (int plate_idx = 0; plate_idx < plate_list.get_plate_count() && ! m_sliced_supports.available; ++ plate_idx)
        if (const Print *print = plate_list.get_plate(plate_idx)->fff_print())
            for (const PrintObject *print_object : print->objects())
                if (print_object->is_step_done(posSupportMaterial) && ! print_object->support_layers().empty()) {
                    m_sliced_supports.available = true;
                    break;
                }
    set_as_dirty();
}

void GLCanvas3D::show_sliced_supports(bool show)
{
    // Read when turned on for the first time since the last slicing, then kept until the next slicing.
    if (show && m_sliced_supports.objects.empty()) {
        if (wxGetApp().plater()->is_background_process_slicing()) {
            wxGetApp().plater()->get_notification_manager()->push_notification(_u8L("Wait for the end of the slicing to show the sliced supports."));
            show = false;
        } else if (! _read_sliced_supports()) {
            wxGetApp().plater()->get_notification_manager()->push_notification(_u8L("No sliced supports to show: slice the plate again."));
            m_sliced_supports.available = false;
            show = false;
        }
    }
    m_sliced_supports.show = show;
    set_as_dirty();
}

void GLCanvas3D::_render_sliced_supports()
{
    // Free the OpenGL buffers of the replaced supports, the context is current here.
    m_sliced_supports.released.clear();
    if (! m_sliced_supports.show || m_sliced_supports.objects.empty() || m_model == nullptr)
        return;

    // Sent once to the graphics card, without keeping a copy in the main memory: the areas of the layers (a prism per run of
    // layers with the same areas, drawn pale where the objects hide the supports) and the segments of the extrusions.
    for (std::unique_ptr<SlicedSupports::Object> &object : m_sliced_supports.objects) {
        if (! object->layers.empty()) {
            std::vector<SlicedSupports::Layer> &layers = object->layers;
            // Only a pale silhouette: simplified areas, one layer out of n if there are too many edges.
            size_t num_edges = 0;
            for (SlicedSupports::Layer &layer : layers) {
                layer.islands = expolygons_simplify(layer.islands, scale_(0.1));
                for (const ExPolygon &expoly : layer.islands) {
                    num_edges += expoly.contour.size();
                    for (const Polygon &hole : expoly.holes)
                        num_edges += hole.size();
                }
            }
            const size_t max_edges = 200000;
            const size_t stride    = std::max<size_t>(1, (num_edges + max_edges - 1) / max_edges);

            GLModel::InitializationData::Entity faces;
            faces.type = GLModel::PrimitiveType::Triangles;
            auto add_vertex = [&faces](const Vec3f &position, const Vec3f &normal) {
                faces.positions.push_back(position);
                faces.normals.push_back(normal);
            };
            auto add_triangle = [&faces](unsigned int a, unsigned int b, unsigned int c) { faces.indices.insert(faces.indices.end(), { a, b, c }); };
            auto add_prism = [&](const ExPolygons &islands, float z_bottom, float z_top) {
                // Walls. Contours are counter-clockwise, holes clockwise: the outer side is on the right.
                for (const ExPolygon &expoly : islands)
                    for (size_t i = 0; i <= expoly.holes.size(); ++ i) {
                        const Points &pts = i == 0 ? expoly.contour.points : expoly.holes[i - 1].points;
                        for (size_t j = 0; j < pts.size(); ++ j) {
                            const Point &pa = pts[j];
                            const Point &pb = pts[(j + 1) % pts.size()];
                            const Vec2f  a(float(unscale<double>(pa.x())), float(unscale<double>(pa.y())));
                            const Vec2f  b(float(unscale<double>(pb.x())), float(unscale<double>(pb.y())));
                            const Vec2f  d      = b - a;
                            const float  length = d.norm();
                            if (length < 1e-4f)
                                continue;
                            const Vec3f        normal(d.y() / length, - d.x() / length, 0.f);
                            const unsigned int v = (unsigned int)faces.positions.size();
                            add_vertex(Vec3f(a.x(), a.y(), z_bottom), normal);
                            add_vertex(Vec3f(b.x(), b.y(), z_bottom), normal);
                            add_vertex(Vec3f(b.x(), b.y(), z_top), normal);
                            add_vertex(Vec3f(a.x(), a.y(), z_top), normal);
                            add_triangle(v, v + 1, v + 2);
                            add_triangle(v, v + 2, v + 3);
                        }
                    }
                // Bottom and top.
                for (const bool top : { false, true }) {
                    const std::vector<Vec3d> triangles = triangulate_expolygons_3d(islands, top ? z_top : z_bottom, ! top);
                    const Vec3f              normal(0.f, 0.f, top ? 1.f : -1.f);
                    const unsigned int       v = (unsigned int)faces.positions.size();
                    for (const Vec3d &p : triangles)
                        add_vertex((Vec3f)p.cast<float>(), normal);
                    for (unsigned int i = 0; i + 2 < (unsigned int)triangles.size(); i += 3)
                        add_triangle(v + i, v + i + 1, v + i + 2);
                }
            };
            for (size_t i = 0; i < layers.size();) {
                // One prism for the next layers with the same areas, at least for stride layers.
                size_t j = i + 1;
                while (j < layers.size() && (j - i < stride || layers[j].islands == layers[i].islands))
                    ++ j;
                add_prism(layers[i].islands, layers[i].bottom_z, layers[j - 1].top_z);
                i = j;
            }
            layers.clear();
            layers.shrink_to_fit();
            if (! faces.indices.empty()) {
                GLModel::InitializationData data;
                data.entities.push_back(std::move(faces));
                object->model.init_from(data);
            }
        }
        if (! object->segments.empty()) {
            // Two segments per instance: an even number of them, the last one may be empty.
            if (object->segments.size() % 16 != 0)
                object->segments.resize(object->segments.size() + 8, 0.f);
            glsafe(::glGenBuffers(1, &object->segments_vbo));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, object->segments_vbo));
            glsafe(::glBufferData(GL_ARRAY_BUFFER, object->segments.size() * sizeof(float), object->segments.data(), GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
            object->segments_instances = (unsigned int)(object->segments.size() / 16);
            object->segments.clear();
            object->segments.shrink_to_fit();
        }
    }
    if (! m_sliced_supports.box.is_initialized()) {
        // Two boxes: x in [0, 1] and [2, 3] along the segment, y in [-0.5, 0.5] across, z in [-1, 0] below the top of the
        // layer. Closed: seen through the cut of the Prepare tab, their back faces are the sections of the extrusions.
        GLModel::InitializationData::Entity box;
        box.type = GLModel::PrimitiveType::Triangles;
        auto add_face = [&box](const Vec3f &normal, std::initializer_list<Vec3f> corners) {
            const unsigned int v = (unsigned int)box.positions.size();
            for (const Vec3f &corner : corners) {
                box.positions.push_back(corner);
                box.normals.push_back(normal);
            }
            box.indices.insert(box.indices.end(), { v, v + 1, v + 2, v, v + 2, v + 3 });
        };
        for (const float x0 : { 0.f, 2.f }) {
            const float x1 = x0 + 1.f;
            add_face(Vec3f(0.f, 0.f, 1.f), { Vec3f(x0, -0.5f, 0.f), Vec3f(x1, -0.5f, 0.f), Vec3f(x1, 0.5f, 0.f), Vec3f(x0, 0.5f, 0.f) });
            add_face(Vec3f(0.f, 0.f, -1.f), { Vec3f(x0, 0.5f, -1.f), Vec3f(x1, 0.5f, -1.f), Vec3f(x1, -0.5f, -1.f), Vec3f(x0, -0.5f, -1.f) });
            add_face(Vec3f(0.f, 1.f, 0.f), { Vec3f(x1, 0.5f, -1.f), Vec3f(x0, 0.5f, -1.f), Vec3f(x0, 0.5f, 0.f), Vec3f(x1, 0.5f, 0.f) });
            add_face(Vec3f(0.f, -1.f, 0.f), { Vec3f(x0, -0.5f, -1.f), Vec3f(x1, -0.5f, -1.f), Vec3f(x1, -0.5f, 0.f), Vec3f(x0, -0.5f, 0.f) });
            add_face(Vec3f(1.f, 0.f, 0.f), { Vec3f(x1, -0.5f, -1.f), Vec3f(x1, 0.5f, -1.f), Vec3f(x1, 0.5f, 0.f), Vec3f(x1, -0.5f, 0.f) });
            add_face(Vec3f(-1.f, 0.f, 0.f), { Vec3f(x0, 0.5f, -1.f), Vec3f(x0, -0.5f, -1.f), Vec3f(x0, -0.5f, 0.f), Vec3f(x0, 0.5f, 0.f) });
        }
        GLModel::InitializationData data;
        data.entities.push_back(std::move(box));
        m_sliced_supports.box.init_from(data);
    }

    // What to draw: a color standing out from the filaments of the object, the matrices of the instances (the supports follow
    // the moves of the reference part of the object since the slicing).
    struct Draw
    {
        SlicedSupports::Object  *object;
        ColorRGB                 color;
        std::vector<Transform3d> world_matrices;
    };
    std::vector<Draw> draws;
    for (const std::unique_ptr<SlicedSupports::Object> &object : m_sliced_supports.objects) {
        const ModelObject *model_object = nullptr;
        for (const ModelObject *mo : m_model->objects)
            if (mo->id() == object->model_object_id) {
                model_object = mo;
                break;
            }
        if (model_object == nullptr)
            // Deleted since.
            continue;
        Draw draw { object.get(), sliced_supports_color(*model_object, m_config, m_gizmos.is_running()), {} };
        // Without its reference part (deleted since), the supports stay where they were sliced.
        const ModelVolume *reference = nullptr;
        for (const ModelVolume *volume : model_object->volumes)
            if (volume->id() == object->reference_volume_id) {
                reference = volume;
                break;
            }
        for (const SlicedSupports::Instance &instance : object->instances)
            for (const ModelInstance *model_instance : model_object->instances)
                if (model_instance->id() == instance.model_instance_id) {
                    const Transform3d now = reference != nullptr ? Transform3d(model_instance->get_matrix() * reference->get_matrix()) :
                                                                   instance.slicing_matrix;
                    draw.world_matrices.push_back(now * instance.slicing_matrix.inverse() *
                                                  Geometry::translation_transform(Vec3d(instance.shift.x(), instance.shift.y(), 0.)));
                    break;
                }
        if (! draw.world_matrices.empty())
            draws.push_back(std::move(draw));
    }
    if (draws.empty())
        return;

    const Camera &camera     = get_active_camera();
    GLint         depth_func = GL_LESS;
    glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_func));
    // The print bed, drawn just before, leaves the depth test off: without it, the supports would be drawn over the objects.
    const bool depth_test = ::glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    glsafe(::glEnable(GL_DEPTH_TEST));
    const bool cull_face = ::glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glCullFace(GL_BACK));
    // The cut X/Y/Z of the Prepare tab (current plate) cuts the supports as the objects.
    const bool cut = m_prepare_cut.is_active();
    // A mirrored instance turns the faces inside out.
    auto set_front_face = [](const Transform3d &world_matrix) {
        glsafe(::glFrontFace(world_matrix.matrix().block<3, 3>(0, 0).determinant() < 0. ? GL_CW : GL_CCW));
    };

    // First the pale silhouette, only where the objects hide the supports (depth greater than theirs). Without the shader of
    // the extrusions (not compiled by the graphics card), the areas are also drawn translucent where they are visible. While
    // the cut is on, it is drawn without lighting by the shader of the selection under the cut, else not at all.
    const bool with_extrusions = wxGetApp().get_shader("support_paths") != nullptr;
    if (const auto &shader = wxGetApp().get_shader(cut ? "flat_clip" : "gouraud_light")) {
        wxGetApp().bind_shader(shader);
        if (cut)
            m_prepare_cut.apply_uniforms(*shader, nullptr);
        else {
            shader->set_uniform("emission_factor", 0.1f);
            shader->set_uniform("u_clip_xy_enabled", 0.0f);
        }
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        glsafe(::glDepthMask(GL_FALSE));
        for (int pass = 0; pass < (with_extrusions ? 1 : 2); ++ pass) {
            const bool hidden = pass == 0;
            glsafe(::glDepthFunc(hidden ? GL_GREATER : GLenum(depth_func)));
            for (const Draw &draw : draws) {
                if (! draw.object->model.is_initialized())
                    continue;
                draw.object->model.set_color(to_rgba(draw.color, hidden ? 0.2f : 0.45f));
                for (const Transform3d &world_matrix : draw.world_matrices) {
                    const Transform3d view_model_matrix = camera.get_view_matrix() * world_matrix;
                    shader->set_uniform("view_model_matrix", view_model_matrix);
                    shader->set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
                    shader->set_uniform("volume_world_matrix", world_matrix);
                    set_front_face(world_matrix);
                    draw.object->model.render_geometry();
                }
            }
        }
        glsafe(::glDepthFunc(GLenum(depth_func)));
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glDisable(GL_BLEND));
        wxGetApp().unbind_shader();
    }

    // Then the extrusions, opaque, with their width and height, clipped and cut as the objects. While the cut is on, their back
    // faces are drawn too: the sections of the cut extrusions.
    if (const auto &shader = wxGetApp().get_shader("support_paths")) {
        wxGetApp().bind_shader(shader);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("z_range", m_volumes.get_z_range());
        shader->set_uniform("clipping_plane", m_volumes.get_clipping_plane());
        m_prepare_cut.apply_uniforms(*shader, nullptr);
        if (cut)
            glsafe(::glDisable(GL_CULL_FACE));
        for (const Draw &draw : draws) {
            if (draw.object->segments_vbo == 0)
                continue;
            m_sliced_supports.box.set_color(to_rgba(draw.color, 1.f));
            shader->set_uniform("interface_color", to_rgba(sliced_supports_interface_color(draw.color), 1.f));
            for (const Transform3d &world_matrix : draw.world_matrices) {
                const Transform3d view_model_matrix = camera.get_view_matrix() * world_matrix;
                shader->set_uniform("view_model_matrix", view_model_matrix);
                shader->set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
                shader->set_uniform("volume_world_matrix", world_matrix);
                set_front_face(world_matrix);
                m_sliced_supports.box.render_geometry_instance(draw.object->segments_vbo, draw.object->segments_instances);
            }
        }
        wxGetApp().unbind_shader();
    }

    glsafe(::glFrontFace(GL_CCW));
    if (cull_face)
        glsafe(::glEnable(GL_CULL_FACE));
    else
        glsafe(::glDisable(GL_CULL_FACE));
    if (! depth_test)
        glsafe(::glDisable(GL_DEPTH_TEST));
}

//Orio: the cut of the Prepare tab is hidden and disabled while a gizmo with its own view of the object
// (cut, painting, text...) or a tool along Z (variable layer height, speed, infill / walls) is open.
bool GLCanvas3D::_is_prepare_cut_allowed() const
{
    if (m_canvas_type != CanvasView3D)
        return false;
    if (is_layers_editing_enabled() || is_speed_editing_enabled() || is_infill_walls_editing_enabled())
        return false;
    const GLGizmosManager::EType gizmo = m_gizmos.get_current_type();
    return gizmo == GLGizmosManager::Undefined || gizmo == GLGizmosManager::Move || gizmo == GLGizmosManager::Rotate ||
           gizmo == GLGizmosManager::Scale;
}

// Orio: the dark background of the ImGui windows (ImGuiWrapper::COL_WINDOW_BG_DARK) with an alpha, for the small
// windows over the scene: they follow the dark theme when it gets darker or lighter.
static ImVec4 theme_dark_window_bg(float alpha)
{
    const ImVec4 &bg = ImGuiWrapper::COL_WINDOW_BG_DARK;
    return ImVec4(bg.x, bg.y, bg.z, alpha);
}

void GLCanvas3D::_render_sliced_supports_toggle()
{
    if (m_canvas_type != CanvasView3D || ! has_sliced_supports() || m_gizmos.is_running())
        return;

    ImGuiWrapper &imgui = *wxGetApp().imgui();
    const float   sc    = get_scale();
    const Size    cnv_size = get_canvas_size();
    // Bottom center of the canvas, above the bar of the variable infill / walls (or speed) tool painting along X / Y.
    const float x = 0.5f * float(cnv_size.get_width());
    const float y = float(cnv_size.get_height()) - 20.f * sc - m_infill_walls_editing.bottom_bar_height(*this) - m_speed_editing.bottom_bar_height(*this);
    imgui.set_next_window_pos(x, y, ImGuiCond_Always, 0.5f, 1.f);

    // Shown supports of a plate modified since it was sliced: they may have changed.
    bool outdated = false;
    if (m_sliced_supports.show) {
        PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
        for (const std::unique_ptr<SlicedSupports::Object> &object : m_sliced_supports.objects) {
            PartPlate *plate = object->plate_idx >= 0 && object->plate_idx < plate_list.get_plate_count() ? plate_list.get_plate(object->plate_idx) : nullptr;
            if (plate == nullptr || ! plate->is_slice_result_valid()) {
                outdated = true;
                break;
            }
        }
    }

    // Orio: framed, its text in the colors of the theme, as the label of the « Hors impression » area (the
    // default text of ImGui is white: unreadable on the light theme).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f * sc);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_is_dark ? theme_dark_window_bg(0.85f) : ImVec4(1.f, 1.f, 1.f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, m_is_dark ? ImVec4(0.75f, 0.75f, 0.75f, 1.f) : ImVec4(0.45f, 0.45f, 0.45f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, m_is_dark ? ImVec4(0.92f, 0.92f, 0.92f, 1.f) : ImVec4(0.16f, 0.16f, 0.16f, 1.f));
    imgui.begin(std::string("sliced_supports_toggle"), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize);
    bool show = m_sliced_supports.show;
    if (imgui.bbl_checkbox(outdated ? _L("Sliced supports (to slice again)") : _L("Sliced supports"), show, true, m_is_dark))
        show_sliced_supports(show);
    if (ImGui::IsItemHovered())
        imgui.tooltip(_L("Show the supports of the last slicing as they will be printed, to place the support modifiers (Ctrl+U). "
                         "Read when checked, unchecked by each slicing. Interface: another shade of the same color. Pale: hidden by the objects."),
                      ImGui::GetFontSize() * 20.f);
    imgui.end();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

//Orio: label of the « Hors impression » area, centered under it, kept in the canvas when the area is out of it.
// The eye shows / hides the area, its name zooms to it.
void GLCanvas3D::_render_parking_zone_toggle()
{
    // Hidden while a gizmo or a tool along Z (variable layer height, speed, infill / walls) is open: it would cover its bar.
    if (m_canvas_type != CanvasView3D || m_gizmos.is_running() || is_layers_editing_enabled() || is_speed_editing_enabled() ||
        is_infill_walls_editing_enabled())
        return;
    PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
    const int      count      = plate_list.parked_instance_count();
    if (count == 0)
        return;

    // An instance of the hidden area selected (in the object list): the area is shown again.
    if (plate_list.is_parking_zone_hidden()) {
        for (unsigned int idx : m_selection.get_volume_idxs()) {
            const GLVolume *volume = m_volumes.volumes[idx];
            if (plate_list.is_parked_instance(volume->object_idx(), volume->instance_idx())) {
                wxGetApp().CallAfter([this]() { set_parking_zone_hidden(false); });
                break;
            }
        }
    }
    const bool hidden = plate_list.is_parking_zone_hidden();

    const BoundingBoxf3 box = plate_list.get_parking_zone_box();
    if (!box.defined)
        return;
    // Centered under the area on the screen: neither on the plates nor on the instances of the area, which it fits
    // (bottom right corner of the canvas when the area is behind the camera).
    const Camera  &camera    = get_active_camera();
    const Size     cnv_size  = get_canvas_size();
    const float    cnv_w     = float(cnv_size.get_width());
    const float    cnv_h     = float(cnv_size.get_height());
    const float    sc        = get_scale();
    const float    margin    = 8.f * sc;
    const Matrix4d view_proj = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    float          x         = cnv_w;
    float          y         = cnv_h;
    {
        float left = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;
        bool  visible = true;
        for (const Vec2d &corner : { Vec2d(box.min.x(), box.min.y()), Vec2d(box.max.x(), box.min.y()), Vec2d(box.max.x(), box.max.y()),
                                     Vec2d(box.min.x(), box.max.y()) }) {
            const Vec4d clip = view_proj * Vec4d(corner.x(), corner.y(), 0., 1.);
            if (clip.w() <= EPSILON) {
                visible = false;
                break;
            }
            const float sx = float((0.5 + 0.5 * clip.x() / clip.w()) * cnv_w);
            const float sy = float((0.5 - 0.5 * clip.y() / clip.w()) * cnv_h);
            left   = std::min(left, sx);
            right  = std::max(right, sx);
            bottom = std::max(bottom, sy);
        }
        if (visible) {
            x = 0.5f * (left + right);
            y = bottom + margin;
        }
    }
    // Kept in the canvas, below the main toolbar (x: its center, y: its top).
    const float half_w  = 0.5f * (m_parking_label_width > 0.f ? m_parking_label_width : 200.f * sc);
    const float label_h = m_parking_label_height > 0.f ? m_parking_label_height : 30.f * sc;
    const float top     = float(get_main_toolbar_height()) + margin;
    x = std::max(margin + half_w, std::min(x, cnv_w - half_w - margin));
    y = std::max(top, std::min(y, cnv_h - label_h - margin));

    ImGuiWrapper &imgui = *wxGetApp().imgui();
    imgui.set_next_window_pos(x, y, ImGuiCond_Always, 0.5f, 0.f);
    // Neutral: the names of the sections have their colors.
    const ImVec4 accent     = m_is_dark ? ImVec4(0.75f, 0.75f, 0.75f, 1.f) : ImVec4(0.45f, 0.45f, 0.45f, 1.f);
    const ImVec4 text_color = m_is_dark ? ImVec4(0.92f, 0.92f, 0.92f, 1.f) : ImVec4(0.16f, 0.16f, 0.16f, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f * sc, 3.f * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * sc, 0.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_is_dark ? theme_dark_window_bg(0.9f) : ImVec4(1.f, 1.f, 1.f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, accent);
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    imgui.begin(std::string("parking_zone_label"), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing);

    // The eye, drawn: open when the area is shown, crossed out when it is hidden.
    const float  eye_size = ImGui::GetFrameHeight();
    const ImVec2 eye_pos  = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##parking_eye", ImVec2(eye_size, eye_size));
    const bool eye_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        wxGetApp().CallAfter([this, hidden]() { set_parking_zone_hidden(!hidden); });
    {
        ImDrawList  *draw  = ImGui::GetWindowDrawList();
        const ImU32  color = ImGui::GetColorU32(eye_hovered ? accent : text_color);
        const ImVec2 c(eye_pos.x + 0.5f * eye_size, eye_pos.y + 0.5f * eye_size);
        const float  rx    = 0.4f * eye_size;
        const float  ry    = 0.24f * eye_size;
        const float  thick = std::max(1.f, 1.4f * sc);
        // Lids: cubic curves whose control points are 4/3 of the height of the eye away.
        draw->AddBezierCurve(ImVec2(c.x - rx, c.y), ImVec2(c.x - 0.45f * rx, c.y - 1.33f * ry), ImVec2(c.x + 0.45f * rx, c.y - 1.33f * ry),
                             ImVec2(c.x + rx, c.y), color, thick);
        draw->AddBezierCurve(ImVec2(c.x - rx, c.y), ImVec2(c.x - 0.45f * rx, c.y + 1.33f * ry), ImVec2(c.x + 0.45f * rx, c.y + 1.33f * ry),
                             ImVec2(c.x + rx, c.y), color, thick);
        if (hidden) {
            draw->AddCircle(c, 0.5f * ry, color, 0, thick);
            draw->AddLine(ImVec2(c.x - 0.85f * rx, c.y + 0.75f * rx), ImVec2(c.x + 0.85f * rx, c.y - 0.75f * rx), color, thick);
        } else
            draw->AddCircleFilled(c, 0.55f * ry, color);
    }
    if (eye_hovered)
        imgui.tooltip(hidden ? _L("Show the \"Not printed\" area.") :
                               _L("Hide the \"Not printed\" area: its objects are no longer shown nor clickable. They stay in the object list."),
                      ImGui::GetFontSize() * 20.f);

    // Name and number of instances of each section, in its color, in the order of the sections: zoom to the area.
    for (const bool printed : { true, false }) {
        const int section_count = plate_list.parked_instance_count(printed);
        if (section_count == 0)
            continue;
        const ColorRGBA color = PartPlateList::parking_section_color(printed, m_is_dark);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.r(), color.g(), color.b(), 1.f));
        imgui.text(format_wxstr(printed ? _L("Already printed (%1%)") : _L("Do not print anymore (%1%)"), section_count));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            imgui.tooltip(_L("Right click > \"Already printed\" or \"Do not print\" again, or drag the object onto a plate, to put it "
                             "back onto its plate. Click to see the area."),
                          ImGui::GetFontSize() * 20.f);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                wxGetApp().CallAfter([this]() {
                    set_parking_zone_hidden(false);
                    zoom_to_parking_zone();
                });
        }
    }

    // Size kept to keep the label in the canvas at the next frame.
    const ImVec2 size = ImGui::GetWindowSize();
    if (std::abs(size.x - m_parking_label_width) > 0.5f || std::abs(size.y - m_parking_label_height) > 0.5f) {
        m_parking_label_width  = size.x;
        m_parking_label_height = size.y;
        request_extra_frame();
    }
    imgui.end();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
}

//Orio: card in the middle of the Preview when the current plate is not sliced with the current settings (a setting
// changed while the Preview is shown): the object is still there (shells), it has to be sliced again. Same look as the label
// of the « Hors impression » area, without its eye. Not while slicing, nor when the plate cannot be sliced (notifications).
void GLCanvas3D::_render_stale_slice_card()
{
    if (m_canvas_type != CanvasPreview || !wxGetApp().is_editor())
        return;
    Plater    *plater = wxGetApp().plater();
    PartPlate *plate  = plater->get_partplate_list().get_curr_plate();
    if (plater->only_gcode_mode() || plater->using_exported_file() || plater->is_background_process_slicing() || plate == nullptr ||
        plate->is_slice_result_valid() || !plate->can_slice() || !plate->has_printable_instances())
        return;

    const Size    cnv_size = get_canvas_size();
    const float   sc       = get_scale();
    ImGuiWrapper &imgui    = *wxGetApp().imgui();
    imgui.set_next_window_pos(0.5f * float(cnv_size.get_width()), 0.5f * float(cnv_size.get_height()), ImGuiCond_Always, 0.5f, 0.5f);
    const ImVec4 accent     = m_is_dark ? ImVec4(0.75f, 0.75f, 0.75f, 1.f) : ImVec4(0.45f, 0.45f, 0.45f, 1.f);
    const ImVec4 text_color = m_is_dark ? ImVec4(0.92f, 0.92f, 0.92f, 1.f) : ImVec4(0.16f, 0.16f, 0.16f, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f * sc, 3.f * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * sc, 0.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_is_dark ? theme_dark_window_bg(0.9f) : ImVec4(1.f, 1.f, 1.f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, accent);
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    imgui.begin(std::string("stale_slice_card"), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Preview not up to date with the current settings."));
    // The action, in green, clickable like the names of the « Hors impression » label: what Ctrl+R does (MainFrame).
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.f));
    imgui.text(format_wxstr(_L("Slice again (%1%)"), GUI::shortkey_ctrl_prefix() + "R"));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        imgui.tooltip(_L("A changed setting shows in the Preview once sliced again. Check \"Auto-slice\" in the \"Slicing Result\" "
                         "window to slice again automatically."),
                      ImGui::GetFontSize() * 20.f);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            wxGetApp().CallAfter([]() {
                Plater *plater = wxGetApp().plater();
                plater->update(true, true);
                wxPostEvent(plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
            });
    }

    // Auto-sized: hidden at its first frame, shown at the next one.
    if (ImGui::IsWindowAppearing())
        request_extra_frame();
    imgui.end();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
}

//Orio: eye of the Preview, on the center of mass of the hovered object (its shells, raycast): a click hides its
// toolpaths (supports and brim included, in its label of the G-code), only its shell stays, very faint; hovering it shows the
// eye again. Objects are labeled in the G-code only from 2 to 64 instances (GCode.cpp), and only the advanced renderer filters
// by object: elsewhere no eye (can_hide_object).
// Orio : l'objet clique dans l'Apercu, dans le cadre blanc de la selection de la Preparation
void GLCanvas3D::_render_picked_object_frame()
{
    const int label = get_gcode_viewer().get_picked_label();
    if (label < 0 || !get_gcode_viewer().can_hide_object(label))
        return;
    const ModelObjectPtrs &objects = wxGetApp().model().objects;
    BoundingBoxf3          box;
    for (const GLVolume *v : get_gcode_viewer().get_shells().volumes.volumes) {
        const int o = v->object_idx(), i = v->instance_idx();
        if (!v->is_wipe_tower && o >= 0 && o < int(objects.size()) && i >= 0 && i < int(objects[o]->instances.size()) &&
            int(objects[o]->instances[i]->get_labeled_id()) == label)
            box.merge(v->transformed_bounding_box());
    }
    if (!box.defined)
        return;
    float scale_factor = 1.0f;
#if ENABLE_RETINA_GL
    scale_factor = m_retina_helper->get_scale_factor();
#endif // ENABLE_RETINA_GL
    float white[3] = { 1.0f, 1.0f, 1.0f };
    m_selection.render_bounding_box(box, white, scale_factor);
}

void GLCanvas3D::_render_object_hide_eye()
{
    const int                  last_label  = m_hide_eye_label;
    const std::array<float, 4> last_eye    = m_hide_eye_rect;
    const std::array<float, 4> last_object = m_hide_eye_object_rect;
    m_hide_eye_label                       = -1;
    m_hover_pick_label                     = -1;
    m_hide_eye_rect = m_hide_eye_object_rect = { { 0.f, 0.f, 0.f, 0.f } };
    if (m_canvas_type != CanvasPreview || !wxGetApp().is_editor() || wxGetApp().plater()->only_gcode_mode())
        return;
    gcode::GCodeViewer &viewer = get_gcode_viewer();
    const GLVolumePtrs &shells = viewer.get_shells().volumes.volumes;
    if (shells.empty()) {
        m_hide_eye_meshes.clear();
        return;
    }

    // The solid part of the model behind a shell, and the label of its instance (-1: wipe tower, out of the model).
    const ModelObjectPtrs &objects = wxGetApp().model().objects;
    const auto part_of = [&objects](const GLVolume &v, int &label) -> const ModelVolume * {
        const int o = v.object_idx(), i = v.instance_idx(), m = v.volume_idx();
        label = -1;
        if (v.is_wipe_tower || o < 0 || o >= int(objects.size()) || i < 0 || i >= int(objects[o]->instances.size()) || m < 0 ||
            m >= int(objects[o]->volumes.size()) || !objects[o]->volumes[m]->is_model_part())
            return nullptr;
        label = int(objects[o]->instances[i]->get_labeled_id());
        return objects[o]->volumes[m];
    };
    // Meshes of the shells only (a mesh edited or deleted is forgotten).
    {
        std::set<const TriangleMesh *> current;
        for (const GLVolume *v : shells) {
            int label;
            if (const ModelVolume *part = part_of(*v, label))
                current.insert(&part->mesh());
        }
        for (auto it = m_hide_eye_meshes.begin(); it != m_hide_eye_meshes.end();)
            it = current.count(it->first) ? std::next(it) : m_hide_eye_meshes.erase(it);
    }
    const auto mesh_of = [this](const ModelVolume &part) -> const HideEyeMesh & {
        auto it = m_hide_eye_meshes.find(&part.mesh());
        if (it != m_hide_eye_meshes.end())
            return it->second;
        HideEyeMesh e;
        e.mesh      = part.get_mesh_shared_ptr();
        e.raycaster = std::make_shared<MeshRaycaster>(*e.mesh);
        // Center of mass: sum of the tetrahedra (origin, triangle), signed.
        const indexed_triangle_set &its = e.mesh->its;
        double                      vol = 0.;
        Vec3d                       sum = Vec3d::Zero();
        for (const stl_triangle_vertex_indices &f : its.indices) {
            const Vec3d  a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(), c = its.vertices[f[2]].cast<double>();
            const double t = a.dot(b.cross(c));
            vol += t;
            sum += t * (a + b + c);
        }
        if (std::abs(vol) > EPSILON) {
            e.center = sum / (4. * vol);
            e.volume = std::abs(vol) / 6.;
        } else
            e.center = e.mesh->bounding_box().center();
        return m_hide_eye_meshes.emplace(&part.mesh(), std::move(e)).first->second;
    };

    const Vec2d mouse  = m_mouse.position;
    const auto  inside = [&mouse](const std::array<float, 4> &r) {
        return r[0] < r[2] && r[0] <= mouse.x() && mouse.x() <= r[2] && r[1] <= mouse.y() && mouse.y() <= r[3];
    };
    const Camera &camera   = get_active_camera();
    const bool    mouse_in = mouse.x() >= 0. && mouse.y() >= 0. && !m_mouse.dragging;
    // The nearest hideable object under the mouse (the one a click picks), not looked for on its eye.
    if (mouse_in && !(last_label >= 0 && inside(last_eye)) && !wxGetApp().imgui()->want_mouse()) {
        double nearest = std::numeric_limits<double>::max();
        for (const GLVolume *v : shells) {
            int                l;
            const ModelVolume *part = part_of(*v, l);
            if (part == nullptr || !viewer.can_hide_object(l))
                continue;
            const Transform3d matrix = v->world_matrix();
            Vec3f             position, normal;
            if (!mesh_of(*part).raycaster->unproject_on_mesh(mouse, matrix, camera, position, normal))
                continue;
            const double depth = (matrix * position.cast<double>() - camera.get_position()).dot(camera.get_dir_forward());
            if (depth < nearest) {
                nearest            = depth;
                m_hover_pick_label = l;
            }
        }
    }
    // Orio : the object picked by a click keeps its eye, wherever the mouse is, and the other objects show none (parts placed
    // inside a ring hide its eye, at its center, otherwise).
    const int picked = viewer.get_picked_label();
    int       label  = -1;
    if (picked >= 0 && viewer.can_hide_object(picked))
        label = picked;
    else if (!mouse_in)
        return;
    else if (last_label >= 0 && inside(last_eye))
        // On its eye: the object stays, whatever is under the eye.
        label = last_label;
    else if (!wxGetApp().imgui()->want_mouse()) {
        label = m_hover_pick_label;
        // Nothing under the mouse, within the object on the screen (the hole of a ring, around its center): towards its eye.
        if (label < 0 && last_label >= 0 && inside(last_object))
            label = last_label;
    }
    if (label < 0 || !viewer.can_hide_object(label))
        return;

    // Center of mass of the object (its parts weighted by their volume, or their mean) and its box on the screen.
    const Size     cnv_size  = get_canvas_size();
    const float    cnv_w     = float(cnv_size.get_width());
    const float    cnv_h     = float(cnv_size.get_height());
    const Matrix4d view_proj = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const auto     project   = [&](const Vec3d &p, float &sx, float &sy) {
        const Vec4d clip = view_proj * Vec4d(p.x(), p.y(), p.z(), 1.);
        if (clip.w() <= EPSILON)
            return false;
        sx = float((0.5 + 0.5 * clip.x() / clip.w()) * cnv_w);
        sy = float((0.5 - 0.5 * clip.y() / clip.w()) * cnv_h);
        return true;
    };
    Vec3d         sum = Vec3d::Zero(), mean = Vec3d::Zero();
    double        weight = 0.;
    int           count  = 0;
    BoundingBoxf3 box;
    for (const GLVolume *v : shells) {
        int                l;
        const ModelVolume *part = part_of(*v, l);
        if (part == nullptr || l != label)
            continue;
        const HideEyeMesh &mesh   = mesh_of(*part);
        const Transform3d  matrix = v->world_matrix();
        const Vec3d        center = matrix * mesh.center;
        const double       w      = mesh.volume * std::abs(matrix.linear().determinant());
        sum += w * center;
        weight += w;
        mean += center;
        ++count;
        box.merge(v->transformed_bounding_box());
    }
    if (count == 0)
        return;
    const Vec3d center = weight > EPSILON ? Vec3d(sum / weight) : Vec3d(mean / double(count));
    float       x = 0.f, y = 0.f;
    if (!project(center, x, y))
        return;
    {
        std::array<float, 4> rect = { { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX } };
        bool                 ok   = true;
        for (int corner = 0; corner < 8 && ok; ++corner) {
            const Vec3d p((corner & 1) ? box.max.x() : box.min.x(), (corner & 2) ? box.max.y() : box.min.y(),
                          (corner & 4) ? box.max.z() : box.min.z());
            float sx = 0.f, sy = 0.f;
            if ((ok = project(p, sx, sy)))
                rect = { { std::min(rect[0], sx), std::min(rect[1], sy), std::max(rect[2], sx), std::max(rect[3], sy) } };
        }
        if (ok)
            m_hide_eye_object_rect = rect;
    }
    m_hide_eye_label = label;

    // The eye alone, in the look of the label of the « Hors impression » area: open when the object is shown, crossed out
    // when it is hidden.
    const bool    hidden = viewer.is_object_hidden(label);
    const float   sc     = get_scale();
    ImGuiWrapper &imgui  = *wxGetApp().imgui();
    imgui.set_next_window_pos(x, y, ImGuiCond_Always, 0.5f, 0.5f);
    const ImVec4 border     = m_is_dark ? ImVec4(0.75f, 0.75f, 0.75f, 1.f) : ImVec4(0.45f, 0.45f, 0.45f, 1.f);
    const ImVec4 text_color = m_is_dark ? ImVec4(0.92f, 0.92f, 0.92f, 1.f) : ImVec4(0.16f, 0.16f, 0.16f, 1.f);
    const ImVec4 accent     = m_is_dark ? ImVec4(0.f, 194.f / 255.f, 1.f, 1.f) : ImVec4(8.f / 255.f, 119.f / 255.f, 232.f / 255.f, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3.f * sc, 3.f * sc));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_is_dark ? theme_dark_window_bg(0.9f) : ImVec4(1.f, 1.f, 1.f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    imgui.begin(std::string("object_hide_eye"), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing);
    // The window is never smaller than WindowMinSize: the button fills it so that the eye sits in its middle.
    const ImGuiStyle &style    = ImGui::GetStyle();
    const float       eye_size = ImGui::GetFrameHeight();
    const float       side     = std::max({ eye_size, style.WindowMinSize.x - 2.f * style.WindowPadding.x,
                                            style.WindowMinSize.y - 2.f * style.WindowPadding.y });
    const ImVec2      eye_pos  = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##object_hide_eye", ImVec2(side, side));
    const bool eye_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        wxGetApp().CallAfter([this, label, hidden]() {
            get_gcode_viewer().set_object_hidden(label, !hidden);
            set_as_dirty();
        });
    {
        ImDrawList  *draw  = ImGui::GetWindowDrawList();
        const ImU32  color = ImGui::GetColorU32(eye_hovered ? accent : text_color);
        const ImVec2 c(eye_pos.x + 0.5f * side, eye_pos.y + 0.5f * side);
        const float  rx    = 0.4f * eye_size;
        const float  ry    = 0.24f * eye_size;
        const float  thick = std::max(1.f, 1.4f * sc);
        draw->AddBezierCurve(ImVec2(c.x - rx, c.y), ImVec2(c.x - 0.45f * rx, c.y - 1.33f * ry), ImVec2(c.x + 0.45f * rx, c.y - 1.33f * ry),
                             ImVec2(c.x + rx, c.y), color, thick);
        draw->AddBezierCurve(ImVec2(c.x - rx, c.y), ImVec2(c.x - 0.45f * rx, c.y + 1.33f * ry), ImVec2(c.x + 0.45f * rx, c.y + 1.33f * ry),
                             ImVec2(c.x + rx, c.y), color, thick);
        if (hidden) {
            draw->AddCircle(c, 0.5f * ry, color, 0, thick);
            draw->AddLine(ImVec2(c.x - 0.85f * rx, c.y + 0.75f * rx), ImVec2(c.x + 0.85f * rx, c.y - 0.75f * rx), color, thick);
        } else
            draw->AddCircleFilled(c, 0.55f * ry, color);
    }
    if (eye_hovered)
        imgui.tooltip(hidden ? _L("Show this object again.") : _L("Hide this object: only its shell stays, very faint."),
                      ImGui::GetFontSize() * 20.f);
    const ImVec2 pos  = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    m_hide_eye_rect   = { { pos.x, pos.y, pos.x + size.x, pos.y + size.y } };
    // Auto-sized: hidden at its first frame, shown at the next one.
    if (ImGui::IsWindowAppearing())
        request_extra_frame();
    imgui.end();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void GLCanvas3D::_render_selection_sidebar_hints() const
{
    m_selection.render_sidebar_hints(m_sidebar_field, m_gizmos.get_uniform_scaling());
}

void GLCanvas3D::_update_volumes_hover_state()
{
    for (GLVolume* v : m_volumes.volumes) {
        v->hover = GLVolume::HS_None;
    }

    if (m_hover_volume_idxs.empty())
        return;

    bool ctrl_pressed = wxGetKeyState(WXK_CONTROL); // additive select/deselect
    bool shift_pressed = wxGetKeyState(WXK_SHIFT);  // select by rectangle
    bool alt_pressed = wxGetKeyState(WXK_ALT);      // deselect by rectangle

    if (alt_pressed && (shift_pressed || ctrl_pressed)) {
        // illegal combinations of keys
        m_hover_volume_idxs.clear();
        return;
    }

    bool selection_modifiers_only = m_selection.is_empty() || m_selection.is_any_modifier();

    bool hover_modifiers_only = true;
    for (int i : m_hover_volume_idxs) {
        if (!m_volumes.volumes[i]->is_modifier) {
            hover_modifiers_only = false;
            break;
        }
    }

    std::set<std::pair<int, int>> hover_instances;
    for (int i : m_hover_volume_idxs) {
        const GLVolume& v = *m_volumes.volumes[i];
        hover_instances.insert(std::make_pair(v.object_idx(), v.instance_idx()));
    }

    bool hover_from_single_instance = hover_instances.size() == 1;

    if (hover_modifiers_only && !hover_from_single_instance) {
        // do not allow to select volumes from different instances
        m_hover_volume_idxs.clear();
        return;
    }

    for (int i : m_hover_volume_idxs) {
        GLVolume& volume = *m_volumes.volumes[i];
        if (volume.hover != GLVolume::HS_None)
            continue;

        bool deselect = volume.selected && ((ctrl_pressed && !shift_pressed) || alt_pressed);
        // (volume->is_modifier && !selection_modifiers_only && !is_ctrl_pressed) -> allows hovering on selected modifiers belonging to selection of type Instance
        bool select = (!volume.selected || (volume.is_modifier && !selection_modifiers_only && !ctrl_pressed)) && !alt_pressed;

        if (select || deselect) {
            bool as_volume =
                volume.is_modifier && hover_from_single_instance && !ctrl_pressed &&
                (
                (!deselect) ||
                (deselect && !m_selection.is_single_full_instance() && (volume.object_idx() == m_selection.get_object_idx()) && (volume.instance_idx() == m_selection.get_instance_idx()))
                );

            if (as_volume)
                volume.hover = deselect ? GLVolume::HS_Deselect : GLVolume::HS_Select;
            else {
                int object_idx = volume.object_idx();
                int instance_idx = volume.instance_idx();

                for (GLVolume* v : m_volumes.volumes) {
                    if (v->object_idx() == object_idx && v->instance_idx() == instance_idx)
                        v->hover = deselect ? GLVolume::HS_Deselect : GLVolume::HS_Select;
                }
            }
        }
        else if (volume.selected)
            volume.hover = GLVolume::HS_Hover;
    }
}

void GLCanvas3D::_perform_layer_editing_action(wxMouseEvent* evt)
{
    int object_idx_selected = m_layers_editing.last_object_id;
    if (object_idx_selected == -1)
        return;

    // A volume is selected. Test, whether hovering over a layer thickness bar.
    if (evt != nullptr) {
        //Orio: the bar in its panel (PaintAxisBar), or the part.
        m_layers_editing.last_z = m_layers_editing.edit_coordinate_at(*this, (float)evt->GetX(), (float)evt->GetY());
        m_layers_editing.last_action =
            evt->ShiftDown() ? (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_SMOOTH : LAYER_HEIGHT_EDIT_ACTION_REDUCE) :
            (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_INCREASE : LAYER_HEIGHT_EDIT_ACTION_DECREASE);
    }
    //Orio: a stroke started on the part pauses while the mouse is off it (the timer keeps running until the
    // button is released).
    if (std::isnan(m_layers_editing.last_z))
        return;

    m_layers_editing.adjust_layer_height_profile();
    _refresh_if_shown_on_screen();

    // Automatic action on mouse down with the same coordinate.
    _start_timer();
}

//BBS: variable print speed along Z.
void GLCanvas3D::_perform_speed_editing_action(wxMouseEvent* evt)
{
    int object_idx_selected = m_speed_editing.last_object_id;
    if (object_idx_selected == -1)
        return;

    if (evt != nullptr) {
        //Orio: along Z the bar on the right, along X / Y the bar at the bottom of the scene.
        m_speed_editing.last_z = m_speed_editing.edit_coordinate_at(*this, (float)evt->GetX(), (float)evt->GetY());
        // Same gestures as the variable layer height tool: left slows down (more quality),
        // right speeds up, shift resets to the base speed / smooths.
        m_speed_editing.last_action =
            evt->ShiftDown() ? (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_SMOOTH : LAYER_HEIGHT_EDIT_ACTION_REDUCE) :
            (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_INCREASE : LAYER_HEIGHT_EDIT_ACTION_DECREASE);
    }
    //Orio: a stroke started on the part pauses while the mouse is off it (the timer keeps running until the
    // button is released).
    if (std::isnan(m_speed_editing.last_z))
        return;

    m_speed_editing.adjust_speed_profile();
    _refresh_if_shown_on_screen();

    // Automatic action on mouse down with the same coordinate.
    _start_timer();
}

//Orio: variable sparse infill density / wall count along Z.
void GLCanvas3D::_perform_infill_walls_editing_action(wxMouseEvent* evt)
{
    int object_idx_selected = m_infill_walls_editing.last_object_id;
    if (object_idx_selected == -1)
        return;

    if (evt != nullptr) {
        // Along Z the bar on the right, along X / Y the bar at the bottom of the scene.
        m_infill_walls_editing.last_z = m_infill_walls_editing.edit_coordinate_at(*this, (float)evt->GetX(), (float)evt->GetY());
        // Same gestures as the other editors: left decreases the value, right increases it, shift resets to
        // the base value / smooths.
        m_infill_walls_editing.last_action =
            evt->ShiftDown() ? (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_SMOOTH : LAYER_HEIGHT_EDIT_ACTION_REDUCE) :
            (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_INCREASE : LAYER_HEIGHT_EDIT_ACTION_DECREASE);
    }
    // A stroke started on the part pauses while the mouse is off it (the timer keeps running until the button is released).
    if (std::isnan(m_infill_walls_editing.last_z))
        return;

    m_infill_walls_editing.adjust_profile();
    _refresh_if_shown_on_screen();

    // Automatic action on mouse down with the same coordinate.
    _start_timer();
}

Vec3d GLCanvas3D::_mouse_to_3d(const Camera& camera, const Point& mouse_pos, float* z, const std::string& frame_name)
{
    if (m_canvas == nullptr)
        return Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);
    Matrix4d modelview = camera.get_view_matrix().matrix();
    Matrix4d projection = camera.get_projection_matrix().matrix();
    Vec4i viewport(camera.get_viewport().data());
    GLint y = viewport[3] - (GLint)mouse_pos(1);
    GLfloat mouse_z;
    if (z == nullptr) {
        const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
        p_ogl_manager->read_pixel(frame_name, (GLint)mouse_pos(0), y, 1, 1, EPixelFormat::DepthComponent, EPixelDataType::Float, (void*)&mouse_z);
    }
    else
        mouse_z = *z;
    Vec3d out;
    igl::unproject(Vec3d(mouse_pos(0), y, mouse_z), modelview, projection, viewport, out);
    return out;
}

Vec3d GLCanvas3D::_mouse_to_bed_3d(const Point& mouse_pos)
{
    return mouse_ray(mouse_pos).intersect_plane(0.0);
}

void GLCanvas3D::_start_timer()
{
    m_timer.Start(100, wxTIMER_CONTINUOUS);
}

void GLCanvas3D::_stop_timer()
{
    m_timer.Stop();
}

void GLCanvas3D::_load_print_toolpaths(const BuildVolume &build_volume)
{
    const Print *print = this->fff_print();
    if (print == nullptr)
        return;

    if (! print->is_step_done(psSkirtBrim))
        return;

    if (!print->has_skirt() && !print->has_brim())
        return;

    const std::array<float, 4> color = { 0.5f, 1.0f, 0.5f, 1.0f }; // greenish

    // number of skirt layers
    size_t total_layer_count = 0;
    for (const PrintObject* print_object : print->objects()) {
        total_layer_count = std::max(total_layer_count, print_object->total_layer_count());
    }
    size_t skirt_height = print->has_infinite_skirt() ? total_layer_count : std::min<size_t>(print->config().skirt_height.value, total_layer_count);
    if (skirt_height == 0 && print->has_brim())
        skirt_height = 1;

    // Get first skirt_height layers.
    //FIXME This code is fishy. It may not work for multiple objects with different layering due to variable layer height feature.
    // This is not critical as this is just an initial preview.
    const PrintObject* highest_object = *std::max_element(print->objects().begin(), print->objects().end(), [](auto l, auto r){ return l->layers().size() < r->layers().size(); });
    std::vector<float> print_zs;
    print_zs.reserve(skirt_height * 2);
    for (size_t i = 0; i < std::min(skirt_height, highest_object->layers().size()); ++ i)
        print_zs.emplace_back(float(highest_object->layers()[i]->print_z));
    // Only add skirt for the raft layers.
    for (size_t i = 0; i < std::min(skirt_height, std::min(highest_object->slicing_parameters().raft_layers(), highest_object->support_layers().size())); ++ i)
        print_zs.emplace_back(float(highest_object->support_layers()[i]->print_z));
    sort_remove_duplicates(print_zs);
    skirt_height = std::min(skirt_height, print_zs.size());
    print_zs.erase(print_zs.begin() + skirt_height, print_zs.end());

    GLVolume *volume = m_volumes.new_toolpath_volume(color, VERTEX_BUFFER_RESERVE_SIZE);
    for (size_t i = 0; i < skirt_height; ++ i) {
        volume->print_zs.emplace_back(print_zs[i]);
        volume->offsets.emplace_back(volume->indexed_vertex_array->quad_indices.size());
        volume->offsets.emplace_back(volume->indexed_vertex_array->triangle_indices.size());
        //BBS: usage of m_brim are deleted
        _3DScene::extrusionentity_to_verts(print->skirt(), print_zs[i], Point(0, 0), *volume);
        // Ensure that no volume grows over the limits. If the volume is too large, allocate a new one.
        if (volume->indexed_vertex_array->vertices_and_normals_interleaved.size() > MAX_VERTEX_BUFFER_SIZE) {
        	GLVolume &vol = *volume;
            volume = m_volumes.new_toolpath_volume(vol.color);
            reserve_new_volume_finalize_old_volume(*volume, vol, m_initialized);
        }
    }
    volume->is_outside = ! build_volume.all_paths_inside_vertices_and_normals_interleaved(volume->indexed_vertex_array->vertices_and_normals_interleaved, volume->indexed_vertex_array->bounding_box());
    volume->indexed_vertex_array->finalize_geometry(m_initialized);
}

void GLCanvas3D::_load_print_object_toolpaths(const PrintObject& print_object, const BuildVolume& build_volume, const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values)
{
    std::vector<std::array<float, 4>> tool_colors = _parse_colors(str_tool_colors);

    struct Ctxt
    {
        const PrintInstances        *shifted_copies;
        std::vector<const Layer*>    layers;
        bool                         has_perimeters;
        bool                         has_infill;
        bool                         has_support;
        const std::vector<std::array<float, 4>>* tool_colors;
        bool                         is_single_material_print;
        int                          filaments_cnt;
        const std::vector<CustomGCode::Item>*   color_print_values;

        static const std::array<float, 4>& color_perimeters() { static std::array<float, 4> color = { 1.0f, 1.0f, 0.0f, 1.f }; return color; } // yellow
        static const std::array<float, 4>& color_infill() { static std::array<float, 4> color = { 1.0f, 0.5f, 0.5f, 1.f }; return color; } // redish
        static const std::array<float, 4>& color_support() { static std::array<float, 4> color = { 0.5f, 1.0f, 0.5f, 1.f }; return color; } // greenish
        static const std::array<float, 4>& color_pause_or_custom_code() { static std::array<float, 4> color = { 0.5f, 0.5f, 0.5f, 1.f }; return color; } // gray

        // For cloring by a tool, return a parsed color.
        bool                         color_by_tool() const { return tool_colors != nullptr; }
        size_t                       number_tools() const { return color_by_tool() ? tool_colors->size() : 0; }
        const std::array<float, 4>&  color_tool(size_t tool) const { return (*tool_colors)[tool]; }

        // For coloring by a color_print(M600), return a parsed color.
        bool                         color_by_color_print() const { return color_print_values!=nullptr; }
        const size_t                 color_print_color_idx_by_layer_idx(const size_t layer_idx) const {
            const CustomGCode::Item value{layers[layer_idx]->print_z + EPSILON, CustomGCode::Custom, 0, ""};
            auto it = std::lower_bound(color_print_values->begin(), color_print_values->end(), value);
            return (it - color_print_values->begin()) % number_tools();
        }

        const size_t                 color_print_color_idx_by_layer_idx_and_extruder(const size_t layer_idx, const int extruder) const
        {
            const coordf_t print_z = layers[layer_idx]->print_z;

            auto it = std::find_if(color_print_values->begin(), color_print_values->end(),
                [print_z](const CustomGCode::Item& code)
                { return fabs(code.print_z - print_z) < EPSILON; });
            if (it != color_print_values->end()) {
                CustomGCode::Type type = it->type;
                // pause print or custom Gcode
                if (type == CustomGCode::PausePrint ||
                    (type != CustomGCode::ColorChange && type != CustomGCode::ToolChange))
                    return number_tools()-1; // last color item is a gray color for pause print or custom G-code

                // change tool (extruder)
                if (type == CustomGCode::ToolChange)
                    return get_color_idx_for_tool_change(it, extruder);
                // change color for current extruder
                if (type == CustomGCode::ColorChange) {
                    int color_idx = get_color_idx_for_color_change(it, extruder);
                    if (color_idx >= 0)
                        return color_idx;
                }
            }

            const CustomGCode::Item value{print_z + EPSILON, CustomGCode::Custom, 0, ""};
            it = std::lower_bound(color_print_values->begin(), color_print_values->end(), value);
            while (it != color_print_values->begin()) {
                --it;
                // change color for current extruder
                if (it->type == CustomGCode::ColorChange) {
                    int color_idx = get_color_idx_for_color_change(it, extruder);
                    if (color_idx >= 0)
                        return color_idx;
                }
                // change tool (extruder)
                if (it->type == CustomGCode::ToolChange)
                    return get_color_idx_for_tool_change(it, extruder);
            }

            return std::min<int>(filaments_cnt - 1, std::max<int>(extruder - 1, 0));;
        }

    private:
        int get_m600_color_idx(std::vector<CustomGCode::Item>::const_iterator it) const
        {
            int shift = 0;
            while (it != color_print_values->begin()) {
                --it;
                if (it->type == CustomGCode::ColorChange)
                    shift++;
            }
            return filaments_cnt + shift;
        }

        int get_color_idx_for_tool_change(std::vector<CustomGCode::Item>::const_iterator it, const int extruder) const
        {
            const int current_extruder = it->extruder == 0 ? extruder : it->extruder;
            if (number_tools() == size_t(filaments_cnt + 1)) // there is no one "M600"
                return std::min<int>(filaments_cnt - 1, std::max<int>(current_extruder - 1, 0));

            auto it_n = it;
            while (it_n != color_print_values->begin()) {
                --it_n;
                if (it_n->type == CustomGCode::ColorChange && it_n->extruder == current_extruder)
                    return get_m600_color_idx(it_n);
            }

            return std::min<int>(filaments_cnt - 1, std::max<int>(current_extruder - 1, 0));
        }

        int get_color_idx_for_color_change(std::vector<CustomGCode::Item>::const_iterator it, const int extruder) const
        {
            if (filaments_cnt == 1)
                return get_m600_color_idx(it);

            auto it_n = it;
            bool is_tool_change = false;
            while (it_n != color_print_values->begin()) {
                --it_n;
                if (it_n->type == CustomGCode::ToolChange) {
                    is_tool_change = true;
                    if (it_n->extruder == it->extruder || (it_n->extruder == 0 && it->extruder == extruder))
                        return get_m600_color_idx(it);
                    break;
                }
            }
            if (!is_tool_change && it->extruder == extruder)
                return get_m600_color_idx(it);

            return -1;
        }

    } ctxt;

    ctxt.has_perimeters = print_object.is_step_done(posPerimeters);
    ctxt.has_infill = print_object.is_step_done(posInfill);
    ctxt.has_support = print_object.is_step_done(posSupportMaterial);
    ctxt.tool_colors = tool_colors.empty() ? nullptr : &tool_colors;
    ctxt.color_print_values = color_print_values.empty() ? nullptr : &color_print_values;
    ctxt.is_single_material_print = this->fff_print()->extruders().size()==1;
    ctxt.filaments_cnt = wxGetApp().filaments_cnt();

    ctxt.shifted_copies = &print_object.instances();

    // order layers by print_z
    {
        size_t nlayers = 0;
        if (ctxt.has_perimeters || ctxt.has_infill)
            nlayers = print_object.layers().size();
        if (ctxt.has_support)
            nlayers += print_object.support_layers().size();
        ctxt.layers.reserve(nlayers);
    }
    if (ctxt.has_perimeters || ctxt.has_infill)
        for (const Layer *layer : print_object.layers())
            ctxt.layers.emplace_back(layer);
    if (ctxt.has_support)
        for (const Layer *layer : print_object.support_layers())
            ctxt.layers.emplace_back(layer);
    std::sort(ctxt.layers.begin(), ctxt.layers.end(), [](const Layer *l1, const Layer *l2) { return l1->print_z < l2->print_z; });

    // Maximum size of an allocation block: 32MB / sizeof(float)
    BOOST_LOG_TRIVIAL(debug) << "Loading print object toolpaths in parallel - start" << m_volumes.log_memory_info() << log_memory_info();

    const bool is_selected_separate_extruder = m_selected_extruder > 0 && ctxt.color_by_color_print();

    //FIXME Improve the heuristics for a grain size.
    size_t          grain_size = std::max(ctxt.layers.size() / 16, size_t(1));
    tbb::spin_mutex new_volume_mutex;
    auto            new_volume = [this, &new_volume_mutex](const std::array<float, 4>& color) {
        // Allocate the volume before locking.
		GLVolume *volume = new GLVolume(color);
		volume->is_extrusion_path = true;
    	tbb::spin_mutex::scoped_lock lock;
    	// Lock by ROII, so if the emplace_back() fails, the lock will be released.
        lock.acquire(new_volume_mutex);
        m_volumes.volumes.emplace_back(volume);
        lock.release();
        return volume;
    };
    const size_t    volumes_cnt_initial = m_volumes.volumes.size();
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, ctxt.layers.size(), grain_size),
        [&ctxt, &new_volume, is_selected_separate_extruder, this](const tbb::blocked_range<size_t>& range) {
        GLVolumePtrs 		vols;
        auto                volume = [&ctxt, &vols](size_t layer_idx, int extruder, int feature) -> GLVolume& {
            return *vols[ctxt.color_by_color_print()?
                ctxt.color_print_color_idx_by_layer_idx_and_extruder(layer_idx, extruder) :
				ctxt.color_by_tool() ?
					std::min<int>(ctxt.number_tools() - 1, std::max<int>(extruder - 1, 0)) :
					feature
				];
        };
        if (ctxt.color_by_color_print() || ctxt.color_by_tool()) {
            for (size_t i = 0; i < ctxt.number_tools(); ++i)
                vols.emplace_back(new_volume(ctxt.color_tool(i)));
        }
        else
            vols = { new_volume(ctxt.color_perimeters()), new_volume(ctxt.color_infill()), new_volume(ctxt.color_support()) };
        for (GLVolume *vol : vols)
			// Reserving number of vertices (3x position + 3x color)
        	vol->indexed_vertex_array->reserve(VERTEX_BUFFER_RESERVE_SIZE / 6);
        for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
            const Layer *layer = ctxt.layers[idx_layer];

            if (is_selected_separate_extruder)
            {
                bool at_least_one_has_correct_extruder = false;
                for (const LayerRegion* layerm : layer->regions())
                {
                    if (layerm->slices.surfaces.empty())
                        continue;
                    const PrintRegionConfig& cfg = layerm->region().config();
                    //Orio: OrcaSlicer's six feature filaments.
                    if (cfg.outer_wall_filament_id.value     == m_selected_extruder ||
                        cfg.inner_wall_filament_id.value     == m_selected_extruder ||
                        cfg.sparse_infill_filament_id.value  == m_selected_extruder ||
                        cfg.internal_solid_filament_id.value == m_selected_extruder ||
                        cfg.top_surface_filament_id.value    == m_selected_extruder ||
                        cfg.bottom_surface_filament_id.value == m_selected_extruder) {
                        at_least_one_has_correct_extruder = true;
                        break;
                    }
                }
                if (!at_least_one_has_correct_extruder)
                    continue;
            }

            for (GLVolume *vol : vols)
                if (vol->print_zs.empty() || vol->print_zs.back() != layer->print_z) {
                    vol->print_zs.emplace_back(layer->print_z);
                    vol->offsets.emplace_back(vol->indexed_vertex_array->quad_indices.size());
                    vol->offsets.emplace_back(vol->indexed_vertex_array->triangle_indices.size());
                }
            for (const PrintInstance &instance : *ctxt.shifted_copies) {
                const Point &copy = instance.shift;
                for (const LayerRegion *layerm : layer->regions()) {
                    if (is_selected_separate_extruder)
                    {
                        const PrintRegionConfig& cfg = layerm->region().config();
                        //Orio: OrcaSlicer's six feature filaments.
                        if (cfg.outer_wall_filament_id.value     != m_selected_extruder ||
                            cfg.inner_wall_filament_id.value     != m_selected_extruder ||
                            cfg.sparse_infill_filament_id.value  != m_selected_extruder ||
                            cfg.internal_solid_filament_id.value != m_selected_extruder ||
                            cfg.top_surface_filament_id.value    != m_selected_extruder ||
                            cfg.bottom_surface_filament_id.value != m_selected_extruder)
                            continue;
                    }
                    if (ctxt.has_perimeters)
                        _3DScene::extrusionentity_to_verts(layerm->perimeters, float(layer->print_z), copy,
                        	volume(idx_layer, layerm->region().config().outer_wall_filament_id.value, 0));
                    if (ctxt.has_infill) {
                        for (const ExtrusionEntity *ee : layerm->fills.entities) {
                            // fill represents infill extrusions of a single island.
                            const auto *fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                            if (! fill->entities.empty())
                                _3DScene::extrusionentity_to_verts(*fill, float(layer->print_z), copy,
	                                volume(idx_layer,
		                                is_solid_infill(fill->entities.front()->role()) ?
			                                PrintRegion::feature_filament(layerm->region().config(), fill->entities.front()->role()) :
			                                layerm->region().config().sparse_infill_filament_id.value,
		                                1));
                        }
                    }
                }
                if (ctxt.has_support) {
                    const SupportLayer *support_layer = dynamic_cast<const SupportLayer*>(layer);
                    if (support_layer) {
                        for (const ExtrusionEntity *extrusion_entity : support_layer->support_fills.entities)
                            _3DScene::extrusionentity_to_verts(extrusion_entity, float(layer->print_z), copy,
	                            volume(idx_layer,
		                            (extrusion_entity->role() == erSupportMaterial ||
                                     extrusion_entity->role() == erSupportTransition) ?
			                            support_layer->object()->config().support_filament :
			                            support_layer->object()->config().support_interface_filament,
		                            2));
                    }
                }
            }
            // Ensure that no volume grows over the limits. If the volume is too large, allocate a new one.
	        for (size_t i = 0; i < vols.size(); ++i) {
	            GLVolume &vol = *vols[i];
	            if (vol.indexed_vertex_array->vertices_and_normals_interleaved.size() > MAX_VERTEX_BUFFER_SIZE) {
	                vols[i] = new_volume(vol.color);
	                reserve_new_volume_finalize_old_volume(*vols[i], vol, false);
	            }
	        }
        }
        for (GLVolume *vol : vols)
        	// Ideally one would call vol->indexed_vertex_array->finalize() here to move the buffers to the OpenGL driver,
        	// but this code runs in parallel and the OpenGL driver is not thread safe.
            vol->indexed_vertex_array->shrink_to_fit();
    });

    BOOST_LOG_TRIVIAL(debug) << "Loading print object toolpaths in parallel - finalizing results" << m_volumes.log_memory_info() << log_memory_info();
    // Remove empty volumes from the newly added volumes.
    m_volumes.volumes.erase(
        std::remove_if(m_volumes.volumes.begin() + volumes_cnt_initial, m_volumes.volumes.end(),
        [](const GLVolume *volume) { return volume->empty(); }),
        m_volumes.volumes.end());
    for (size_t i = volumes_cnt_initial; i < m_volumes.volumes.size(); ++i) {
        GLVolume* v = m_volumes.volumes[i];
        v->is_outside = ! build_volume.all_paths_inside_vertices_and_normals_interleaved(v->indexed_vertex_array->vertices_and_normals_interleaved, v->indexed_vertex_array->bounding_box());
        v->indexed_vertex_array->finalize_geometry(m_initialized);
    }

    BOOST_LOG_TRIVIAL(debug) << "Loading print object toolpaths in parallel - end" << m_volumes.log_memory_info() << log_memory_info();
}

void GLCanvas3D::_load_wipe_tower_toolpaths(const BuildVolume& build_volume, const std::vector<std::string>& str_tool_colors)
{
    const Print *print = this->fff_print();
    if (print == nullptr || print->wipe_tower_data().tool_changes.empty())
        return;

    if (!print->is_step_done(psWipeTower))
        return;

    std::vector<std::array<float, 4>> tool_colors = _parse_colors(str_tool_colors);

    struct Ctxt
    {
        const Print                 *print;
        const std::vector<std::array<float, 4>>* tool_colors;
        Vec2f                        wipe_tower_pos;
        float                        wipe_tower_angle;

        static const std::array<float, 4>& color_support() { static std::array<float, 4> color = { 0.5f, 1.0f, 0.5f, 1.f }; return color; } // greenish

        // For cloring by a tool, return a parsed color.
        bool                         color_by_tool() const { return tool_colors != nullptr; }
        size_t                       number_tools() const { return this->color_by_tool() ? tool_colors->size() : 0; }
        const std::array<float, 4>&  color_tool(size_t tool) const { return (*tool_colors)[tool]; }
        int                          volume_idx(int tool, int feature) const {
            return this->color_by_tool() ? std::min<int>(this->number_tools() - 1, std::max<int>(tool, 0)) : feature;
        }

        const std::vector<WipeTower::ToolChangeResult>& tool_change(size_t idx) {
            const auto &tool_changes = print->wipe_tower_data().tool_changes;
            return priming.empty() ?
                ((idx == tool_changes.size()) ? final : tool_changes[idx]) :
                ((idx == 0) ? priming : (idx == tool_changes.size() + 1) ? final : tool_changes[idx - 1]);
        }
        std::vector<WipeTower::ToolChangeResult> priming;
        std::vector<WipeTower::ToolChangeResult> final;
    } ctxt;

    ctxt.print = print;
    ctxt.tool_colors = tool_colors.empty() ? nullptr : &tool_colors;
    //BBS: has no single_extruder_multi_material_priming
    //if (print->wipe_tower_data().priming && print->config().single_extruder_multi_material_priming)
    if (print->wipe_tower_data().priming)
        for (int i=0; i<(int)print->wipe_tower_data().priming.get()->size(); ++i)
            ctxt.priming.emplace_back(print->wipe_tower_data().priming.get()->at(i));
    if (print->wipe_tower_data().final_purge)
        ctxt.final.emplace_back(*print->wipe_tower_data().final_purge.get());

    ctxt.wipe_tower_angle = ctxt.print->config().wipe_tower_rotation_angle.value/180.f * PI;

    // BBS: add partplate logic
    int plate_idx = print->get_plate_index();
    Vec3d plate_origin = print->get_plate_origin();
    double wipe_tower_x = ctxt.print->config().wipe_tower_x.get_at(plate_idx) + plate_origin(0);
    double wipe_tower_y = ctxt.print->config().wipe_tower_y.get_at(plate_idx) + plate_origin(1);
    ctxt.wipe_tower_pos = Vec2f(wipe_tower_x, wipe_tower_y);

    BOOST_LOG_TRIVIAL(debug) << "Loading wipe tower toolpaths in parallel - start" << m_volumes.log_memory_info() << log_memory_info();

    //FIXME Improve the heuristics for a grain size.
    size_t          n_items = print->wipe_tower_data().tool_changes.size() + (ctxt.priming.empty() ? 0 : 1);
    size_t          grain_size = std::max(n_items / 128, size_t(1));
    tbb::spin_mutex new_volume_mutex;
    auto            new_volume = [this, &new_volume_mutex](const std::array<float, 4>& color) {
        auto *volume = new GLVolume(color);
		volume->is_extrusion_path = true;
        tbb::spin_mutex::scoped_lock lock;
        lock.acquire(new_volume_mutex);
        m_volumes.volumes.emplace_back(volume);
        lock.release();
        return volume;
    };
    const size_t   volumes_cnt_initial = m_volumes.volumes.size();
    std::vector<GLVolumeCollection> volumes_per_thread(n_items);
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, n_items, grain_size),
        [&ctxt, &new_volume](const tbb::blocked_range<size_t>& range) {
        // Bounding box of this slab of a wipe tower.
        GLVolumePtrs vols;
        if (ctxt.color_by_tool()) {
            for (size_t i = 0; i < ctxt.number_tools(); ++i)
                vols.emplace_back(new_volume(ctxt.color_tool(i)));
        }
        else
            vols = { new_volume(ctxt.color_support()) };
        for (GLVolume *volume : vols)
			// Reserving number of vertices (3x position + 3x color)
            volume->indexed_vertex_array->reserve(VERTEX_BUFFER_RESERVE_SIZE / 6);
        for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++idx_layer) {
            const std::vector<WipeTower::ToolChangeResult> &layer = ctxt.tool_change(idx_layer);
            for (size_t i = 0; i < vols.size(); ++i) {
                GLVolume &vol = *vols[i];
                if (vol.print_zs.empty() || vol.print_zs.back() != layer.front().print_z) {
                    vol.print_zs.emplace_back(layer.front().print_z);
                    vol.offsets.emplace_back(vol.indexed_vertex_array->quad_indices.size());
                    vol.offsets.emplace_back(vol.indexed_vertex_array->triangle_indices.size());
                }
            }
            for (const WipeTower::ToolChangeResult &extrusions : layer) {
                for (size_t i = 1; i < extrusions.extrusions.size();) {
                    const WipeTower::Extrusion &e = extrusions.extrusions[i];
                    if (e.width == 0.) {
                        ++i;
                        continue;
                    }
                    size_t j = i + 1;
                    if (ctxt.color_by_tool())
                        for (; j < extrusions.extrusions.size() && extrusions.extrusions[j].tool == e.tool && extrusions.extrusions[j].width > 0.f; ++j);
                    else
                        for (; j < extrusions.extrusions.size() && extrusions.extrusions[j].width > 0.f; ++j);
                    size_t              n_lines = j - i;
                    Lines               lines;
                    std::vector<double> widths;
                    std::vector<double> heights;
                    lines.reserve(n_lines);
                    widths.reserve(n_lines);
                    heights.assign(n_lines, extrusions.layer_height);
                    WipeTower::Extrusion e_prev = extrusions.extrusions[i-1];

                    if (!extrusions.priming) { // wipe tower extrusions describe the wipe tower at the origin with no rotation
                        e_prev.pos = Eigen::Rotation2Df(ctxt.wipe_tower_angle) * e_prev.pos;
                        e_prev.pos += ctxt.wipe_tower_pos;
                    }

                    for (; i < j; ++i) {
                        WipeTower::Extrusion e = extrusions.extrusions[i];
                        assert(e.width > 0.f);
                        if (!extrusions.priming) {
                            e.pos = Eigen::Rotation2Df(ctxt.wipe_tower_angle) * e.pos;
                            e.pos += ctxt.wipe_tower_pos;
                        }

                        lines.emplace_back(Point::new_scale(e_prev.pos.x(), e_prev.pos.y()), Point::new_scale(e.pos.x(), e.pos.y()));
                        widths.emplace_back(e.width);

                        e_prev = e;
                    }
                    _3DScene::thick_lines_to_verts(lines, widths, heights, lines.front().a == lines.back().b, extrusions.print_z,
                        *vols[ctxt.volume_idx(e.tool, 0)]);
                }
            }
        }
        for (size_t i = 0; i < vols.size(); ++i) {
            GLVolume &vol = *vols[i];
            if (vol.indexed_vertex_array->vertices_and_normals_interleaved.size() > MAX_VERTEX_BUFFER_SIZE) {
                vols[i] = new_volume(vol.color);
                reserve_new_volume_finalize_old_volume(*vols[i], vol, false);
            }
        }
        for (GLVolume *vol : vols)
            vol->indexed_vertex_array->shrink_to_fit();
    });

    BOOST_LOG_TRIVIAL(debug) << "Loading wipe tower toolpaths in parallel - finalizing results" << m_volumes.log_memory_info() << log_memory_info();
    // Remove empty volumes from the newly added volumes.
    m_volumes.volumes.erase(
        std::remove_if(m_volumes.volumes.begin() + volumes_cnt_initial, m_volumes.volumes.end(),
        [](const GLVolume *volume) { return volume->empty(); }),
        m_volumes.volumes.end());
    for (size_t i = volumes_cnt_initial; i < m_volumes.volumes.size(); ++i) {
        GLVolume* v = m_volumes.volumes[i];
        v->is_outside = ! build_volume.all_paths_inside_vertices_and_normals_interleaved(v->indexed_vertex_array->vertices_and_normals_interleaved, v->indexed_vertex_array->bounding_box());
        v->indexed_vertex_array->finalize_geometry(m_initialized);
    }

    BOOST_LOG_TRIVIAL(debug) << "Loading wipe tower toolpaths in parallel - end" << m_volumes.log_memory_info() << log_memory_info();
}

// While it looks like we can call
// this->reload_scene(true, true)
// the two functions are quite different:
// 1) This function only loads objects, for which the step slaposSliceSupports already finished. Therefore objects outside of the print bed never load.
// 2) This function loads object mesh with the relative scaling correction (the "relative_correction" parameter) was applied,
// 	  therefore the mesh may be slightly larger or smaller than the mesh shown in the 3D scene.
void GLCanvas3D::_load_sla_shells()
{
    const SLAPrint* print = this->sla_print();
    if (print->objects().empty())
        // nothing to render, return
        return;

    auto add_volume = [this](const SLAPrintObject &object, int volume_id, const SLAPrintObject::Instance& instance,
        const TriangleMesh& mesh, const std::array<float, 4>& color, bool outside_printer_detection_enabled) {
        m_volumes.volumes.emplace_back(new GLVolume(color));
        GLVolume& v = *m_volumes.volumes.back();
#if ENABLE_SMOOTH_NORMALS
        v.indexed_vertex_array->load_mesh(mesh, true);
#else
        v.indexed_vertex_array->load_mesh(mesh);
#endif // ENABLE_SMOOTH_NORMALS
        v.indexed_vertex_array->finalize_geometry(m_initialized);
        v.shader_outside_printer_detection_enabled = outside_printer_detection_enabled;
        v.composite_id.volume_id = volume_id;
        v.set_instance_offset(unscale(instance.shift.x(), instance.shift.y(), 0.0));
        v.set_instance_rotation({ 0.0, 0.0, (double)instance.rotation });
        v.set_instance_mirror(X, object.is_left_handed() ? -1. : 1.);
        v.set_convex_hull(mesh.convex_hull_3d());
    };

    // adds objects' volumes
    for (const SLAPrintObject* obj : print->objects())
        if (obj->is_step_done(slaposSliceSupports)) {
            unsigned int initial_volumes_count = (unsigned int)m_volumes.volumes.size();
            for (const SLAPrintObject::Instance& instance : obj->instances()) {
                add_volume(*obj, 0, instance, obj->get_mesh_to_print(), GLVolume::MODEL_COLOR[0], true);
                // Set the extruder_id and volume_id to achieve the same color as in the 3D scene when
                // through the update_volumes_colors_by_extruder() call.
                m_volumes.volumes.back()->extruder_id = obj->model_object()->volumes.front()->extruder_id();
                if (obj->is_step_done(slaposSupportTree) && obj->has_mesh(slaposSupportTree))
                    add_volume(*obj, -int(slaposSupportTree), instance, obj->support_mesh(), GLVolume::SLA_SUPPORT_COLOR, true);
                if (obj->is_step_done(slaposPad) && obj->has_mesh(slaposPad))
                    add_volume(*obj, -int(slaposPad), instance, obj->pad_mesh(), GLVolume::SLA_PAD_COLOR, false);
            }
            double shift_z = obj->get_current_elevation();
            for (unsigned int i = initial_volumes_count; i < m_volumes.volumes.size(); ++ i) {
                // apply shift z
                m_volumes.volumes[i]->set_sla_shift_z(shift_z);
            }
        }

    update_volumes_colors_by_extruder();
}

void GLCanvas3D::_update_sla_shells_outside_state()
{
    check_volumes_outside_state();
}

void GLCanvas3D::_set_warning_notification_if_needed(EWarning warning)
{
    _set_current(true);
    bool show = false;
    auto& t_gcode_viewer = get_gcode_viewer();
    if (!m_volumes.empty()) {
        show = _is_any_volume_outside();
        show &= t_gcode_viewer.has_data() && t_gcode_viewer.is_contained_in_bed() && t_gcode_viewer.get_conflict_result().has_value();
    } else {
        if (wxGetApp().is_editor()) {
            if (current_printer_technology() != ptSLA) {
                unsigned int max_z_layer = t_gcode_viewer.get_layers_z_range().back();
                if (warning == EWarning::ToolHeightOutside) // check if max z_layer height exceed max print height
                    show = t_gcode_viewer.has_data() && (t_gcode_viewer.get_layers_zs()[max_z_layer] - t_gcode_viewer.get_max_print_height() >= 1e-6);
                else if (warning == EWarning::ToolpathOutside) { // check if max x,y coords exceed bed area
                    show = t_gcode_viewer.has_data() && !t_gcode_viewer.is_contained_in_bed() &&
                           (t_gcode_viewer.get_max_print_height() - t_gcode_viewer.get_layers_zs()[max_z_layer] >= 1e-6);
                } else if (warning == EWarning::GCodeConflict)
                    show = t_gcode_viewer.has_data() && t_gcode_viewer.is_contained_in_bed() && t_gcode_viewer.get_conflict_result().has_value();
                else if (warning == EWarning::MultiExtruderPrintableError)
                    show = t_gcode_viewer.has_data() && (t_gcode_viewer.get_gcode_check_result().error_code & 1);
                else if (warning == EWarning::MultiExtruderHeightOutside)
                    show = t_gcode_viewer.has_data() && (t_gcode_viewer.get_gcode_check_result().error_code & (1 << 1));
                else if (warning == EWarning::FilamentUnPrintableOnFirstLayer)
                    show = t_gcode_viewer.has_data() && t_gcode_viewer.get_filament_printable_result().has_value();
            }
        }
    }

    _set_warning_notification(warning, show);
}

std::vector<std::array<float, 4>> GLCanvas3D::_parse_colors(const std::vector<std::string>& colors)
{
    static const float INV_255 = 1.0f / 255.0f;

    std::vector<std::array<float, 4>> output(colors.size(), { 1.0f, 1.0f, 1.0f, 1.0f });
    for (size_t i = 0; i < colors.size(); ++i) {
        const std::string& color = colors[i];
        const char* c = color.data() + 1;
        if (color.size() == 7 && color.front() == '#') {
            for (size_t j = 0; j < 3; ++j) {
                int digit1 = hex_digit_to_int(*c++);
                int digit2 = hex_digit_to_int(*c++);
                if (digit1 == -1 || digit2 == -1)
                    break;

                output[i][j] = float(digit1 * 16 + digit2) * INV_255;
            }
        }
    }
    return output;
}

void GLCanvas3D::_push_render_stage(ERenderPipelineStage stage)
{
    m_render_pipeline_stage_stack.push(stage);
}

void GLCanvas3D::_pop_render_stage()
{
    if (m_render_pipeline_stage_stack.size() > 1)
    {
        m_render_pipeline_stage_stack.pop();
    }
}

ERenderPipelineStage GLCanvas3D::_get_current_render_stage() const
{
    return m_render_pipeline_stage_stack.top();
}

void GLCanvas3D::_render_silhouette_effect()
{
    RenderPipelineStageModifier render_pipeline_stage_modifier(*this, ERenderPipelineStage::Silhouette);

    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    if (!p_ogl_manager) {
        return;
    }

    auto& ogl_manager = *p_ogl_manager;
    float t_viewport_scale = 1.0f;
    uint32_t viewport_width = 0;
    uint32_t viewport_height = 0;
    ogl_manager.get_viewport_size(viewport_width, viewport_height);
    viewport_width = viewport_width * t_viewport_scale;
    viewport_height = viewport_height * t_viewport_scale;
    {
        OpenGLManager::FrameBufferModifier frame_buffer_modifier(ogl_manager, "silhouette", EMSAAType::X4);
        frame_buffer_modifier.set_width(viewport_width)
            .set_height(viewport_height);
    }
    // BBS: render silhouette
    glViewport(0, 0, viewport_width, viewport_height);
    glsafe(::glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
    glsafe(::glClearDepth(1.0f));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    const auto& p_silhouette_shader = wxGetApp().get_shader("silhouette");
    if (!p_silhouette_shader)
    {
        BOOST_LOG_TRIVIAL(error) << "Invalid shader: silhouette. Failed to render highlight effect.";
        return;
    }

    wxGetApp().bind_shader(p_silhouette_shader);

    const auto& picking_color = wxGetApp().get_picking_color();
    p_silhouette_shader->set_uniform("u_base_color", picking_color);

    const Camera& camera = get_active_camera();
    const Transform3d& view_matrix = camera.get_view_matrix();
    const Transform3d& projection_matrix = camera.get_projection_matrix();
    const Matrix4d view_proj = projection_matrix.matrix() * view_matrix.matrix();
    p_silhouette_shader->set_uniform("u_view_projection_matrix", view_proj);
    _render_objects(m_volumes,GLVolumeCollection::ERenderType::Opaque, false);
    _render_objects(m_volumes, GLVolumeCollection::ERenderType::Transparent, false);
    _render_objects(m_paint_outline_volumes, GLVolumeCollection::ERenderType::Opaque, false,true);
    _render_objects(m_paint_outline_volumes, GLVolumeCollection::ERenderType::Transparent, false, true);
    wxGetApp().unbind_shader();

    // BBS: end render silhouette

    std::string input_fb_name = "silhouette";
    const auto& p_frame_buffer = ogl_manager.get_frame_buffer(input_fb_name);
    if (!p_frame_buffer)
    {
        BOOST_LOG_TRIVIAL(error) << "Invalid framebuffer. Failed to render highlight effect.";
        return;
    }

    int stage = 0;

    const std::array<float, 2> viewport_size{ static_cast<float>(viewport_width), static_cast<float>(viewport_height) };

    {
        glViewport(0, 0, viewport_width, viewport_height);
        glsafe(::glDisable(GL_DEPTH_TEST));
        _init_fullscreen_mesh();
        // BBS: gaussian blur
        {
            OpenGLManager::FrameBufferModifier frame_buffer_gaussian_blur33(ogl_manager, "silhouette_gaussian_blur33", EMSAAType::Disabled);
            frame_buffer_gaussian_blur33.set_width(viewport_width)
                .set_height(viewport_height);
        }
        const auto& p_gaussian_blur33_shader = wxGetApp().get_shader("gaussian_blur33");
        if (!p_gaussian_blur33_shader)
        {
            BOOST_LOG_TRIVIAL(error) << "Invalid gaussian_blur33 shader. Failed to render highlight effect.";
            return;
        }
        wxGetApp().bind_shader(p_gaussian_blur33_shader);

        p_gaussian_blur33_shader->set_uniform("u_viewport_size", viewport_size);

        Matrix3f gaussian_blur33_convolution_matrix;
        // https://lodev.org/cgtutor/filtering.html#Gaussian_Blur_
        gaussian_blur33_convolution_matrix.data()[3 * 0 + 0] = 0.077847;
        gaussian_blur33_convolution_matrix.data()[3 * 0 + 1] = 0.123317;
        gaussian_blur33_convolution_matrix.data()[3 * 0 + 2] = 0.077847;
        gaussian_blur33_convolution_matrix.data()[3 * 1 + 0] = 0.123317;
        gaussian_blur33_convolution_matrix.data()[3 * 1 + 1] = 0.195346;
        gaussian_blur33_convolution_matrix.data()[3 * 1 + 2] = 0.123317;
        gaussian_blur33_convolution_matrix.data()[3 * 2 + 0] = 0.077847;
        gaussian_blur33_convolution_matrix.data()[3 * 2 + 1] = 0.123317;
        gaussian_blur33_convolution_matrix.data()[3 * 2 + 2] = 0.077847;
        p_gaussian_blur33_shader->set_uniform("u_convolution_matrix", gaussian_blur33_convolution_matrix);

        const auto color_texture_id = p_frame_buffer->get_color_texture();
        if (!p_frame_buffer->is_texture_valid(color_texture_id))
        {
            BOOST_LOG_TRIVIAL(error) << "Invalid silhouette texture. Failed to render highlight effect.";
            return;
        }
        int stage = 0;
        p_gaussian_blur33_shader->set_uniform("u_sampler", stage);
        glsafe(::glActiveTexture(GL_TEXTURE0 + stage));
        glsafe(::glBindTexture(GL_TEXTURE_2D, color_texture_id));

        s_full_screen_mesh.render_geometry();

        wxGetApp().unbind_shader();
        glsafe(::glEnable(GL_DEPTH_TEST));
        // BBS: end gaussian blur
    }
    ogl_manager.get_viewport_size(viewport_width, viewport_height);
    glViewport(0, 0, viewport_width, viewport_height);
}

void GLCanvas3D::_composite_silhouette_effect()
{
    // BBS: composite silhouette

    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    if (!p_ogl_manager) {
        return;
    }
    const auto& p_frame_buffer = p_ogl_manager->get_frame_buffer("silhouette_gaussian_blur33");
    if (!p_frame_buffer)
    {
        BOOST_LOG_TRIVIAL(error) << "Invalid framebuffer. Failed to render highlight effect.";
        return;
    }

    const auto color_texture_id = p_frame_buffer->get_color_texture();
    if (!p_frame_buffer->is_texture_valid(color_texture_id))
    {
        BOOST_LOG_TRIVIAL(error) << "Invalid silhouette texture. Failed to render highlight effect.";
        return;
    }

    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const auto& p_silhouette_composite_shader = wxGetApp().get_shader("silhouette_composite");
    if (!p_silhouette_composite_shader)
    {
        BOOST_LOG_TRIVIAL(error) << "Invalid silhouette shader. Failed to render highlight effect.";
        return;
    }
    wxGetApp().bind_shader(p_silhouette_composite_shader);

    uint32_t viewport_width = 0;
    uint32_t viewport_height = 0;
    p_ogl_manager->get_viewport_size(viewport_width, viewport_height);

    const float alpha = 0.4f;
    Matrix3f convolution_matrix;
    convolution_matrix.data()[3 * 0 + 0] = -alpha;
    convolution_matrix.data()[3 * 0 + 1] = -alpha;
    convolution_matrix.data()[3 * 0 + 2] = -alpha;

    convolution_matrix.data()[3 * 1 + 0] = -alpha;
    convolution_matrix.data()[3 * 1 + 1] = alpha * 9.0f;
    convolution_matrix.data()[3 * 1 + 2] = -alpha;

    convolution_matrix.data()[3 * 2 + 0] = -alpha;
    convolution_matrix.data()[3 * 2 + 1] = -alpha;
    convolution_matrix.data()[3 * 2 + 2] = -alpha;
    p_silhouette_composite_shader->set_uniform("u_convolution_matrix", convolution_matrix);

    const std::array<float, 3> viewport_size_alpha{ static_cast<float>(viewport_width), static_cast<float>(viewport_height), alpha };
    p_silhouette_composite_shader->set_uniform("u_viewport_size_alpha", viewport_size_alpha);

    const auto& picking_color = wxGetApp().get_picking_color();
    p_silhouette_composite_shader->set_uniform("u_picking_color", picking_color);

    const int stage = 0;
    p_silhouette_composite_shader->set_uniform("u_sampler", stage);
    glsafe(::glActiveTexture(GL_TEXTURE0 + stage));
    glsafe(::glBindTexture(GL_TEXTURE_2D, color_texture_id));

    _init_fullscreen_mesh();

    s_full_screen_mesh.render_geometry();

    wxGetApp().unbind_shader();

    glsafe(::glDisable(GL_BLEND));
    // BBS: end composite silhouette
}

bool GLCanvas3D::is_volume_in_plate_boundingbox(const GLVolume &v, int plate_idx, const BoundingBoxf3 &plate_build_volume)
{
    bool ret = v.printable;
    if (plate_idx >= 0) {
        bool          contained          = false;
        BoundingBoxf3 plate_bbox         = plate_build_volume;
        plate_bbox.min(2)                = -1e10;
        const BoundingBoxf3 &volume_bbox = v.transformed_convex_hull_bounding_box();
        if (plate_bbox.contains(volume_bbox) && (volume_bbox.max(2) > 0)) { contained = true; }
        ret &= contained;
    } else {
        ret &= (!v.shader_outside_printer_detection_enabled || !v.is_outside);
    }
    return ret;
}

void GLCanvas3D::_init_fullscreen_mesh()
{
    if (s_full_screen_mesh.is_initialized()) {
        return;
    }
    GLModel::Geometry geo;
    geo.format.type = GLModel::PrimitiveType::Triangles;
    geo.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3T2;

    geo.add_vertex(Vec3f{ -1.0f, -1.0f, 0.0f }, Vec2f{ 0.0f, 0.0f });
    geo.add_vertex(Vec3f{ 3.0f, -1.0f, 0.0f }, Vec2f{ 2.0f, 0.0f });
    geo.add_vertex(Vec3f{ -1.0f, 3.0f, 0.0f }, Vec2f{ 0.0f, 2.0f });

    geo.add_triangle(0, 1, 2);

    s_full_screen_mesh.init_from(std::move(geo));
}

void GLCanvas3D::_rebuild_postprocessing_pipeline(const std::shared_ptr<OpenGLManager>& p_ogl_manager, const std::string& input_framebuffer_name, std::string& output_framebuffer_name, uint32_t width, uint32_t height)
{
    if (!p_ogl_manager) {
        return;
    }

    auto &ogl_manager = *p_ogl_manager;
    const bool offscreen_rendering = input_framebuffer_name != OpenGLManager::s_back_frame;
    if (!offscreen_rendering && !ogl_manager.is_fxaa_enabled()) {
        return;
    }

    _init_fullscreen_mesh();

    uint32_t output_texture_id = UINT32_MAX;
    if (ogl_manager.is_fxaa_enabled()) {
        if (!offscreen_rendering) {
            {
                OpenGLManager::FrameBufferModifier fxaa_frame(ogl_manager, "fxaaframe_temp", EMSAAType::Disabled);
                fxaa_frame.set_width(width)
                    .set_height(height);
            }

            p_ogl_manager->blit_framebuffer(OpenGLManager::s_back_frame, "fxaaframe_temp");
        }

        {
            OpenGLManager::FrameBufferModifier fxaa_frame(ogl_manager, "fxaaframe", EMSAAType::Disabled);
            fxaa_frame.set_width(width)
                .set_height(height);
        }

        // BBS: composite main frame
        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glDisable(GL_BLEND));

        const auto& p_main_frame_buffer = ogl_manager.get_frame_buffer(offscreen_rendering ? input_framebuffer_name : "fxaaframe_temp");
        if (p_main_frame_buffer) {
            output_texture_id = p_main_frame_buffer->get_color_texture();
            if (p_main_frame_buffer->is_texture_valid(output_texture_id)) {
                const auto& p_fxaa_shader = p_ogl_manager->get_shader("fxaa");
                if (p_fxaa_shader) {
                    p_ogl_manager->bind_shader(p_fxaa_shader);

                    const int stage = 0;
                    p_fxaa_shader->set_uniform("u_sampler", stage);
                    glsafe(::glActiveTexture(GL_TEXTURE0 + stage));
                    glsafe(::glBindTexture(GL_TEXTURE_2D, output_texture_id));

                    const std::array<float, 4> viewport_size{ static_cast<float>(width), static_cast<float>(height), 1.0f / width, 1.0f / height };
                    p_fxaa_shader->set_uniform("u_viewport_size", viewport_size);

                    s_full_screen_mesh.render_geometry();

                    p_ogl_manager->unbind_shader();

                    const auto& p_fxaa_frame_buffer = p_ogl_manager->get_frame_buffer("fxaaframe");
                    if (p_fxaa_frame_buffer) {
                        output_texture_id = p_fxaa_frame_buffer->get_color_texture();
                        if (!p_fxaa_frame_buffer->is_texture_valid(output_texture_id)) {
                            BOOST_LOG_TRIVIAL(error) << "Invalid fxaa texture.";
                        }
                    }
                    else {
                        BOOST_LOG_TRIVIAL(error) << "Invalid fxaa framebuffer.";
                    }
                }
                else {
                    BOOST_LOG_TRIVIAL(error) << "Invalid fxaa shader.";
                }
            }
            else {
                BOOST_LOG_TRIVIAL(error) << "Invalid main frame texture. Failed to composite main frame.";
            }
        }
    }
    else if (offscreen_rendering) {
        const auto& p_main_frame_buffer = ogl_manager.get_frame_buffer(input_framebuffer_name);
        if (p_main_frame_buffer) {
            output_texture_id = p_main_frame_buffer->get_color_texture();
        }
    }

    {
        OpenGLManager::FrameBufferModifier output_frame(ogl_manager, output_framebuffer_name);
        output_frame.set_width(width)
            .set_height(height);
    }

    const auto& p_mainframe_composite_shader = p_ogl_manager->get_shader("mainframe_composite");
    if (!p_mainframe_composite_shader)
    {
        BOOST_LOG_TRIVIAL(error) << "Invalid mainframe composite shader. Failed to composite main frame.";
        return;
    }
    p_ogl_manager->bind_shader(p_mainframe_composite_shader);

    const int stage = 0;
    p_mainframe_composite_shader->set_uniform("u_sampler", stage);
    glsafe(::glActiveTexture(GL_TEXTURE0 + stage));
    glsafe(::glBindTexture(GL_TEXTURE_2D, output_texture_id));

    s_full_screen_mesh.render_geometry();

    p_ogl_manager->unbind_shader();

    // BBS: end composite mainframe
}

void GLCanvas3D::_debug_draw_camera(const Camera& t_camera)
{
    if (!m_unit_cube.is_initialized()) {
        _init_unit_cube();
    }

    const auto& p_flat_shader = wxGetApp().get_shader("flat");
    if (p_flat_shader) {
        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glLineWidth(2.0f));

        Transform3d final_model_matrix{ Transform3d::Identity() };
        final_model_matrix.data()[0 * 4 + 0] = 2.0f;
        final_model_matrix.data()[1 * 4 + 1] = 2.0f;
        final_model_matrix.data()[2 * 4 + 2] = 2.0f;

        auto vp_matrix = t_camera.get_projection_matrix().matrix() * t_camera.get_view_matrix().matrix();
        auto inv_vp_matrix = vp_matrix.inverse();
        auto m_inv_vp = inv_vp_matrix * final_model_matrix.matrix();
        final_model_matrix = m_inv_vp.eval();

        wxGetApp().bind_shader(p_flat_shader);

        const Camera& camera = get_active_camera();
        const auto& view_matrix = camera.get_view_matrix();
        const auto& proj_matrix = camera.get_projection_matrix();
        auto mv = view_matrix.matrix() * final_model_matrix.matrix();
        Matrix4d final_mv = mv.eval();
        p_flat_shader->set_uniform("view_model_matrix", final_mv);
        p_flat_shader->set_uniform("projection_matrix", proj_matrix);

        m_unit_cube.set_color({ 1.0f, 0.0f, 0.0f, 1.0f });
        m_unit_cube.render_geometry();

        wxGetApp().unbind_shader();
    }
}

void GLCanvas3D::_debug_draw_aabb()
{
    if (!m_unit_cube.is_initialized()) {
        _init_unit_cube();
    }

    std::vector<BoundingBoxf3> bbox_list;

    const auto& partplate_list = wxGetApp().plater()->get_partplate_list().get_plate_list();
    for (int i = 0; i < partplate_list.size(); ++i) {
        const auto& bbox = partplate_list[i]->get_bounding_box(true);
        bbox_list.emplace_back(bbox);
    }

    if (m_gizmos.is_running()) {
        const auto t_aabb = m_gizmos.get_bounding_box();
        if (t_aabb.defined) {
            bbox_list.emplace_back(t_aabb);
        }
    }

    const auto& p_flat_shader = wxGetApp().get_shader("flat");
    if (p_flat_shader) {
        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glLineWidth(2.0f));
        wxGetApp().bind_shader(p_flat_shader);

        const Camera& camera = get_active_camera();
        const auto& view_matrix = camera.get_view_matrix();
        const auto& proj_matrix = camera.get_projection_matrix();
        p_flat_shader->set_uniform("projection_matrix", proj_matrix);
        m_unit_cube.set_color({ 1.0f, 0.0f, 0.0f, 1.0f });

        for (int i = 0; i < bbox_list.size(); ++i) {
            const auto& bbox = bbox_list[i];
            Vec3f b_min = bbox.min.cast<float>();
            Vec3f b_max = bbox.max.cast<float>();
            Vec3f size = bbox.size().cast<float>();
            const auto& center = bbox.center();

            Transform3d final_model_matrix{ Transform3d::Identity() };
            final_model_matrix.data()[3 * 4 + 0] = center(0);
            final_model_matrix.data()[3 * 4 + 1] = center(1);
            final_model_matrix.data()[3 * 4 + 2] = center(2);
            final_model_matrix.data()[0 * 4 + 0] = size(0);
            final_model_matrix.data()[1 * 4 + 1] = size(1);
            final_model_matrix.data()[2 * 4 + 2] = size(2);

            p_flat_shader->set_uniform("view_model_matrix", view_matrix * final_model_matrix);

            m_unit_cube.render_geometry();
        }

        wxGetApp().unbind_shader();
    }
}

void GLCanvas3D::_init_unit_cube()
{
    GLModel::Geometry geo;
    geo.format.type = GLModel::PrimitiveType::Lines;
    geo.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3;

    const float size = 1.0f;
    Vec3f b_min{ -0.5f, -0.5f, -0.5f };
    Vec3f b_max{ 0.5f, 0.5f, 0.5f };
    geo.add_vertex(Vec3f{ b_min(0), b_min(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_min(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_max(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_max(1), b_min(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_min(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_min(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_max(0), b_max(1), b_max(2) });
    geo.add_vertex(Vec3f{ b_min(0), b_max(1), b_max(2) });

    geo.add_line(0, 1);
    geo.add_line(0, 3);
    geo.add_line(0, 4);

    geo.add_line(1, 2);
    geo.add_line(1, 5);

    geo.add_line(2, 3);
    geo.add_line(2, 6);

    geo.add_line(3, 7);

    geo.add_line(4, 5);
    geo.add_line(4, 7);

    geo.add_line(5, 6);

    geo.add_line(6, 7);

    m_unit_cube.init_from(std::move(geo));
}

void GLCanvas3D::_append_to_frame_callback(const FrameCallback& cb)
{
    m_frame_callback_list.emplace_back(cb);
}

void GLCanvas3D::_render_toolbar()
{
    _render_main_toolbar();
    _render_collapse_toolbar();

    //BBS: GUI refactor: GLToolbar
    _render_imgui_select_plate_toolbar();
    _render_assembly_view_thumbnail_toolbar();
    _render_return_toolbar();
    // BBS
    //_render_view_toolbar();
    _render_paint_toolbar();
}

const std::shared_ptr<GLToolbar>& GLCanvas3D::get_main_toolbar() const
{
    if (!m_main_toolbar) {
        m_main_toolbar = std::make_shared<GLToolbar>(GLToolbar::EType::Normal, "Main");
    }
    return m_main_toolbar;
}

void GLCanvas3D::_render_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params,
    PartPlateList& partplate_list, ModelObjectPtrs& model_objects, const GLVolumeCollection& volumes, std::vector<std::array<float, 4>>& extruder_colors,
    const std::shared_ptr<GLShaderProgram>& shader,
    Camera::EType                      camera_type,
    Camera::ViewAngleType              camera_view_angle_type,
    bool                               for_picking,
    bool                               ban_light,
    const ExtraThumbData&              extra_thumb_data)
{
    //BBS modify visible calc function
    static std::array<float, 4>       curr_color;
    static const std::array<float, 4> orange = { 0.923f, 0.504f, 0.264f, 1.0f };
    static const std::array<float, 4> gray = { 0.64f, 0.64f, 0.64f, 1.0f };
    GLVolumePtrs                      visible_volumes;
    BoundingBoxf3                     plate_build_volume;
    if (thumbnail_params.use_plate_box) {
        int           plate_idx = thumbnail_params.plate_id;
        PartPlate* plate = partplate_list.get_plate(plate_idx);
        plate_build_volume      = plate->get_build_volume();

        for (GLVolume* vol : volumes.volumes) {
            if (!vol->is_modifier
                && !vol->is_wipe_tower
                && (!thumbnail_params.parts_only || vol->composite_id.volume_id >= 0)
                && (vol->partly_inside || is_volume_in_plate_boundingbox(*vol, plate_idx, plate_build_volume))) {
                    visible_volumes.emplace_back(vol);
            }
        }

        BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail: plate_idx %1% volumes size %2%, shader %3%, use_top_view=%4%, for_picking=%5%") % plate_idx %
            visible_volumes.size() % shader.get() % (int)camera_view_angle_type % for_picking;
    }
    else {
        visible_volumes = volumes.volumes;
    }
    //BoundingBoxf3 volumes_box = plate_build_volume;
    BoundingBoxf3 volumes_box;
    volumes_box.min.z() = 0;
    volumes_box.max.z() = 0;
    if (!visible_volumes.empty()) {
        for (const GLVolume* vol : visible_volumes) {
            volumes_box.merge(vol->transformed_bounding_box());
        }
    }
    volumes_box.min.z() = -Slic3r::BuildVolume::SceneEpsilon;
    double width = volumes_box.max.x() - volumes_box.min.x();
    double depth = volumes_box.max.y() - volumes_box.min.y();
    double height = volumes_box.max.z() - volumes_box.min.z();
    volumes_box.max.x() = volumes_box.max.x() + width * 0.1f;
    volumes_box.min.x() = volumes_box.min.x() - width * 0.1f;
    volumes_box.max.y() = volumes_box.max.y() + depth * 0.1f;
    volumes_box.min.y() = volumes_box.min.y() - depth * 0.1f;
    volumes_box.max.z() = volumes_box.max.z() + height * 0.2f;
    volumes_box.min.z() = volumes_box.min.z() - height * 0.2f;

    Camera camera;
    camera.set_type(camera_type);
    //BBS modify scene box to plate scene bounding box
    //plate_build_volume.min(2) = - plate_build_volume.max(2);
    if (thumbnail_params.use_plate_box) {
        camera.set_scene_box(plate_build_volume);
    }

    camera.apply_viewport(0, 0, thumbnail_data.width, thumbnail_data.height);

    //BoundingBoxf3 plate_box = plate->get_bounding_box(false);
    //plate_box.min.z() = 0.0;
    //plate_box.max.z() = 0.0;

    if (camera_view_angle_type == Camera::ViewAngleType::Top_Plate) {
        float center_x = (plate_build_volume.max(0) + plate_build_volume.min(0)) / 2;
        float center_y = (plate_build_volume.max(1) + plate_build_volume.min(1)) / 2;
        float distance_z = plate_build_volume.max(2) - plate_build_volume.min(2);
        Vec3d center(center_x, center_y, 0.f);
        double zoom_ratio, scale_x, scale_y;

        scale_x = ((double)thumbnail_data.width) / (plate_build_volume.max(0) - plate_build_volume.min(0));
        scale_y = ((double)thumbnail_data.height) / (plate_build_volume.max(1) - plate_build_volume.min(1));
        zoom_ratio = (scale_x <= scale_y) ? scale_x : scale_y;
        camera.look_at(center + distance_z * Vec3d::UnitZ(), center, Vec3d::UnitY());
        camera.set_zoom(zoom_ratio);
        //camera.select_view("top");
    }
    else {
        //camera.zoom_to_box(volumes_box);

        //const Vec3d& target = camera.get_target();
        //double distance = camera.get_distance();
        //camera.look_at(target - 0.707 * distance * Vec3d::UnitY() + 0.3 * distance * Vec3d::UnitZ(), target, Vec3d::UnitY() + Vec3d::UnitZ());

        //BBS: use original iso view for thumbnail
        camera.select_view(camera_view_angle_type);
        camera.zoom_to_box(volumes_box);
    }

    if (thumbnail_params.use_plate_box) {
        camera.apply_projection(plate_build_volume);
    }
    else {
        camera.apply_projection(volumes_box);
    }
    //double near_z = -1.0;
    //double far_z = -1.0;
    //camera.apply_projection(volumes_box, near_z, far_z);

    //GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (!shader) {
        BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail with invalid shader");
        return;
    }

    //if (thumbnail_params.transparent_background)
    if (for_picking)
        glsafe(::glClearColor(0.f, 0.f, 0.f, 0.f));
    else {
        const auto& bg_color = thumbnail_params.background_color;
        glsafe(::glClearColor(bg_color.x(), bg_color.y(), bg_color.z(), bg_color.w()));
        //glsafe(::glClearColor(0.50f, 0.5f, 0.5f, 1.0f));
        //glsafe(::glClearColor(0.121568f, 0.121568f, 0.121568f, 1.0f));
        //glsafe(::glClearColor(0.17647f, 0.17647f, 0.17647f, 1.0f));
        //glsafe(::glClearColor(0.906f, 0.906f, 0.906f, 1.0f));
        //glsafe(::glClearColor(0.37647f, 0.37647f, 0.37647f, 0.5f)); too lite
        //glsafe(::glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
    }

    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    if (ban_light) {
        glsafe(::glDisable(GL_BLEND));
    }

    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();
    shader->set_uniform("ban_light", ban_light);
    if (for_picking) {
        //if (OpenGLManager::can_multisample())
              // This flag is often ignored by NVIDIA drivers if rendering into a screen buffer.
        //    glsafe(::glDisable(GL_MULTISAMPLE));

        glsafe(::glDisable(GL_BLEND));

        static const GLfloat INV_255 = 1.0f / 255.0f;

        // do not cull backfaces to show broken geometry, if any
        glsafe(::glDisable(GL_CULL_FACE));

        for (GLVolume* vol : visible_volumes) {
            // Object picking mode. Render the object with a color encoding the object index.
            // we reserve color = (0,0,0) for occluders (as the printbed)
            // so we shift volumes' id by 1 to get the proper color
            //BBS: remove the bed picking logic
            unsigned int id = vol->model_object_ID;
            //unsigned int id = 1 + volume.second.first;
            unsigned int r = (id & (0x000000FF << 0)) >> 0;
            unsigned int g = (id & (0x000000FF << 8)) >> 8;
            unsigned int b = (id & (0x000000FF << 16)) >> 16;
            unsigned int a = 0xFF;

            const std::array<float, 4> color{ (GLfloat)r * INV_255, (GLfloat)g * INV_255, (GLfloat)b * INV_255, (GLfloat)a * INV_255 };
            shader->set_uniform("uniform_color", color);

            bool is_active = vol->is_active;
            vol->is_active = true;
            vol->picking = true;

            const Transform3d matrix = view_matrix * vol->world_matrix();
            shader->set_uniform("view_model_matrix", matrix);
            shader->set_uniform("projection_matrix", projection_matrix);
            shader->set_uniform("normal_matrix", (Matrix3d)matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
            shader->set_uniform("volume_world_matrix", vol->world_matrix());
            vol->simple_render(shader, model_objects, extruder_colors);
            vol->is_active = is_active;
            vol->picking = false;
        }
        glsafe(::glEnable(GL_CULL_FACE));

        //if (OpenGLManager::can_multisample())
        //    glsafe(::glEnable(GL_MULTISAMPLE));
    }
    else {
        shader->set_uniform("emission_factor", 0.1f);
        for (GLVolume* vol : visible_volumes) {
            //BBS set render color for thumbnails
            curr_color[0] = vol->color[0];
            curr_color[1] = vol->color[1];
            curr_color[2] = vol->color[2];
            curr_color[3] = vol->color[3];

            std::array<float, 4> new_color = adjust_color_for_rendering(curr_color);
            if (ban_light) {
                new_color[3] = (255 - (vol->extruder_id - 1)) / 255.0f;
            }
            shader->set_uniform("uniform_color", new_color);
            shader->set_uniform("volume_world_matrix", vol->world_matrix());
            //BBS set all volume to orange
            //shader->set_uniform("uniform_color", orange);
            /*if (plate_idx > 0) {
                shader->set_uniform("uniform_color", orange);
            }
            else {
                shader->set_uniform("uniform_color", (vol->printable && !vol->is_outside) ? orange : gray);
            }*/
            // the volume may have been deactivated by an active gizmo
            bool is_active = vol->is_active;
            vol->is_active = true;
            const Transform3d matrix = view_matrix * vol->world_matrix();
            shader->set_uniform("view_model_matrix", matrix);
            shader->set_uniform("projection_matrix", projection_matrix);
            shader->set_uniform("normal_matrix", (Matrix3d)matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
            vol->simple_render(shader, model_objects, extruder_colors, ban_light);
            vol->is_active = is_active;
        }
    }

    glsafe(::glDisable(GL_DEPTH_TEST));

    //don't render plate in thumbnail
    //plate->render( false, true, true);

    // restore background color
    //if (thumbnail_params.transparent_background)
    //    glsafe(::glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
    BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail: finished");
}

void GLCanvas3D::_show_isolated_volumes_notification()
{
    if (s_isolated_volumes.empty())
        return;

    auto* plater = wxGetApp().plater();
    if (!plater)
        return;

    std::string names;
    int count = 0;
    for (const auto& iv : s_isolated_volumes) {
        if (count >= 2 || names.size() > 70) {
            names += "...";
            break;
        }
        if (!names.empty()) names += ", ";
        names += iv.vol->name;
        ++count;
    }
    std::string info_text = _u8L("Overview") + ": " + _u8L("Isolated objects detected") + ": " + names + "\n"
                          + _u8L("Click to move them closer to the main body.");



    NotificationManager* notify_mgr = plater->get_notification_manager();
    notify_mgr->push_notification(NotificationType::BBLIsolatedVolumeInfo,
                                  NotificationManager::NotificationLevel::ImportantNotificationLevel,
                                  info_text, _u8L("Move closer"), &GLCanvas3D::_move_isolated_volumes_closer);
}

void GLCanvas3D::_check_assembly_far_from_origin()
{
    auto *plater = wxGetApp().plater();
    if (!plater) return;

    NotificationManager* notify_mgr = plater->get_notification_manager();
    if (!notify_mgr) return;

    s_far_from_origin_notification_shown = true;

    bool is_far = false;
    if (s_bvh_primary_bounds.defined) {
        const Vec3d origin  = Vec3d::Zero();
        if (!s_bvh_primary_bounds.contains(origin)) {
            const Vec3d clamped = s_bvh_primary_bounds.min.cwiseMax(origin).cwiseMin(s_bvh_primary_bounds.max);
            is_far = (origin - clamped).norm() > 1000.0;
        }
    }

    if (is_far) {
        std::string info_text = _u8L("The main assembly bounding box is too far from the world origin. Reset assembly relationships and move to origin?");
        notify_mgr->push_notification(NotificationType::BBLAssemblyFarFromOrigin,
                                      NotificationManager::NotificationLevel::ImportantNotificationLevel,
                                      info_text, _u8L("Reset to origin"), &GLCanvas3D::_reset_assembly_to_origin);
    } else {
        notify_mgr->close_notification_of_type(NotificationType::BBLAssemblyFarFromOrigin);
    }
}

bool GLCanvas3D::_reset_assembly_to_origin(wxEvtHandler*)
{
    auto* plater = wxGetApp().plater();
    if (!plater) return false;
    Model& model = plater->model();

    plater->take_snapshot("reset all volumes to assembly origin", UndoRedo::SnapshotType::GizmoAction);

    for (ModelObject* obj : model.objects) {
        for (ModelInstance* inst : obj->instances) {
            Geometry::Transformation trafo = inst->get_assemble_transformation();
            trafo.set_offset(Vec3d::Zero());
            inst->set_assemble_transformation(trafo);
        }
    }
    s_bvh_primary_bounds.reset();
    s_far_from_origin_notification_shown = false;
    plater->get_partplate_list().reset_thumbnail_assembly_view_data();
    plater->update();
    return false;
}

bool GLCanvas3D::_move_isolated_volumes_closer(wxEvtHandler*)
{
    auto* plater = wxGetApp().plater();
    if (!plater) return false;
    Model& model = plater->model();

    const Vec3d  box_center  = s_bvh_primary_bounds.center();
    const double target_dist = 30.0;

    plater->take_snapshot("Move isolated volumes", UndoRedo::SnapshotType::GizmoAction);

    for (const auto& iv : s_isolated_volumes) {
        if (!iv.vol || iv.obj_idx < 0 || iv.obj_idx >= (int) model.objects.size()) continue;

        const int inst_idx = iv.vol->instance_idx();
        ModelObject* obj = model.objects[iv.obj_idx];
        if (inst_idx < 0 || inst_idx >= (int) obj->instances.size()) continue;
        ModelInstance* inst = obj->instances[inst_idx];

        const Vec3d world_center = iv.world_box_assembly.center();
        const Vec3d obj_half    = iv.world_box_assembly.size() * 0.5;

        Vec3d delta = Vec3d::Zero();
        for (int axis = 0; axis < 3; ++axis) {
            const double obj_min = world_center(axis) - obj_half(axis);
            const double obj_max = world_center(axis) + obj_half(axis);
            const double pri_min = s_bvh_primary_bounds.min(axis);
            const double pri_max = s_bvh_primary_bounds.max(axis);

            if (obj_max < pri_min - target_dist) {
                delta(axis) = (pri_min - target_dist - obj_half(axis)) - world_center(axis);
            } else if (obj_min > pri_max + target_dist) {
                delta(axis) = (pri_max + target_dist + obj_half(axis)) - world_center(axis);
            }
        }

        if (delta.squaredNorm() < 1e-3) continue;

        Geometry::Transformation new_trafo = inst->get_assemble_transformation();
        new_trafo.set_offset(new_trafo.get_offset() + delta);
        inst->set_assemble_transformation(new_trafo);
    }

    GLCanvas3D* canvas = plater->get_current_canvas3D();
    if (canvas && canvas->get_canvas_type() == ECanvasType::CanvasAssembleView) {
        Selection& sel = canvas->get_selection();
        sel.clear();
        for (const auto& iv : s_isolated_volumes) {
            if (iv.obj_idx >= 0 && iv.obj_idx < (int) model.objects.size())
                sel.add_object((unsigned int) iv.obj_idx, false);
        }
    }

    s_isolated_volumes.clear();
    s_isolated_notification_shown = false;
    s_intersects_notification_shown = false;
    plater->get_partplate_list().reset_thumbnail_assembly_view_data();
    plater->update();
    return false;
}

static void _collect_bvh_subtree_prims(const tinybvh::BVH& bvh, uint32_t node_idx, std::vector<uint32_t>& prim_indices)
{
    const tinybvh::BVH::BVHNode& node = bvh.bvhNode[node_idx];
    if (node.isLeaf()) {
        for (uint32_t i = 0; i < node.triCount; ++i)
            prim_indices.emplace_back(bvh.primIdx[node.leftFirst + i]);
        return;
    }
    _collect_bvh_subtree_prims(bvh, node.leftFirst, prim_indices);
    _collect_bvh_subtree_prims(bvh, node.leftFirst + 1, prim_indices);
}

static BoundingBoxf3 _bvh_node_bounds(const tinybvh::BVH& bvh, uint32_t node_idx)
{
    const tinybvh::BVH::BVHNode& n = bvh.bvhNode[node_idx];
    BoundingBoxf3 b;
    b.min = Vec3d(n.aabbMin.x, n.aabbMin.y, n.aabbMin.z);
    b.max = Vec3d(n.aabbMax.x, n.aabbMax.y, n.aabbMax.z);
    b.defined = true;
    return b;
}

static BoundingBoxf3 _expand_bounds(const BoundingBoxf3& box, double dist)
{
    BoundingBoxf3 expanded = box;
    expanded.min -= Vec3d(dist, dist, dist);
    expanded.max += Vec3d(dist, dist, dist);
    expanded.defined = true;
    return expanded;
}

static uint32_t _find_largest_leaf_node_by_volume(const tinybvh::BVH& bvh, uint32_t node_idx, uint32_t depth,
                                        double& max_volume, uint32_t& max_depth)
{
    const tinybvh::BVH::BVHNode& node = bvh.bvhNode[node_idx];
    if (node.isLeaf()) {
        const double node_volume = _bvh_node_bounds(bvh, node_idx).volume();
        constexpr double eps = 1e-6;
        if (node_volume > max_volume + eps ||
            (node_volume > max_volume - eps && depth > max_depth)) {
            max_volume = node_volume;
            max_depth  = depth;
            return node_idx;
        }
        return uint32_t(-1);
    }

    uint32_t best_idx = _find_largest_leaf_node_by_volume(bvh, node.leftFirst, depth + 1, max_volume, max_depth);
    const uint32_t right_best_idx = _find_largest_leaf_node_by_volume(bvh, node.leftFirst + 1, depth + 1, max_volume, max_depth);
    if (right_best_idx != uint32_t(-1)) {
        best_idx = right_best_idx;
    }
    return best_idx;
}

// Check if any leaf in subtree A overlaps any leaf in subtree B (cross-subtree test).
static bool _bvh_cross_intersect(const tinybvh::BVH& bvh, uint32_t idx_a, uint32_t idx_b)
{
    if (!_bvh_node_bounds(bvh, idx_a).intersects(_bvh_node_bounds(bvh, idx_b)))
        return false;
    const tinybvh::BVH::BVHNode& a = bvh.bvhNode[idx_a];
    const tinybvh::BVH::BVHNode& b = bvh.bvhNode[idx_b];
    if (a.isLeaf() && b.isLeaf())
        return true;
    if (a.isLeaf())
        return _bvh_cross_intersect(bvh, idx_a, b.leftFirst) ||
               _bvh_cross_intersect(bvh, idx_a, b.leftFirst + 1);
    return _bvh_cross_intersect(bvh, a.leftFirst, idx_b) ||
           _bvh_cross_intersect(bvh, a.leftFirst + 1, idx_b);
}

// Check if any two distinct primitives within this subtree have overlapping bounds.
static bool _bvh_has_self_intersection(const tinybvh::BVH& bvh, uint32_t root_idx,
                                       const std::vector<BoundingBoxf3>& prim_boxes)
{
    std::vector<uint32_t> stack;
    stack.push_back(root_idx);
    while (!stack.empty()) {
        const uint32_t cur = stack.back();
        stack.pop_back();
        const tinybvh::BVH::BVHNode& node = bvh.bvhNode[cur];
        if (node.isLeaf()) {
            if (node.triCount <= 1)
                continue;
            for (uint32_t i = 0; i < node.triCount; ++i) {
                const uint32_t pi = bvh.primIdx[node.leftFirst + i];
                if (pi >= prim_boxes.size()) continue;
                for (uint32_t j = i + 1; j < node.triCount; ++j) {
                    const uint32_t pj = bvh.primIdx[node.leftFirst + j];
                    if (pj >= prim_boxes.size()) continue;
                    if (prim_boxes[pi].intersects(prim_boxes[pj]))
                        return true;
                }
            }
            continue;
        }
        const uint32_t left  = node.leftFirst;
        const uint32_t right = node.leftFirst + 1;
        if (_bvh_cross_intersect(bvh, left, right))
            return true;
        stack.push_back(left);
        stack.push_back(right);
    }
    return false;
}

static void _check_and_exclude_bvh_node(const tinybvh::BVH&               bvh,
                                         uint32_t                           node_idx,
                                         uint32_t                           primary_idx,
                                         BoundingBoxf3&                     primary_bounds,
                                         double                             threshold_dist,
                                         const std::vector<GLVolume*>&      candidate_volumes,
                                         const std::vector<BoundingBoxf3>&  candidate_boxes,
                                         std::vector<bool>&                 include_flags)
{
    if (node_idx == primary_idx) {
        return;
    }

    const tinybvh::BVH::BVHNode& node = bvh.bvhNode[node_idx];
    if (node.isLeaf()) {
        const BoundingBoxf3 bounds         = _bvh_node_bounds(bvh, node_idx);
        const Vec3d         primary_center = primary_bounds.center();
        const double        dist           = (bounds.center() - primary_center).norm();
        const bool          intersects     = bounds.intersects(primary_bounds);
        if (intersects) {
            GLCanvas3D::s_bvh_primary_bounds.merge(bounds);
            primary_bounds = _expand_bounds(GLCanvas3D::s_bvh_primary_bounds, GLCanvas3D::s_expand_bvh_box_dist);
#if !BBL_RELEASE_TO_PUBLIC
            BOOST_LOG_TRIVIAL(info) << boost::format("assembly thumbnail BVH merge leaf=%1% dist=%2% => primary expanded to min(%3%,%4%,%5%) max(%6%,%7%,%8%)") % node_idx %
                                           dist % primary_bounds.min.x() % primary_bounds.min.y() % primary_bounds.min.z() % primary_bounds.max.x() % primary_bounds.max.y() %
                                           primary_bounds.max.z();
#endif
            return;
        }

        if (dist > threshold_dist) {
            std::vector<uint32_t> prim_indices;
            _collect_bvh_subtree_prims(bvh, node_idx, prim_indices);

            std::string names;
            for (uint32_t pi : prim_indices) {
                if (pi < candidate_volumes.size()) {
                    if (!names.empty()) names += ", ";
                    names += candidate_volumes[pi]->name;
                }
            }
#if !BBL_RELEASE_TO_PUBLIC
            BOOST_LOG_TRIVIAL(info) << boost::format("assembly thumbnail BVH exclude leaf=%1% name=[%2%] dist=%3%") % node_idx % names % dist;
#endif
            for (uint32_t pi : prim_indices) {
                if (pi < include_flags.size()) include_flags[pi] = false;
                if (pi < candidate_volumes.size()) {
                    GLVolume *vol     = candidate_volumes[pi];
                    const int oid     = vol->object_idx();
                    bool      already = false;
                    for (const auto &iv : GLCanvas3D::s_isolated_volumes) {
                        if (iv.obj_idx == oid) {
                            already = true;
                            break;
                        }
                    }
                    if (!already) {
                        GLCanvas3D::s_isolated_volumes.push_back({vol, oid, candidate_boxes[pi]});
                        const auto &back = GLCanvas3D::s_isolated_volumes.back();
                        BOOST_LOG_TRIVIAL(info) << boost::format("assembly thumbnail BVH isolated: obj_idx=%1% name=%2% stored_obj_idx=%3%") % oid % vol->name % back.obj_idx;
                    }
                }
            }
        }
        return;
    }

    _check_and_exclude_bvh_node(bvh, node.leftFirst,     primary_idx, primary_bounds, threshold_dist, candidate_volumes, candidate_boxes, include_flags);
    _check_and_exclude_bvh_node(bvh, node.leftFirst + 1, primary_idx, primary_bounds, threshold_dist, candidate_volumes, candidate_boxes, include_flags);
}
constexpr double c_bvh_expand_dist = 3.0;
static void _reclaim_isolated_volumes_by_bvh(
    const std::vector<GLVolume*>&                    candidate_volumes,
    const std::vector<BoundingBoxf3>&                candidate_boxes,
    std::vector<bool>&                               include_flags,
    const std::unordered_map<int, std::vector<size_t>>& obj_to_candidates,
    int&                                             out_iterations)
{
    constexpr int    max_reclaim_iters  = 3;
    constexpr uint32_t PRIMARY_PRIM = 0;
    out_iterations = 0;

    for (int iter = 0; iter < max_reclaim_iters; ++iter) {
        ++out_iterations;
        const size_t iso_count = GLCanvas3D::s_isolated_volumes.size();

        // Build AABB array: [0] = primary bounds, [1..iso_count] = isolated volumes
        std::vector<tinybvh::bvhvec4> iso_aabbs;
        iso_aabbs.reserve((1 + iso_count) * 2);

        {
            const BoundingBoxf3 pb = _expand_bounds(GLCanvas3D::s_bvh_primary_bounds, c_bvh_expand_dist);
            iso_aabbs.emplace_back((float) pb.min.x(), (float) pb.min.y(), (float) pb.min.z(), 0.0f);
            iso_aabbs.emplace_back((float) pb.max.x(), (float) pb.max.y(), (float) pb.max.z(), 0.0f);
        }
        for (const auto& iv : GLCanvas3D::s_isolated_volumes) {
            const BoundingBoxf3 eb = _expand_bounds(iv.world_box_assembly, c_bvh_expand_dist);
            iso_aabbs.emplace_back((float) eb.min.x(), (float) eb.min.y(), (float) eb.min.z(), 0.0f);
            iso_aabbs.emplace_back((float) eb.max.x(), (float) eb.max.y(), (float) eb.max.z(), 0.0f);
        }

        tinybvh::BVH iso_bvh;
        iso_bvh.BuildAABB(iso_aabbs.data(), (uint32_t) (1 + iso_count));

        GLCanvas3D::s_bvh_expanded_bounds = _expand_bounds(GLCanvas3D::s_bvh_primary_bounds, GLCanvas3D::s_expand_bvh_box_dist);
        std::vector<bool> reclaimed(iso_count, false);
        bool              reclaimed_any = false;

        auto reclaim_dfs = [&](auto&& self, uint32_t node_idx) -> void {
            const auto& node = iso_bvh.bvhNode[node_idx];
            BoundingBoxf3 node_box = _bvh_node_bounds(iso_bvh, node_idx);
            if (!node_box.intersects(GLCanvas3D::s_bvh_expanded_bounds))
                return;

            if (node.isLeaf()) {
                for (uint32_t i = 0; i < node.triCount; ++i) {
                    const uint32_t raw_pi = iso_bvh.primIdx[node.leftFirst + i];
                    if (raw_pi == PRIMARY_PRIM)
                        continue;
                    const uint32_t pi = raw_pi - 1; // map back to s_isolated_volumes index
                    if (pi >= iso_count || reclaimed[pi])
                        continue;
                    if (!GLCanvas3D::s_bvh_expanded_bounds.intersects(GLCanvas3D::s_isolated_volumes[pi].world_box_assembly))
                        continue;

                    reclaimed[pi]  = true;
                    reclaimed_any  = true;
                    const int oid  = GLCanvas3D::s_isolated_volumes[pi].obj_idx;
                    auto map_it    = obj_to_candidates.find(oid);
                    if (map_it != obj_to_candidates.end()) {
                        for (size_t j : map_it->second) {
                            include_flags[j] = true;
                            GLCanvas3D::s_bvh_primary_bounds.merge(candidate_boxes[j]);
                        }
                    }
                    GLCanvas3D::s_bvh_expanded_bounds = _expand_bounds(GLCanvas3D::s_bvh_primary_bounds, GLCanvas3D::s_expand_bvh_box_dist);
#if !BBL_RELEASE_TO_PUBLIC
                    BOOST_LOG_TRIVIAL(info) << boost::format("assembly BVH: reclaimed isolated vol obj_idx=%1% name=%2% (bvh pass %3%)")
                        % oid % GLCanvas3D::s_isolated_volumes[pi].vol->name % iter;
#endif
                }
                return;
            }
            self(self, node.leftFirst);
            self(self, node.leftFirst + 1);
        };
        reclaim_dfs(reclaim_dfs, 0);

        if (!reclaimed_any)
            break;

        std::vector<GLCanvas3D::IsolatedVolumeInfo> remaining;
        remaining.reserve(iso_count);
        for (size_t i = 0; i < iso_count; ++i) {
            if (!reclaimed[i])
                remaining.push_back(std::move(GLCanvas3D::s_isolated_volumes[i]));
        }
        GLCanvas3D::s_isolated_volumes = std::move(remaining);

        if (GLCanvas3D::s_isolated_volumes.empty())
            break;
    }
    GLCanvas3D::s_bvh_expanded_bounds = _expand_bounds(GLCanvas3D::s_bvh_primary_bounds, GLCanvas3D::s_expand_bvh_box_dist);
}


void GLCanvas3D::_filter_assembly_thumbnail_candidates_by_bvh(const std::vector<GLVolume*>& assemble_candidate_volumes,
    const std::vector<BoundingBoxf3>&  assemble_candidate_boxes,
    bool                               skip_single_volume_bvh,
    bool                               rebuild_bvh,
    std::vector<bool>&                 include_candidate_volumes)
{
    if (skip_single_volume_bvh || !rebuild_bvh || assemble_candidate_volumes.size() <= 1) {
        return;
    }

    std::vector<BoundingBoxf3> expanded_boxes;
    expanded_boxes.reserve(assemble_candidate_boxes.size());
    for (const BoundingBoxf3& box : assemble_candidate_boxes)
        expanded_boxes.emplace_back(_expand_bounds(box, c_bvh_expand_dist));

    std::vector<tinybvh::bvhvec4> volume_aabbs;
    volume_aabbs.reserve(expanded_boxes.size() * 2);
    for (const BoundingBoxf3& box : expanded_boxes) {
        volume_aabbs.emplace_back((float)box.min.x(), (float)box.min.y(), (float)box.min.z(), 0.0f);
        volume_aabbs.emplace_back((float)box.max.x(), (float)box.max.y(), (float)box.max.z(), 0.0f);
    }

    tinybvh::BVH volume_bvh;
    volume_bvh.BuildAABB(volume_aabbs.data(), (uint32_t)assemble_candidate_volumes.size());

    BOOST_LOG_TRIVIAL(info) << "assembly thumbnail BVH tree:";
    auto dump_bvh_node = [&volume_bvh, &assemble_candidate_volumes](auto&& self, uint32_t node_idx, const std::string& prefix, bool is_last, bool is_root) -> void {
        const tinybvh::BVH::BVHNode& node         = volume_bvh.bvhNode[node_idx];
        const std::string            branch       = is_root ? "" : (is_last ? "`- " : "|- ");
        const std::string            child_prefix = is_root ? "" : prefix + (is_last ? "   " : "|  ");
        std::string                  node_name    = is_root ? "RootNode" : (node.isLeaf() ? "LeafNode_" : "MergeNode_") + std::to_string(node_idx);

        if (node.isLeaf()) {
            std::string volume_names;
            for (uint32_t i = 0; i < node.triCount; ++i) {
                const uint32_t prim_idx = volume_bvh.primIdx[node.leftFirst + i];
                if (prim_idx >= assemble_candidate_volumes.size()) {
                    continue;
                }
                if (!volume_names.empty()) {
                    volume_names += ", ";
                }
                volume_names += assemble_candidate_volumes[prim_idx]->name;
            }
            if (!volume_names.empty()) {
                node_name += " [" + volume_names + "]";
            }
        }

        BOOST_LOG_TRIVIAL(info) << boost::format("assembly thumbnail BVH %1%%2%") % prefix % (branch + node_name);
        BOOST_LOG_TRIVIAL(info) << boost::format("assembly thumbnail BVH %1%min:%2%,%3%,%4%; max:%5%,%6%,%7%")
                                       % child_prefix % node.aabbMin.x % node.aabbMin.y % node.aabbMin.z % node.aabbMax.x % node.aabbMax.y %
                                       node.aabbMax.z;

        if (node.isLeaf()) {
            return;
        }

        self(self, node.leftFirst, child_prefix, false, false);
        self(self, node.leftFirst + 1, child_prefix, true, false);
    };
    dump_bvh_node(dump_bvh_node, 0, "", true, true);

    const bool has_intersection = _bvh_has_self_intersection(volume_bvh, 0, expanded_boxes);
    if (!has_intersection) {
        BOOST_LOG_TRIVIAL(info) << "assembly thumbnail BVH: no leaf pairs intersect, skipping (ratio=0)";
        s_assemble_candidate_volumes_size = 0;
        s_assemble_ratio        = 0;
        s_assemble_volume_ratio = 0;
        s_isolated_volumes.clear();
        return;
    }

    double max_leaf_volume = -1.0;
    uint32_t max_leaf_depth = 0;
    const uint32_t primary_idx = _find_largest_leaf_node_by_volume(volume_bvh, 0, 0, max_leaf_volume, max_leaf_depth);
    if (primary_idx == uint32_t(-1)) {
        return;
    }
    BoundingBoxf3 primary_bounds = _bvh_node_bounds(volume_bvh, primary_idx);
    s_first_primary_bounds = primary_bounds;

    const double        threshold_dist = 100.0;

    BOOST_LOG_TRIVIAL(info) << boost::format("assembly thumbnail BVH primary leaf=%1% volume=%2%") % primary_idx % max_leaf_volume;

    s_isolated_volumes.clear();
    s_isolated_notification_shown = false;
    s_intersects_notification_shown = false;
    s_far_from_origin_notification_shown = false;
    s_isolated_volumes.reserve(assemble_candidate_volumes.size());
    s_bvh_primary_bounds = primary_bounds;
    s_bvh_expanded_bounds = _expand_bounds(primary_bounds, s_expand_bvh_box_dist);

    _check_and_exclude_bvh_node(volume_bvh, 0, primary_idx, s_bvh_expanded_bounds, threshold_dist,
                                assemble_candidate_volumes, assemble_candidate_boxes, include_candidate_volumes);

    if (!s_isolated_volumes.empty()) {
        std::unordered_map<int, std::vector<size_t>> obj_to_candidates;
        obj_to_candidates.reserve(assemble_candidate_volumes.size());
        for (size_t j = 0; j < assemble_candidate_volumes.size(); ++j)
            obj_to_candidates[assemble_candidate_volumes[j]->object_idx()].push_back(j);

        int reclaim_iters = 0;
        const auto reclaim_t0 = std::chrono::steady_clock::now();
        _reclaim_isolated_volumes_by_bvh(assemble_candidate_volumes, assemble_candidate_boxes,
                                         include_candidate_volumes, obj_to_candidates, reclaim_iters);
        const auto reclaim_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - reclaim_t0).count();
        BOOST_LOG_TRIVIAL(info) << boost::format("assembly BVH: reclaim_isolated_by_bvh done in %1%ms, iterations=%2%, remaining_isolated=%3%")
            % reclaim_ms % reclaim_iters % s_isolated_volumes.size();
    }

    for (int i = (int) s_isolated_volumes.size() - 1; i >= 0; --i) {
        auto is_intersects = s_bvh_expanded_bounds.intersects(s_isolated_volumes[i].world_box_assembly);
        if (s_bvh_expanded_bounds.contains(s_isolated_volumes[i].world_box_assembly) || is_intersects) {
            BOOST_LOG_TRIVIAL(info) << boost::format("assembly BVH: removing false-positive isolated vol obj_idx=%1% (inside expanded bounds)") % s_isolated_volumes[i].obj_idx;
            s_isolated_volumes.erase(s_isolated_volumes.begin() + i);
        }
    }
    const double primary_bounds_volume = s_bvh_primary_bounds.volume();
    double total_volume = primary_bounds_volume;
    for (const IsolatedVolumeInfo& isolated_volume : s_isolated_volumes) {
        if (isolated_volume.world_box_assembly.defined) {
            total_volume += isolated_volume.world_box_assembly.volume();
        }
    }
    s_assemble_volume_ratio = int(primary_bounds_volume * 100.0 / total_volume + 0.01);
}

static long long s_last_bvh_filter_ms = 0;

void GLCanvas3D::_render_assembly_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params,
    PartPlateList& partplate_list, ModelObjectPtrs& model_objects, const GLVolumeCollection& volumes, std::vector<std::array<float, 4>>& extruder_colors,
    const std::shared_ptr<GLShaderProgram>& shader,
    Camera::EType                      camera_type,
    Camera::ViewAngleType              camera_view_angle_type,
    bool                               for_picking,
    bool                               ban_light,
    const ExtraThumbData&              extra_thumb_data)
{
    bool is_assemble_view_thumbnail = extra_thumb_data.canvas_type == ECanvasType::CanvasAssembleView;
    if (!is_assemble_view_thumbnail) {
        return;
    }
    const auto render_assembly_t0 = std::chrono::steady_clock::now();
    //BBS modify visible calc function
    static std::array<float, 4>       curr_color;
    static const std::array<float, 4> orange = { 0.923f, 0.504f, 0.264f, 1.0f };
    static const std::array<float, 4> gray = { 0.64f, 0.64f, 0.64f, 1.0f };
    GLVolumePtrs                      visible_volumes;

    struct VolumeTransformBackup {
        GLVolume*                volume{ nullptr };
        Geometry::Transformation instance_transformation;
        Geometry::Transformation volume_transformation;
        Vec3d                    offset_to_assembly{ 0.0, 0.0, 0.0 };
    };
    std::vector<VolumeTransformBackup> assemble_volume_backups;
    auto restore_assemble_volume_transforms = [&assemble_volume_backups]() {
        for (const VolumeTransformBackup& backup : assemble_volume_backups) {
            backup.volume->set_instance_transformation(backup.instance_transformation);
            backup.volume->set_volume_transformation(backup.volume_transformation);
            backup.volume->set_offset_to_assembly(backup.offset_to_assembly);
        }
    };
    std::vector<std::pair<GLVolume*, Vec3d>> thumbnail_offset_backups;
    auto restore_thumbnail_offsets = [&thumbnail_offset_backups]() {
        for (const auto& backup : thumbnail_offset_backups) {
            backup.first->set_instance_offset(backup.second);
        }
    };

    BoundingBoxf3                     plate_build_volume;
    if (thumbnail_params.use_plate_box) {
        int           plate_idx = thumbnail_params.plate_id;
        PartPlate* plate = partplate_list.get_plate(plate_idx);
        plate_build_volume      = plate->get_build_volume();

        for (GLVolume* vol : volumes.volumes) {
            if (!vol->is_modifier
                && !vol->is_wipe_tower
                && (!thumbnail_params.parts_only || vol->composite_id.volume_id >= 0)
                && (vol->partly_inside || is_volume_in_plate_boundingbox(*vol, plate_idx, plate_build_volume))) {
                    visible_volumes.emplace_back(vol);
            }
        }

        BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail: plate_idx %1% volumes size %2%, shader %3%, use_top_view=%4%, for_picking=%5%") % plate_idx %
            visible_volumes.size() % shader.get() % (int)camera_view_angle_type % for_picking;
    }
    else {
        std::vector<GLVolume *>     assemble_candidate_volumes;
        std::vector<BoundingBoxf3>  assemble_candidate_boxes;
        bool skip_single_volume_bvh = volumes.volumes.size() == 1 && !volumes.volumes.front()->is_modifier && !volumes.volumes.front()->is_wipe_tower;
        if (!s_enable_bvh) {
            skip_single_volume_bvh = true;
        }
        assemble_candidate_volumes.reserve(volumes.volumes.size());
        assemble_candidate_boxes.reserve(volumes.volumes.size());

        for (GLVolume *vol : volumes.volumes) {
            if (vol->is_modifier || vol->is_wipe_tower) {
                continue;
            }

            const int obj_idx  = vol->object_idx();
            const int inst_idx = vol->instance_idx();
            const int vol_idx  = vol->volume_idx();
            if (obj_idx >= 0 && obj_idx < (int) model_objects.size()) {
                ModelObject *model_object = model_objects[obj_idx];
                if (model_object != nullptr && inst_idx >= 0 && inst_idx < (int) model_object->instances.size() && vol_idx >= 0 &&
                    vol_idx < (int) model_object->volumes.size()) {
                    assemble_volume_backups.emplace_back(
                        VolumeTransformBackup{vol, vol->get_instance_transformation(), vol->get_volume_transformation(), vol->get_offset_to_assembly()});
                    vol->set_instance_transformation(model_object->instances[inst_idx]->get_assemble_transformation());
                    vol->set_volume_transformation(model_object->volumes[vol_idx]->get_transformation());
                    vol->set_offset_to_assembly(model_object->instances[inst_idx]->get_offset_to_assembly());
                }
            }
            assemble_candidate_volumes.emplace_back(vol);
            assemble_candidate_boxes.emplace_back(vol->transformed_bounding_box());
        }
        s_assemble_candidate_volumes_size = assemble_candidate_volumes.size();
        std::vector<bool> include_candidate_volumes(assemble_candidate_volumes.size(), true);
        const auto bvh_t0 = std::chrono::steady_clock::now();
        _filter_assembly_thumbnail_candidates_by_bvh(assemble_candidate_volumes, assemble_candidate_boxes, skip_single_volume_bvh, extra_thumb_data.rebuild_bvh, include_candidate_volumes);
        s_last_bvh_filter_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - bvh_t0).count();

        for (size_t i = 0; i < assemble_candidate_volumes.size(); ++i) {
            if (include_candidate_volumes[i]) {
                visible_volumes.emplace_back(assemble_candidate_volumes[i]);
            }
        }
    }
    // Translate to positive space for thumbnail rendering (same idea as ObjColorDialog).
    BoundingBoxf3 visible_raw_box;
    if (!visible_volumes.empty()) {
        for (const GLVolume* vol : visible_volumes) {
            visible_raw_box.merge(vol->transformed_bounding_box());
        }
    }
    if (visible_raw_box.defined &&
        (visible_raw_box.min.x() < 0.0 || visible_raw_box.min.y() < 0.0 || visible_raw_box.min.z() < 0.0)) {
        const Vec3d thumbnail_offset(
            visible_raw_box.min.x() < 0.0 ? -visible_raw_box.min.x() : 0.0,
            visible_raw_box.min.y() < 0.0 ? -visible_raw_box.min.y() : 0.0,
            visible_raw_box.min.z() < 0.0 ? -visible_raw_box.min.z() : 0.0);
        thumbnail_offset_backups.reserve(visible_volumes.size());
        for (GLVolume* vol : visible_volumes) {
            const Vec3d old_offset = vol->get_instance_offset();
            thumbnail_offset_backups.emplace_back(vol, old_offset);
            vol->set_instance_offset(old_offset + thumbnail_offset);
        }
    }

    //BoundingBoxf3 volumes_box = plate_build_volume;
    BoundingBoxf3 volumes_box;
    volumes_box.min.z() = 0;
    volumes_box.max.z() = 0;
    if (!visible_volumes.empty()) {
        for (const GLVolume* vol : visible_volumes) {
            volumes_box.merge(vol->transformed_bounding_box());
        }
    }
    volumes_box.min.z() = -Slic3r::BuildVolume::SceneEpsilon;
    double width = volumes_box.max.x() - volumes_box.min.x();
    double depth = volumes_box.max.y() - volumes_box.min.y();
    double height = volumes_box.max.z() - volumes_box.min.z();
    volumes_box.max.x() = volumes_box.max.x() + width * 0.1f;
    volumes_box.min.x() = volumes_box.min.x() - width * 0.1f;
    volumes_box.max.y() = volumes_box.max.y() + depth * 0.1f;
    volumes_box.min.y() = volumes_box.min.y() - depth * 0.1f;
    volumes_box.max.z() = volumes_box.max.z() + height * 0.2f;
    volumes_box.min.z() = volumes_box.min.z() - height * 0.2f;

    Camera camera;
    camera.set_type(camera_type);
    //BBS modify scene box to plate scene bounding box
    if (thumbnail_params.use_plate_box) {
        camera.set_scene_box(plate_build_volume);
    }
    camera.apply_viewport(0, 0, thumbnail_data.width, thumbnail_data.height);
    {//BBS: use original iso view for thumbnail
        camera.select_view(camera_view_angle_type);
        camera.zoom_to_box(volumes_box);
    }

    if (thumbnail_params.use_plate_box) {
        camera.apply_projection(plate_build_volume);
    }
    else {
        camera.apply_projection(volumes_box);
    }
    //GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (!shader) {
        BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail with invalid shader");
        restore_thumbnail_offsets();
        restore_assemble_volume_transforms();
        return;
    }
    //set background
    {
        const auto& bg_color = thumbnail_params.background_color;
        glsafe(::glClearColor(bg_color.x(), bg_color.y(), bg_color.z(), bg_color.w()));
    }

    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    if (ban_light) {
        glsafe(::glDisable(GL_BLEND));
    }

    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();
    shader->set_uniform("ban_light", ban_light);
    {//render
        shader->set_uniform("emission_factor", 0.1f);
        for (GLVolume* vol : visible_volumes) {
            //BBS set render color for thumbnails
            curr_color[0] = vol->color[0];
            curr_color[1] = vol->color[1];
            curr_color[2] = vol->color[2];
            curr_color[3] = vol->color[3];

            std::array<float, 4> new_color = adjust_color_for_rendering(curr_color);
            if (ban_light) {
                new_color[3] = (255 - (vol->extruder_id - 1)) / 255.0f;
            }
            shader->set_uniform("uniform_color", new_color);
            shader->set_uniform("volume_world_matrix", vol->world_matrix());
            //BBS set all volume to orange
            //shader->set_uniform("uniform_color", orange);
            /*if (plate_idx > 0) {
                shader->set_uniform("uniform_color", orange);
            }
            else {
                shader->set_uniform("uniform_color", (vol->printable && !vol->is_outside) ? orange : gray);
            }*/
            // the volume may have been deactivated by an active gizmo
            bool is_active = vol->is_active;
            vol->is_active = true;
            const Transform3d matrix = view_matrix * vol->world_matrix();
            shader->set_uniform("view_model_matrix", matrix);
            shader->set_uniform("projection_matrix", projection_matrix);
            shader->set_uniform("normal_matrix", (Matrix3d)matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
            vol->simple_render(shader, model_objects, extruder_colors, ban_light);
            vol->is_active = is_active;
        }
    }
    glsafe(::glDisable(GL_DEPTH_TEST));
    restore_thumbnail_offsets();
    restore_assemble_volume_transforms();

    BOOST_LOG_TRIVIAL(info) << boost::format("render assembly thumbnail: finished, total=%1%ms, bvh_filter=%2%ms")
                                   % std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - render_assembly_t0).count()
                                   % s_last_bvh_filter_ms;
}

void GLCanvas3D::_render_custom_thumbnail_internal(ThumbnailData &                         thumbnail_data,
                                            const ThumbnailsParams &                thumbnail_params,
                                            PartPlateList &                         partplate_list,
                                            ModelObjectPtrs &                       model_objects,
                                            const GLVolumeCollection &              volumes,
                                            std::vector<std::array<float, 4>> &     extruder_colors,
                                            const std::shared_ptr<GLShaderProgram> &shader,
                                            Camera::EType                           camera_type,
                                            Camera::ViewAngleType                   camera_view_angle_type,
                                            bool                                    for_picking,
                                                   bool                                    ban_light,
                                                   const ExtraThumbData&                   extra_thumb_data)
{
    static std::array<float, 4>       curr_color;
    static const std::array<float, 4> orange = {0.923f, 0.504f, 0.264f, 1.0f};
    static const std::array<float, 4> gray   = {0.64f, 0.64f, 0.64f, 1.0f};
    GLVolumePtrs                      visible_volumes;
    GLVolumePtrs                      visible_render_mesh_volumes;
    GLVolumePtrs                      visible_render_vertice_volumes;
    BoundingBoxf3                     plate_build_volume;
    if (thumbnail_params.use_plate_box) {
        int        plate_idx = thumbnail_params.plate_id;
        PartPlate *plate     = partplate_list.get_plate(plate_idx);
        plate_build_volume   = plate->get_build_volume();

        for (GLVolume *vol : volumes.volumes) {
            if (!vol->is_modifier && !vol->is_wipe_tower && (!thumbnail_params.parts_only || vol->composite_id.volume_id >= 0) &&
                (vol->partly_inside || is_volume_in_plate_boundingbox(*vol, plate_idx, plate_build_volume))) {
                visible_volumes.emplace_back(vol);
                if (vol->get_origin_mesh_or_vertice_render()) {
                    visible_render_mesh_volumes.emplace_back(vol);
                } else {
                    visible_render_vertice_volumes.emplace_back(vol);
                }
            }
        }

        BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail: plate_idx %1% volumes size %2%,  use_top_view=%4%, for_picking=%5%") % plate_idx %
                                       visible_volumes.size() %  (int) camera_view_angle_type % for_picking;
    } else {
        visible_volumes = volumes.volumes;
        for (GLVolume *vol : volumes.volumes) {
            if (vol->get_origin_mesh_or_vertice_render()) {
                visible_render_mesh_volumes.emplace_back(vol);
            } else {
                visible_render_vertice_volumes.emplace_back(vol);
            }
        }
    }
    // BoundingBoxf3 volumes_box = plate_build_volume;
    BoundingBoxf3 volumes_box;
    volumes_box.min.z() = 0;
    volumes_box.max.z() = 0;
    if (!visible_volumes.empty()) {
        for (const GLVolume *vol : visible_volumes) {
            volumes_box.merge(vol->transformed_bounding_box());
        }
    }
    volumes_box.min.z() = -Slic3r::BuildVolume::SceneEpsilon;
    double width        = volumes_box.max.x() - volumes_box.min.x();
    double depth        = volumes_box.max.y() - volumes_box.min.y();
    double height       = volumes_box.max.z() - volumes_box.min.z();
    volumes_box.max.x() = volumes_box.max.x() + width * 0.1f;
    volumes_box.min.x() = volumes_box.min.x() - width * 0.1f;
    volumes_box.max.y() = volumes_box.max.y() + depth * 0.1f;
    volumes_box.min.y() = volumes_box.min.y() - depth * 0.1f;
    volumes_box.max.z() = volumes_box.max.z() + height * 0.2f;
    volumes_box.min.z() = volumes_box.min.z() - height * 0.2f;

    Camera camera;
    camera.set_type(camera_type);

    if (thumbnail_params.use_plate_box) { camera.set_scene_box(plate_build_volume); }

    camera.apply_viewport(0, 0, thumbnail_data.width, thumbnail_data.height);

    if (camera_view_angle_type == Camera::ViewAngleType::Top_Plate) {
        float  center_x   = (plate_build_volume.max(0) + plate_build_volume.min(0)) / 2;
        float  center_y   = (plate_build_volume.max(1) + plate_build_volume.min(1)) / 2;
        float  distance_z = plate_build_volume.max(2) - plate_build_volume.min(2);
        Vec3d  center(center_x, center_y, 0.f);
        double zoom_ratio, scale_x, scale_y;

        scale_x    = ((double) thumbnail_data.width) / (plate_build_volume.max(0) - plate_build_volume.min(0));
        scale_y    = ((double) thumbnail_data.height) / (plate_build_volume.max(1) - plate_build_volume.min(1));
        zoom_ratio = (scale_x <= scale_y) ? scale_x : scale_y;
        camera.look_at(center + distance_z * Vec3d::UnitZ(), center, Vec3d::UnitY());
        camera.set_zoom(zoom_ratio);
        // camera.select_view("top");
    } else {
        camera.select_view(camera_view_angle_type);
        camera.zoom_to_box(volumes_box);
    }

    if (thumbnail_params.use_plate_box) {
        camera.apply_projection(plate_build_volume);
    } else {
        camera.apply_projection(volumes_box);
    }

    if (for_picking)
        glsafe(::glClearColor(0.f, 0.f, 0.f, 0.f));
    else {
        const auto &bg_color = thumbnail_params.background_color;
        glsafe(::glClearColor(bg_color.x(), bg_color.y(), bg_color.z(), bg_color.w()));
    }

    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    if (ban_light) { glsafe(::glDisable(GL_BLEND)); }

    const auto &view_matrix       = camera.get_view_matrix();
    const auto &projection_matrix = camera.get_projection_matrix();
    const auto &ogl_manager     = wxGetApp().get_opengl_manager();

    std::vector<GLModel> glmodels;
    if (visible_render_mesh_volumes.size() > 0) {
        auto new_shader = ogl_manager->get_shader("thumbnail");
        ogl_manager->bind_shader(new_shader);
        new_shader->set_uniform("ban_light", ban_light);
        new_shader->set_uniform("emission_factor", 0.1f);
        for (GLVolume *vol : visible_render_mesh_volumes) {
            new_shader->set_uniform("volume_world_matrix", vol->world_matrix());
            const Transform3d matrix = view_matrix * vol->world_matrix();
            new_shader->set_uniform("view_model_matrix", matrix);
            new_shader->set_uniform("projection_matrix", projection_matrix);
            new_shader->set_uniform("normal_matrix", (Matrix3d) matrix.matrix().block(0, 0, 3, 3).inverse().transpose());

            if (vol->object_idx() >= 0 && vol->volume_idx() >= 0) {
                int obj_idx   = vol->object_idx();
                int vol_idx   = vol->volume_idx();
                if (obj_idx < (int)model_objects.size()) {
                    const ModelObject* mo = model_objects[obj_idx];
                    if (vol_idx < (int)mo->volumes.size()) {
                        ModelVolume* mv = mo->volumes[vol_idx];
                        if(mv->get_origin_mesh_or_vertice_render() && mv->origin_render_info_ptr) {
                            for (int i = 0; i < mv->origin_render_info_ptr->mesh_with_colors.size(); i++) {
                                auto&    temp_pair = mv->origin_render_info_ptr->mesh_with_colors[i];
                                GLModel temp_render_model;
                                temp_render_model.init_from(temp_pair.first.its);
                                std::array<float, 4> new_color = adjust_color_for_rendering(temp_pair.second);
                                temp_render_model.set_color(ColorRGBA(new_color)); // shader->set_uniform("uniform_color", new_color);
                                temp_render_model.render_geometry();
                                glmodels.emplace_back(temp_render_model);
                            }
                        }
                    }
                }
            }
        }
    }
    if (visible_render_vertice_volumes.size() > 0) {
        auto new_shader = ogl_manager->get_shader("thumbnailWithColor");
        ogl_manager->bind_shader(new_shader);
        new_shader->set_uniform("ban_light", ban_light);
        new_shader->set_uniform("emission_factor", 0.1f);
        for (GLVolume *vol : visible_render_vertice_volumes) {
            new_shader->set_uniform("volume_world_matrix", vol->world_matrix());
            const Transform3d matrix = view_matrix * vol->world_matrix();
            new_shader->set_uniform("view_model_matrix", matrix);
            new_shader->set_uniform("projection_matrix", projection_matrix);
            new_shader->set_uniform("normal_matrix", (Matrix3d) matrix.matrix().block(0, 0, 3, 3).inverse().transpose());

            if (vol->object_idx() >= 0 && vol->volume_idx() >= 0) {
                int obj_idx = vol->object_idx();
                int vol_idx = vol->volume_idx();
                if (obj_idx < (int) model_objects.size()) {
                    const ModelObject *mo = model_objects[obj_idx];
                    if (vol_idx < (int) mo->volumes.size()) {
                        ModelVolume *mv = mo->volumes[vol_idx];
                        if (!mv->get_origin_mesh_or_vertice_render() && mv->origin_render_info_ptr) {
                            auto &  temp_pair = mv->origin_render_info_ptr->vertices_with_colors;
                            if (temp_pair.first.its.vertices.size() == temp_pair.second.size()) {
                                GLModel temp_render_model;
                                temp_render_model.init_from_its_and_color(temp_pair.first.its, temp_pair.second);
                                temp_render_model.render_geometry_with_vertice_color();
                                glmodels.emplace_back(temp_render_model);
                            }
                        }
                    }
                }
            }
        }
    }

    glsafe(::glDisable(GL_DEPTH_TEST));

    BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail: finished");
}

void GLCanvas3D::_set_warning_notification(EWarning warning, bool state)
{
    using NotificationLevel = NotificationManager::NotificationLevel;
    enum ErrorType{
        PLATER_WARNING,
        PLATER_ERROR,
        SLICING_SERIOUS_WARNING,
        SLICING_ERROR,
        SLICING_LIMIT_ERROR,
        SLICING_HEIGHT_OUTSIDE,
        ASSEMBLY_WARNNING
    };
    std::string gc2_pt = wxGetApp().preset_bundle->printers.get_edited_preset().get_printer_type(wxGetApp().preset_bundle);
    std::string gc2_dep = DevPrinterConfigUtil::get_toolhead_display_name(gc2_pt, DEPUTY_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::LowerCase);
    std::string gc2_main = DevPrinterConfigUtil::get_toolhead_display_name(gc2_pt, MAIN_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::LowerCase);
    const std::vector<std::string> extruder_name_list= {_u8L(gc2_dep.c_str()), _u8L(gc2_main.c_str())};  // in ui, we treat extruder as nozzle
    std::string text;
    ErrorType error = ErrorType::PLATER_WARNING;
    const ModelObject* conflictObj=nullptr;
    const auto& t_gocde_viewer = get_gcode_viewer();
    switch (warning) {
    case EWarning::GCodeConflict: {
        static std::string prevConflictText;
        text  = prevConflictText;
        error = ErrorType::SLICING_SERIOUS_WARNING;
        const auto& t_conflict_result = t_gocde_viewer.get_conflict_result();
        if (!t_conflict_result.has_value()) { break; }
        std::string objName1 = t_conflict_result.value()._objName1;
        std::string objName2 = t_conflict_result.value()._objName2;
        double      height   = t_conflict_result.value()._height;
        int         layer    = t_conflict_result.value().layer;
        text = (boost::format(_u8L("Conflicts of gcode paths have been found at layer %d. Please separate the conflicted objects farther (%s <-> %s).")) % (layer + 1) %
                objName1 % objName2)
                   .str();
        //Orio: tilted layers, the print head or the gantry rod would hit an object (see Print.cpp).
        if (! t_conflict_result->message.empty())
            text = t_conflict_result->message;
        prevConflictText        = text;
        const PrintObject *obj2 = reinterpret_cast<const PrintObject *>(t_conflict_result.value()._obj2);
        conflictObj             = obj2->model_object();
        break;
    }
    case EWarning::ObjectOutside:      text = _u8L("An object is layed over the boundary of plate."); break;
    case EWarning::ToolHeightOutside:  text = _u8L("A G-code path goes beyond the max print height."); error = ErrorType::SLICING_ERROR; break;
    case EWarning::ToolpathOutside:    text = _u8L("A G-code path goes beyond the boundary of plate."); error = ErrorType::SLICING_ERROR; break;
    case EWarning::TPUPrintableError: {
        text = _u8L("Not support printing 2 or more TPU filaments.");
        error = ErrorType::SLICING_ERROR;
        break;
    }
    case EWarning::FilamentPrintableError: {
        text  = filament_printable_error_msg.ToUTF8();
        error = ErrorType::SLICING_ERROR;
        break;
    }
    case EWarning::LeftExtruderPrintableError:
    case EWarning::RightExtruderPrintableError: {
        error = ErrorType::PLATER_ERROR;
        break;
    }
    case EWarning::MultiExtruderPrintableError: {
        const auto& t_gcode_check_result = t_gocde_viewer.get_gcode_check_result();
        for (auto error_iter = t_gcode_check_result.print_area_error_infos.begin(); error_iter != t_gcode_check_result.print_area_error_infos.end(); ++error_iter) {
            if (error_iter != t_gcode_check_result.print_area_error_infos.begin()) {
                text += "\n";
            }
            int extruder_id = error_iter->first + 1; // change extruder id to 1 based
            std::string filaments;
            std::vector<int> slice_error_object_idxs;
            for (size_t i = 0; i < error_iter->second.size(); ++i) {
                if (i > 0) {
                    filaments += ", ";
                }
                int filament_id = error_iter->second[i].first + 1; // change filament id to 1 based
                int object_label_id = error_iter->second[i].second;

                filaments += std::to_string(filament_id);
                for (int object_idx = 0; object_idx < (int) m_model->objects.size(); ++object_idx) {
                    const ModelObject *model_object = m_model->objects[object_idx];
                    for (int instance_idx = 0; instance_idx < (int) model_object->instances.size(); ++instance_idx) {
                        const ModelInstance *model_instance = model_object->instances[instance_idx];
                        auto                 expect_id      = model_instance->get_labeled_id();
                        if (object_label_id == expect_id) {
                            slice_error_object_idxs.emplace_back(object_idx);
                        }
                    }
                }
            }
            for (GLVolume *volume : t_gocde_viewer.get_shells().volumes.volumes) {
                for (auto obj_idx : slice_error_object_idxs) {
                    if (volume->object_idx() == obj_idx) {
                        volume->slice_error = true;
                        volume->selected    = true;
                    }
                }
            }
            std::string extruder_name = extruder_name_list[extruder_id-1];
            if (error_iter->second.size() == 1) {
                text += (boost::format(_u8L("Filament %s is placed in the %s, but the generated G-code path exceeds the printable range of the %s.")) %filaments %extruder_name %extruder_name).str();
            }
            else {
                text += (boost::format(_u8L("Filaments %s is placed in the %s, but the generated G-code path exceeds the printable range of the %s.")) %filaments %extruder_name %extruder_name).str();
            }
        }
        error = ErrorType::SLICING_LIMIT_ERROR;
        break;
    }
    case EWarning::MultiExtruderHeightOutside: {
        const auto& t_gcode_check_result = t_gocde_viewer.get_gcode_check_result();
        for (auto error_iter = t_gcode_check_result.print_height_error_infos.begin(); error_iter != t_gcode_check_result.print_height_error_infos.end(); ++error_iter) {
            if (error_iter != t_gcode_check_result.print_height_error_infos.begin()) {
                text += "\n";
            }
            int              extruder_id = error_iter->first + 1; // change extruder id to 1 based
            std::set<int>    filament_ids;
            std::vector<int> slice_error_object_idxs;
            for (size_t i = 0; i < error_iter->second.size(); ++i) {
                int filament_id     = error_iter->second[i].first + 1; // change filament id to 1 based
                int object_label_id = error_iter->second[i].second;
                filament_ids.insert(filament_id);
                for (int object_idx = 0; object_idx < (int) m_model->objects.size(); ++object_idx) {
                    const ModelObject *model_object = m_model->objects[object_idx];
                    for (int instance_idx = 0; instance_idx < (int) model_object->instances.size(); ++instance_idx) {
                        const ModelInstance *model_instance = model_object->instances[instance_idx];
                        auto                 expect_id      = model_instance->get_labeled_id();
                        if (object_label_id == expect_id) {
                            slice_error_object_idxs.emplace_back(object_idx);
                        }
                    }
                }
            }

            std::string filaments;
            int         index = 0;
            for (auto filament_id : filament_ids) {
                if (index > 0) {
                    filaments += ", ";
                }
                filaments += std::to_string(filament_id);
                ++index;
            }

            for (GLVolume* volume : t_gocde_viewer.get_shells().volumes.volumes) {
                for (auto obj_idx : slice_error_object_idxs) {
                    if (volume->object_idx() == obj_idx) {
                        volume->slice_error = true;
                        volume->selected    = true;
                    }
                }
            }
            std::string extruder_name = extruder_name_list[extruder_id-1];
            if (error_iter->second.size() == 1) {
                text += (boost::format(_u8L("Filament %s is placed in the %s, but the generated G-code path exceeds the printable height of the %s.")) % filaments % extruder_name % extruder_name).str();
            } else {
                text += (boost::format(_u8L("Filaments %s is placed in the %s, but the generated G-code path exceeds the printable height of the %s.")) % filaments % extruder_name % extruder_name).str();
            }
        }
        if (!text.empty()) {
            text += "\n";
            text += _u8L("Open wiki for more information.");
        }
        error = ErrorType::SLICING_HEIGHT_OUTSIDE;
        break;
    }

    // BBS: remove _u8L() for SLA
    case EWarning::SlaSupportsOutside: text = ("SLA supports outside the print area were detected."); error = ErrorType::PLATER_ERROR; break;
    case EWarning::SomethingNotShown:  text = _u8L("Only the object being edited is visible."); break;
    case EWarning::ObjectClashed:
        error = ErrorType::PLATER_ERROR;
        break;
    case EWarning::ObjectLimited:
        text = get_object_limited_text();
        break;
    case EWarning::FilamentUnPrintableOnFirstLayer: {
        std::string             warning;
        const auto& t_tilament_result_conflict = t_gocde_viewer.get_filament_printable_result();
        const std::vector<int>& conflict_filament = t_tilament_result_conflict.conflict_filament;
        auto                    iter              = conflict_filament.begin();
        for (int filament : conflict_filament) {
            warning += std::to_string(filament + 1);
            warning += " ";
        }
        text  = (boost::format(_u8L("filaments %s cannot be printed directly on the surface of this plate.")) % warning).str();
        error = ErrorType::SLICING_ERROR;
        break;
    }
    case EWarning::MixUsePLAAndPETG:
        text = _u8L("PLA and PETG filaments detected in the mixture. Adjust parameters according to the Wiki to ensure print quality.");
        break;
    case EWarning::MultiFilaNoWipeTower:
        text = _u8L("The prime tower improves multi-color print quality and is recommended.");
        break;
    case EWarning::PrimeTowerOutside:
        text  = _u8L("The prime tower extends beyond the plate boundary.");
        break;
    case EWarning::AsemblyInvalid:
    {
        error = ErrorType::ASSEMBLY_WARNNING;
        break;
    }
    case EWarning::NozzleFilamentIncompatible: {
        text = _u8L(get_nozzle_filament_incompatible_text());
        break;
    }
    case EWarning::MixtureFilamentIncompatible: {
        text = _u8L(get_filament_mixture_warning_text());
        break;
    }
    case EWarning::FilamentNozzleFlowIncompatible: {
        text = _u8L(get_filament_flow_warning_text());
        error = ErrorType::SLICING_ERROR;
        break;
    }
    case EWarning::TpuNozzleMultipleFilaments: {
        text = _u8L(get_tpu_nozzle_multi_filaments_warning_text());
        break;
    }
    case EWarning::HighTempNeedWrappingDetection: {
        text = _u8L(get_high_temp_wrapping_warning_text());
        break;
    }
    case EWarning::SingleExtruderMixedFilament: {
        text = _u8L(get_single_extruder_mixed_filament_warning_text());
        break;
    }
    case EWarning::FlushingVolumeZero:
        text = _u8L("Partial flushing volume set to 0. Multi-color printing may cause color mixing in models. Please redjust flushing settings.");
        error = ErrorType::SLICING_ERROR;
        break;
    }
    //BBS: this may happened when exit the app, plater is null
    if (!wxGetApp().plater())
        return;
    auto& notification_manager = *wxGetApp().plater()->get_notification_manager();

    switch (error)
    {
    case PLATER_WARNING:
        if (warning == EWarning::MixUsePLAAndPETG) {
            if (state) {
                notification_manager.push_slicing_customize_error_notification(NotificationType::BBLMixUsePLAAndPETG, NotificationLevel::WarningNotificationLevel, text, _u8L("Click Wiki for help."),
                    [](wxEvtHandler*) {
                        std::string language = wxGetApp().app_config->get("language");
                        wxString    region = L"en";
                        if (language.find("zh") == 0)
                        	region = L"zh";
                        wxGetApp().open_browser_with_warning_dialog(wxString::Format(L"https://wiki.bambulab.com/%s/filament-acc/filament/h2d-pla-and-petg-mutual-support", region));
                        return false;
                    });
            }
            else
                notification_manager.close_slicing_customize_error_notification(NotificationType::BBLMixUsePLAAndPETG, NotificationLevel::WarningNotificationLevel);
        } else if (warning == EWarning::MultiFilaNoWipeTower) {
            if (state) {
                notification_manager.push_notification(NotificationType::BBLMultiFilaNoWipeTower, NotificationLevel::HintNotificationLevel, text, _u8L("Jump to: Prime tower"),
                                                       [](wxEvtHandler *) {
                                                           wxGetApp().sidebar().jump_to_option("enable_prime_tower", Preset::TYPE_PRINT, L"");
                                                           return false;
                                                       });
            } else {
                notification_manager.close_notification_of_type(NotificationType::BBLMultiFilaNoWipeTower);
            }
        }
        else if (warning == EWarning::NozzleFilamentIncompatible){
            if(state){
                notification_manager.push_slicing_customize_error_notification(NotificationType::BBLNozzleFilamentIncompatible, NotificationLevel::WarningNotificationLevel, text);
            }
            else{
                notification_manager.close_slicing_customize_error_notification(NotificationType::BBLNozzleFilamentIncompatible, NotificationLevel::WarningNotificationLevel);
            }
        }
        else if (warning == EWarning::TpuNozzleMultipleFilaments) {
            if (state) {
                notification_manager.push_slicing_customize_error_notification(NotificationType::BBLTpuNozzleHasMultiFilament, NotificationLevel::WarningNotificationLevel, text);
            } else {
                notification_manager.close_slicing_customize_error_notification(NotificationType::BBLTpuNozzleHasMultiFilament, NotificationLevel::WarningNotificationLevel);
            }
        }
        else if (warning == EWarning::HighTempNeedWrappingDetection) {
            if (state) {
                notification_manager.push_slicing_customize_error_notification(NotificationType::BBLHighTempNeedWrappingDetection, NotificationLevel::WarningNotificationLevel, text,
                    _u8L("Enable Clumping Detection"),
                    [](wxEvtHandler *) {
                        if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_PRINT)) {
                            DynamicPrintConfig cfg;
                            cfg.set_key_value("enable_wrapping_detection", new ConfigOptionBool(true));
                            tab->load_config(cfg);
                            tab->on_value_change("enable_wrapping_detection", boost::any(true));
                        }
                        wxGetApp().sidebar().jump_to_option("enable_wrapping_detection", Preset::TYPE_PRINT, L"");
                        return false;
                    });
            } else {
                notification_manager.close_slicing_customize_error_notification(NotificationType::BBLHighTempNeedWrappingDetection, NotificationLevel::WarningNotificationLevel);
            }
        }
        else if (warning == EWarning::SingleExtruderMixedFilament) {
            if (state) {
                notification_manager.push_slicing_customize_error_notification(
                    NotificationType::BBLSingleExtruderMixedFilamentRisk,
                    NotificationLevel::WarningNotificationLevel,
                    text);
            } else {
                notification_manager.close_slicing_customize_error_notification(
                    NotificationType::BBLSingleExtruderMixedFilamentRisk,
                    NotificationLevel::WarningNotificationLevel);
            }
        }
        else {
            if (state)
                notification_manager.push_plater_warning_notification(text);
            else
                notification_manager.close_plater_warning_notification(text);
        }
        break;
    case PLATER_ERROR:
        if (warning == EWarning::LeftExtruderPrintableError) {
            if (state) {
                if (auto left_str = get_left_extruder_unprintable_text(); !left_str.empty())
                    notification_manager.bbl_show_filament_map_invalid_notification_before_slice(NotificationType::LeftExtruderUnprintableError, left_str);
            }
            else {
                notification_manager.bbl_close_filament_map_invalid_notification_before_slice(NotificationType::LeftExtruderUnprintableError);
            }
        }
        else if (warning == EWarning::RightExtruderPrintableError) {
            if (state) {
                if (auto right_str = get_right_extruder_unprintable_text(); !right_str.empty())
                    notification_manager.bbl_show_filament_map_invalid_notification_before_slice(NotificationType::RightExtruderUnprintableError, right_str);
            }
            else {
                notification_manager.bbl_close_filament_map_invalid_notification_before_slice(NotificationType::RightExtruderUnprintableError);
            }
        }
        else if (warning == EWarning::ObjectClashed) {
            auto str = get_object_clashed_text();
            if(state){
                if (!str.empty())
                    notification_manager.push_plater_error_notification(str);
            }
            else{
                notification_manager.close_plater_error_notification(str);
            }
        }
        else {
            if (state)
                notification_manager.push_plater_error_notification(text);
            else
                notification_manager.close_plater_error_notification(text);
        }
        break;
    case SLICING_SERIOUS_WARNING:
        if (state)
            notification_manager.push_slicing_serious_warning_notification(text, conflictObj ? std::vector<ModelObject const*>{conflictObj} : std::vector<ModelObject const*>{});
        else
            notification_manager.close_slicing_serious_warning_notification(text);
        break;
    case SLICING_ERROR:
        if (warning == EWarning::FilamentUnPrintableOnFirstLayer) {
            if (state) {
                notification_manager.bbl_show_bed_filament_incompatible_notification(text);
            }
            else {
                notification_manager.bbl_close_bed_filament_incompatible_notification();
            }
        }
        if (warning == EWarning::FilamentPrintableError) {
            if (state){
                auto callback = [](wxEvtHandler*) {
                    auto plater = wxGetApp().plater();
                    auto partplate = plater->get_partplate_list().get_curr_plate();
                    try_pop_up_before_slice(false, plater, partplate, true); // ignore the return value
                    return false;
                };
                notification_manager.push_slicing_customize_error_notification(NotificationType::BBLFilamentPrintableError, NotificationLevel::ErrorNotificationLevel, text,  _u8L("Click here to regroup"), callback);
            }
            else
                notification_manager.close_slicing_customize_error_notification(NotificationType::BBLFilamentPrintableError, NotificationLevel::ErrorNotificationLevel);
        }
        else if (warning == EWarning::FlushingVolumeZero) {
            if (state) {
                auto callback = [](wxEvtHandler *) {
                    auto                              plater              = wxGetApp().plater();
                    open_flushing_dialog(plater, SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, plater));
                    plater->get_view3D_canvas3D()->reload_scene(true);
                    return false;
                };
                notification_manager.push_flushing_volume_error_notification(NotificationType::BBLFlushingVolumeZero, NotificationLevel::WarningNotificationLevel, text, _u8L("Flushing Volume"), callback);
            } else
                notification_manager.close_flushing_volume_error_notification(NotificationType::BBLFlushingVolumeZero, NotificationLevel::WarningNotificationLevel);
        }
        else if (warning == EWarning::FilamentNozzleFlowIncompatible) {
            if (state) {
                notification_manager.push_slicing_customize_error_notification(NotificationType::BBLNozzleFilamentIncompatible, NotificationLevel::ErrorNotificationLevel, text);
            } else {
                notification_manager.close_slicing_customize_error_notification(NotificationType::BBLNozzleFilamentIncompatible, NotificationLevel::ErrorNotificationLevel);
            }
        }
        else {
            if (state)
                notification_manager.push_slicing_error_notification(text, conflictObj ? std::vector<ModelObject const*>{conflictObj} : std::vector<ModelObject const*>{});
            else
                notification_manager.close_slicing_error_notification(text);
        }
        break;
    case SLICING_LIMIT_ERROR:
        if (state)
            notification_manager.bbl_show_filament_map_invalid_notification_after_slice(NotificationType::BBLSliceLimitError, text);
        else
            notification_manager.bbl_close_filament_map_invalid_notification_after_slice(NotificationType::BBLSliceLimitError);
        break;
    case SLICING_HEIGHT_OUTSIDE:
        if (state)
            notification_manager.push_slicing_customize_error_notification(NotificationType::BBLSliceMultiExtruderHeightOutside, NotificationLevel::ErrorNotificationLevel, text);
        else
            notification_manager.close_slicing_customize_error_notification(NotificationType::BBLSliceMultiExtruderHeightOutside, NotificationLevel::ErrorNotificationLevel);
        break;
    case ASSEMBLY_WARNNING:
    {
        text = get_assembly_too_far_text();
        if (state)
            notification_manager.push_assembly_warning_notification(text);
        else
            notification_manager.close_assembly_warning_notification(text);
        break;
    }
    default:
        break;
    }
}

bool GLCanvas3D::is_flushing_matrix_error() {

    const auto                &project_config = wxGetApp().preset_bundle->project_config;
    const std::vector<double> &config_matrix  = (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values;
    const std::vector<double> &config_multiplier = (project_config.option<ConfigOptionFloats>("flush_multiplier"))->values;

    for (auto multiplier : config_multiplier) {
        if (multiplier == 0 && config_matrix.size() >= config_multiplier.size() * 4) return true;
    }

    int  matrix_len = config_matrix.size() / config_multiplier.size();
    int  row_len    = std::sqrt(matrix_len);
    for (int i = 0; i < config_matrix.size(); i++)
    {
        int relative_id = i % matrix_len;
        int row_id      = relative_id / row_len;
        int col_id      = relative_id % row_len;
        if (row_id != col_id && config_matrix[i] == 0) return true;
    }
    return false;
}

bool GLCanvas3D::_is_any_volume_outside() const
{
    for (const GLVolume* volume : m_volumes.volumes) {
        if (volume != nullptr && volume->is_outside)
            return true;
    }

    return false;
}

void GLCanvas3D::_update_selection_from_hover()
{
    bool ctrl_pressed = wxGetKeyState(WXK_CONTROL);

    if (m_hover_volume_idxs.empty()) {
        // Orio : un rectangle (Maj + glisser) tracé dans le vide ne désélectionne plus quand l'outil ouvert reste
        // ouvert sur un clic dans le vide (Déplacer, Pivoter, Échelle…, is_gizmo_click_empty_not_exit) : il le fermait
        if (!ctrl_pressed && (m_rectangle_selection.get_state() == GLSelectionRectangle::Select) && !m_gizmos.is_gizmo_click_empty_not_exit())
            m_selection.remove_all();

        return;
    }

    GLSelectionRectangle::EState state = m_rectangle_selection.get_state();

    bool hover_modifiers_only = true;
    for (int i : m_hover_volume_idxs) {
        if (!m_volumes.volumes[i]->is_modifier) {
            hover_modifiers_only = false;
            break;
        }
    }

    bool selection_changed = false;
    if (state == GLSelectionRectangle::Select) {
        bool contains_all = true;
        for (int i : m_hover_volume_idxs) {
            if (!m_selection.contains_volume((unsigned int)i)) {
                contains_all = false;
                break;
            }
        }

        // the selection is going to be modified (Add)
        if (!contains_all) {
            wxGetApp().plater()->take_snapshot(std::string("Select by rectangle"), UndoRedo::SnapshotType::Selection);
            selection_changed = true;
        }
    }
    else {
        bool contains_any = false;
        for (int i : m_hover_volume_idxs) {
            if (m_selection.contains_volume((unsigned int)i)) {
                contains_any = true;
                break;
            }
        }

        // the selection is going to be modified (Remove)
        if (contains_any) {
            wxGetApp().plater()->take_snapshot(std::string("Unselect by rectangle"), UndoRedo::SnapshotType::Selection);
            selection_changed = true;
        }
    }

    if (!selection_changed)
        return;

    Plater::SuppressSnapshots suppress(wxGetApp().plater());

    if ((state == GLSelectionRectangle::Select) && !ctrl_pressed)
        m_selection.clear();

    for (int i : m_hover_volume_idxs) {
        if (state == GLSelectionRectangle::Select) {
            if (hover_modifiers_only) {
                const GLVolume& v = *m_volumes.volumes[i];
                m_selection.add_volume(v.object_idx(), v.volume_idx(), v.instance_idx(), false);
            }
            else
                m_selection.add(i, false);
        }
        else
            m_selection.remove(i);
    }

    if (m_selection.is_empty())
        m_gizmos.reset_all_states();
    else
        m_gizmos.refresh_on_off_state();

    m_gizmos.update_data();
    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
    m_dirty = true;
}

bool GLCanvas3D::_deactivate_arrange_menu()
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return false;
    }
    if (p_main_toolbar->is_item_pressed("arrange")) {
        p_main_toolbar->force_right_action(p_main_toolbar->get_item_id("arrange"), *this);
        return true;
    }

    return false;
}

//BBS: add deactivate orient menu
bool GLCanvas3D::_deactivate_orient_menu()
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return false;
    }
    if (p_main_toolbar->is_item_pressed("orient")) {
        p_main_toolbar->force_right_action(p_main_toolbar->get_item_id("orient"), *this);
        return true;
    }

    return false;
}

//BBS: add deactivate layersediting menu
bool GLCanvas3D::_deactivate_layersediting_menu()
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return false;
    }
    if (p_main_toolbar->is_item_pressed("layersediting")) {
        p_main_toolbar->force_right_action(p_main_toolbar->get_item_id("layersediting"), *this);
        return true;
    }

    return false;
}

//BBS: add deactivate speedediting menu
bool GLCanvas3D::_deactivate_speedediting_menu()
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return false;
    }
    if (p_main_toolbar->is_item_pressed("speedediting")) {
        p_main_toolbar->force_right_action(p_main_toolbar->get_item_id("speedediting"), *this);
        return true;
    }

    return false;
}

//Orio: variable sparse infill density / wall count along Z.
// Orio: closes the tool, as Esc does, but at once: without the steps Esc goes through first in some tools (cancel
// a choice of anchors, a selection...).
void GLCanvas3D::close_tool_window(const std::string &id)
{
    if (id == "toolbar_arrange")
        _deactivate_arrange_menu();
    else if (id == "toolbar_variable_layer_height")
        _deactivate_layersediting_menu();
    else if (id == "toolbar_variable_speed")
        _deactivate_speedediting_menu();
    else if (id == "toolbar_variable_infill_walls")
        _deactivate_infillwallsediting_menu();
    else // the window of a gizmo
        m_gizmos.reset_all_states();
    set_as_dirty();
}

bool GLCanvas3D::_deactivate_infillwallsediting_menu()
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return false;
    }
    if (p_main_toolbar->is_item_pressed("infillwallsediting")) {
        p_main_toolbar->force_right_action(p_main_toolbar->get_item_id("infillwallsediting"), *this);
        return true;
    }

    return false;
}

bool GLCanvas3D::_deactivate_collapse_toolbar_items()
{
    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    if (collapse_toolbar.is_item_pressed("print")) {
        collapse_toolbar.force_left_action(collapse_toolbar.get_item_id("print"), *this);
        return true;
    }

    return false;
}

void GLCanvas3D::highlight_toolbar_item(const std::string& item_name)
{
    const auto& p_main_toolbar = get_main_toolbar();
    if (!p_main_toolbar) {
        return;
    }
    std::shared_ptr<GLToolbarItem> item = p_main_toolbar->get_item(item_name);
    if (!item || !item->is_visible())
        return;
    m_toolbar_highlighter.init(item, this);
}

void GLCanvas3D::highlight_gizmo(const std::string& gizmo_name)
{
    GLGizmosManager::EType gizmo = m_gizmos.get_gizmo_from_name(gizmo_name);
    if(gizmo == GLGizmosManager::EType::Undefined)
        return;
    m_gizmo_highlighter.init(&m_gizmos, gizmo, this);
}

const Print* GLCanvas3D::fff_print() const
{
    return (m_process == nullptr) ? nullptr : m_process->fff_print();
}

const SLAPrint* GLCanvas3D::sla_print() const
{
    return (m_process == nullptr) ? nullptr : m_process->sla_print();
}

void GLCanvas3D::WipeTowerInfo::apply_wipe_tower() const
{
    if (m_plate_idx >= wxGetApp().plater()->get_partplate_list().get_plate_count()) {
        BOOST_LOG_TRIVIAL(error) << "Invalid plate index: " << m_plate_idx << ">=" << wxGetApp().plater()->get_partplate_list().get_plate_count();
        return;
    }
    // BBS: add partplate logic
    DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
    Vec3d plate_origin = wxGetApp().plater()->get_partplate_list().get_plate(m_plate_idx)->get_origin();
    ConfigOptionFloat wipe_tower_x(m_pos(X) - plate_origin(0));
    ConfigOptionFloat wipe_tower_y(m_pos(Y) - plate_origin(1));

    ConfigOptionFloats* wipe_tower_x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x", true);
    ConfigOptionFloats* wipe_tower_y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y", true);
    wipe_tower_x_opt->set_at(&wipe_tower_x, m_plate_idx, 0);
    wipe_tower_y_opt->set_at(&wipe_tower_y, m_plate_idx, 0);

    //q->update();
}

void GLCanvas3D::RenderTimer::Notify()
{
    wxPostEvent((wxEvtHandler*)GetOwner(), RenderTimerEvent( EVT_GLCANVAS_RENDER_TIMER, *this));
}

void GLCanvas3D::ToolbarHighlighterTimer::Notify()
{
    wxPostEvent((wxEvtHandler*)GetOwner(), ToolbarHighlighterTimerEvent(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, *this));
}

void GLCanvas3D::GizmoHighlighterTimer::Notify()
{
    wxPostEvent((wxEvtHandler*)GetOwner(), GizmoHighlighterTimerEvent(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, *this));
}

GLCanvas3D::RenderPipelineStageModifier::RenderPipelineStageModifier(GLCanvas3D& canvas, ERenderPipelineStage stage)
    : m_canvas(canvas)
{
    m_canvas._push_render_stage(stage);
}

GLCanvas3D::RenderPipelineStageModifier::~RenderPipelineStageModifier()
{
    m_canvas._pop_render_stage();
}

void GLCanvas3D::ToolbarHighlighter::set_timer_owner(wxEvtHandler* owner, int timerid/* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void GLCanvas3D::ToolbarHighlighter::init(const std::shared_ptr<GLToolbarItem>& toolbar_item, GLCanvas3D* canvas)
{
    if (m_timer.IsRunning())
        invalidate();
    if (!toolbar_item || !canvas)
        return;

    m_timer.Start(300, false);

    m_toolbar_item = toolbar_item;
    m_canvas       = canvas;
}

void GLCanvas3D::ToolbarHighlighter::invalidate()
{
    m_timer.Stop();

    if (const auto& p_toolbar_item = m_toolbar_item.lock()) {
        p_toolbar_item->set_highlight(GLToolbarItem::EHighlightState::NotHighlighted);
    }
    m_toolbar_item.reset();
    m_blink_counter = 0;
    m_render_arrow = false;
}

void GLCanvas3D::ToolbarHighlighter::blink()
{
    const auto& p_toolbar_item = m_toolbar_item.lock();
    if (p_toolbar_item) {
        char state = p_toolbar_item->get_highlight();
        if (state != (char)GLToolbarItem::EHighlightState::HighlightedShown)
            p_toolbar_item->set_highlight(GLToolbarItem::EHighlightState::HighlightedShown);
        else
            p_toolbar_item->set_highlight(GLToolbarItem::EHighlightState::HighlightedHidden);

        m_render_arrow = !m_render_arrow;
        m_canvas->set_as_dirty();
    }
    else
        invalidate();

    if ((++m_blink_counter) >= 11)
        invalidate();
}

void GLCanvas3D::GizmoHighlighter::set_timer_owner(wxEvtHandler* owner, int timerid/* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void GLCanvas3D::GizmoHighlighter::init(GLGizmosManager* manager, GLGizmosManager::EType gizmo, GLCanvas3D* canvas)
{
    if (m_timer.IsRunning())
        invalidate();
    if (!gizmo || !canvas)
        return;

    m_timer.Start(300, false);

    m_gizmo_manager = manager;
    m_gizmo_type    = gizmo;
    m_canvas        = canvas;
}

void GLCanvas3D::GizmoHighlighter::invalidate()
{
    m_timer.Stop();

    if (m_gizmo_manager) {
        m_gizmo_manager->set_highlight(GLGizmosManager::EType::Undefined, false);
    }
    m_gizmo_manager = nullptr;
    m_gizmo_type = GLGizmosManager::EType::Undefined;
    m_blink_counter = 0;
    m_render_arrow = false;
}

void GLCanvas3D::GizmoHighlighter::blink()
{
    if (m_gizmo_manager) {
        if (m_blink_counter % 2 == 0)
            m_gizmo_manager->set_highlight(m_gizmo_type, true);
        else
            m_gizmo_manager->set_highlight(m_gizmo_type, false);

        m_render_arrow = !m_render_arrow;
        m_canvas->set_as_dirty();
    }
    else
        invalidate();

    if ((++m_blink_counter) >= 11)
        invalidate();
}

const ModelVolume *get_model_volume(const GLVolume &v, const Model &model)
{
    const ModelVolume * ret = nullptr;

    if (v.object_idx() < (int)model.objects.size()) {
        const ModelObject *obj = model.objects[v.object_idx()];
        if (v.volume_idx() < (int)obj->volumes.size())
            ret = obj->volumes[v.volume_idx()];
    }

    return ret;
}

ModelVolume *get_model_volume(const ObjectID &volume_id, const ModelObjectPtrs &objects)
{
    for (const ModelObject *obj : objects)
        for (ModelVolume *vol : obj->volumes)
            if (vol->id() == volume_id)
                return vol;
    return nullptr;
}

ModelVolume *get_model_volume(const GLVolume &v, const ModelObject& object) {
    if (v.volume_idx() < 0)
        return nullptr;

    size_t volume_idx = static_cast<size_t>(v.volume_idx());
    if (volume_idx >= object.volumes.size())
        return nullptr;

    return object.volumes[volume_idx];
}

ModelVolume *get_selected_model_volume(const GLCanvas3D &canvas)
{
    auto gl_volume = get_selected_gl_volume(canvas);
    if (gl_volume) {
        return get_model_volume(*gl_volume, canvas.get_model()->objects);
    }
    return nullptr;
}

ModelVolume *get_model_volume(const GLVolume &v, const ModelObjectPtrs &objects)
{
    if (v.object_idx() < 0)
        return nullptr;
    size_t objext_idx = static_cast<size_t>(v.object_idx());
    if (objext_idx >= objects.size())
        return nullptr;
    if (objects[objext_idx] == nullptr)
        return nullptr;
    return get_model_volume(v, *objects[objext_idx]);
}

GLVolume *get_first_hovered_gl_volume(const GLCanvas3D &canvas)
{
    int hovered_id_signed =  canvas.get_first_hover_volume_idx();
    if (hovered_id_signed < 0)
        return nullptr;

    size_t              hovered_id = static_cast<size_t>(hovered_id_signed);
    const GLVolumePtrs &volumes    = canvas.get_volumes().volumes;
    if (hovered_id >= volumes.size())
        return nullptr;

    return volumes[hovered_id];
}

GLVolume *get_selected_gl_volume(const GLCanvas3D &canvas)
{
    const GLVolume *gl_volume = get_selected_gl_volume(canvas.get_selection());
    if (gl_volume == nullptr) return nullptr;

    const GLVolumePtrs &gl_volumes = canvas.get_volumes().volumes;
    for (GLVolume *v : gl_volumes)
        if (v->composite_id == gl_volume->composite_id) return v;
    return nullptr;
}

ModelObject *get_selected_model_object(GLCanvas3D &canvas) {
    auto v = get_selected_gl_volume(canvas);
    if (v) {
        auto mo = get_model_object(*v, canvas.get_model()->objects);
        return mo;
    }
    else {
        int out_object_idx;
        return canvas.get_selection().get_selected_single_object(out_object_idx);
    }
    return nullptr;
}

ModelObject *get_model_object(const GLVolume &gl_volume, const Model &model) { return get_model_object(gl_volume, model.objects); }

ModelObject *get_model_object(const GLVolume &gl_volume, const ModelObjectPtrs &objects)
{
    if (gl_volume.object_idx() < 0) return nullptr;
    size_t objext_idx = static_cast<size_t>(gl_volume.object_idx());
    if (objext_idx >= objects.size()) return nullptr;
    return objects[objext_idx];
}

ModelInstance *get_model_instance(const GLVolume &gl_volume, const Model &model) { return get_model_instance(gl_volume, model.objects); }

ModelInstance *get_model_instance(const GLVolume &gl_volume, const ModelObjectPtrs &objects)
{
    if (gl_volume.instance_idx() < 0) return nullptr;
    ModelObject *object = get_model_object(gl_volume, objects);
    return get_model_instance(gl_volume, *object);
}

ModelInstance *get_model_instance(const GLVolume &gl_volume, const ModelObject &object)
{
    if (gl_volume.instance_idx() < 0) return nullptr;
    size_t instance_idx = static_cast<size_t>(gl_volume.instance_idx());
    if (instance_idx >= object.instances.size()) return nullptr;
    return object.instances[instance_idx];
}

} // namespace GUI
} // namespace Slic3r
