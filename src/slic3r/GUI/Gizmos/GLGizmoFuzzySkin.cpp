#include "GLGizmoFuzzySkin.hpp"

#include "libslic3r/Model.hpp"
//BBS
#include "libslic3r/Layer.hpp"
#include "libslic3r/Thread.hpp"

//#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"


#include <GL/glew.h>
#include <imgui/imgui_internal.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <functional>
#include <optional>

namespace Slic3r::GUI {

//Orio: color of each layer of the fuzzy skin painting, by state (the state 1 keeps the color of the painting
// before the layers).
static constexpr std::array<std::array<float, 4>, FUZZY_SKIN_LAYERS_MAX> fuzzy_skin_layer_colors {{
    TriangleSelectorGUI::enforcers_color, // lavender
    { 0.95f, 0.60f, 0.25f, 1.f },         // orange
    { 0.35f, 0.78f, 0.72f, 1.f },         // turquoise
    { 0.90f, 0.45f, 0.65f, 1.f },         // pink
    { 0.90f, 0.86f, 0.38f, 1.f },         // lemon yellow
    { 0.40f, 0.70f, 0.90f, 1.f },         // sky blue
    { 0.58f, 0.80f, 0.55f, 1.f },         // soft green
    { 0.65f, 0.48f, 0.35f, 1.f },         // brown
}};

static const std::array<float, 4> &fuzzy_skin_layer_color(int state)
{
    return fuzzy_skin_layer_colors[size_t(std::clamp(state, 1, FUZZY_SKIN_LAYERS_MAX) - 1)];
}

GLGizmoFuzzySkin::GLGizmoFuzzySkin(GLCanvas3D& parent, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
    m_tool_type = ToolType::BRUSH;
    m_cursor_type = TriangleSelector::CursorType::CIRCLE;
}

void GLGizmoFuzzySkin::data_changed(bool is_serializing) {
    set_painter_gizmo_data(m_parent.get_selection());
}

void GLGizmoFuzzySkin::on_shutdown()
{
    //BBS
    //wait the thread
    if (m_thread.joinable()) {
        Print *print = m_print_instance.print_object->print();
        if (print) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "cancel the print";
            print->cancel();
        }
        //join the thread
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "try to join thread for 2000 ms";
        auto ret = m_thread.try_join_for(boost::chrono::milliseconds(2000));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "join thread returns "<<ret;
    }

    m_print_instance.print_object = NULL;
    m_print_instance.model_instance = NULL;

    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

std::string GLGizmoFuzzySkin::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        return _u8L("Fuzzy skin Painting") + ":\n" + _u8L("Please select single object.");
    } else {
        return _u8L("Fuzzy skin Painting");
    }
}

EnforcerBlockerType GLGizmoFuzzySkin::get_right_button_state_type() const {
    return EnforcerBlockerType::NONE;
}

std::array<float, 4> GLGizmoFuzzySkin::get_cursor_sphere_left_button_color() const
{
    std::array<float, 4> color = fuzzy_skin_layer_color(m_layer_state);
    color[3]                   = 0.25f;
    return color;
}

bool GLGizmoFuzzySkin::on_init()
{
    // BBS
    m_shortcut_key = WXK_CONTROL_H;
    const wxString ctrl                = GUI::shortkey_ctrl_prefix();
    const wxString alt                 = GUI::shortkey_alt_prefix();
    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]      = _L("Section view");
    m_desc["reset_direction"]       = _L("Reset direction");
    m_desc["cursor_size_caption"]      = ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]           = _L("Pen size");
    m_desc["add_fuzzyskin_caption"] = _L("Left mouse button");
    m_desc["add_fuzzyskin"]         = _L("Add fuzzy skin");
    m_desc["remove_caption"]        =  _L("Right mouse button");
    //Orio: the eraser only erases the painting of the selected layer.
    m_desc["remove"]                = _L("Erase the painting of the selected layer");
    m_desc["remove_all"]            = _L("Erase all painting");
    m_desc["perform"]               = _L("Perform");
    m_desc["tool_type"]             = _L("Tool type");
    m_desc["smart_fill_angle_caption"] = ctrl + _L("Mouse wheel");
    m_desc["smart_fill_angle"]      = _L("Smart fill angle");
    m_desc["airbrush_density"]      = _L("Dots per cm²");

    memset(&m_print_instance, 0, sizeof(m_print_instance));
    return true;
}

void GLGizmoFuzzySkin::render_painter_gizmo() const
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

// BBS
bool GLGizmoFuzzySkin::on_key_down_select_tool_type(int keyCode) {
    switch (keyCode)
    {
    case 'F':
        m_current_tool = ImGui::FillButtonIcon;
        break;
    case 'S':
        m_current_tool = ImGui::SphereButtonIcon;
        break;
    case 'C':
        m_current_tool = ImGui::CircleButtonIcon;
        break;
    case 'T':
        m_current_tool = ImGui::TriangleButtonIcon; break;
    default:
        return false;
        break;
    }
    return true;
}

std::string GLGizmoFuzzySkin::get_icon_filename(bool is_dark_mode) const
{
    return is_dark_mode ? "toolbar_fuzzyskin_dark.svg" : "toolbar_fuzzyskin.svg";
}

// BBS
void GLGizmoFuzzySkin::render_triangles(const Selection& selection) const
{
    ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    const auto &             shader   = wxGetApp().get_shader("mm_gouraud");
    if (!shader) return;
    wxGetApp().bind_shader(shader);
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    shader->set_uniform("slope.actived", m_parent.is_using_slope());
    ScopeGuard guard([shader]() {
        if (shader) wxGetApp().unbind_shader();
    });

    // BBS: to improve the random white pixel issue
    glsafe(::glDisable(GL_CULL_FACE));

    const ModelObject *mo      = m_c->selection_info()->model_object();
    int                mesh_id = -1;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part()) continue;

        ++mesh_id;

        Transform3d trafo_matrix;
        if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_assemble_transformation().get_matrix() * mv->get_matrix();
            trafo_matrix.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) +
                                   mo->instances[selection.get_instance_idx()]->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
        } else {
            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
        }

        bool is_left_handed = trafo_matrix.matrix().determinant() < 0.;
        if (is_left_handed) glsafe(::glFrontFace(GL_CW));

        const Camera &    camera = wxGetApp().plater()->get_camera();
        const Transform3d matrix = camera.get_view_matrix() * trafo_matrix;
        shader->set_uniform("view_model_matrix", matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("normal_matrix", (Matrix3d) matrix.matrix().block(0, 0, 3, 3).inverse().transpose());

        shader->set_uniform("volume_world_matrix", trafo_matrix);
        shader->set_uniform("volume_mirrored", is_left_handed);
        m_triangle_selectors[mesh_id]->render(m_imgui, trafo_matrix);

        if (is_left_handed) glsafe(::glFrontFace(GL_CCW));
    }
}

void GLGizmoFuzzySkin::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == On) {

    }
    else if (get_state() == Off) {
        ModelObject* mo = m_c->selection_info()->model_object();
        if (mo) Slic3r::save_object_mesh(*mo);
    }
}

void GLGizmoFuzzySkin::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c) {
        return;
    }
    const auto& p_selection_info = m_c->selection_info();
    if (!p_selection_info) {
        return;
    }
    const auto& p_model_object = p_selection_info->model_object();
    if (!p_model_object) {
        return;
    }
    m_imgui_start_pos[0] = x;
    m_imgui_start_pos[1] = y;
    // BBS
    wchar_t old_tool = m_current_tool;


    const float approx_height = m_imgui->scaled(23.f);
    y = std::min(y, bottom_limit - approx_height);

    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);

    //BBS
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size = m_imgui->get_style_scaling() * 8;
    const float remove_btn_width        = m_imgui->calc_text_size(m_desc.at("remove_all")).x + m_imgui->scaled(1.5f);
    const float buttons_width           = remove_btn_width  + m_imgui->scaled(1.5f);
    const float empty_button_width      = m_imgui->calc_button_size("").x;

    const float minimal_slider_width = m_imgui->scaled(4.f);

    float caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 5>{"add_fuzzyskin", "remove", "cursor_size", "clipping_of_view", "smart_fill_angle"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }
    total_text_max += caption_max + m_imgui->scaled(1.f);
    caption_max += m_imgui->scaled(1.f);

    //Orio: the sliders (and the tool icons) just after the longest label shown for the current tool.
    const float tool_label_width = m_current_tool == ImGui::TriangleButtonIcon ? 0.f :
        m_current_tool == ImGui::AirbrushButtonIcon ? std::max(m_imgui->calc_text_size(m_desc.at("cursor_size")).x, m_imgui->calc_text_size(m_desc.at("airbrush_density")).x) :
        m_imgui->calc_text_size(m_desc.at(m_current_tool == ImGui::FillButtonIcon ? "smart_fill_angle" : "cursor_size")).x;
    const float sliders_left_width = ImGui::GetStyle().WindowPadding.x + space_size +
        std::max({m_imgui->calc_text_size(m_desc.at("tool_type")).x, tool_label_width, clipping_label_width(m_desc.at("clipping_of_view"))});
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    const float sliders_width = m_imgui->scaled(7.0f);
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_left_width + sliders_width - space_size;

    float drag_pos_times     = 0.7;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("tool_type"));
    //Orio: + the airbrush (see m_airbrush).
    std::array<wchar_t, 5> tool_ids = {ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::FillButtonIcon, ImGui::AirbrushButtonIcon};
    std::array<wchar_t, 5> icons;
    if (m_is_dark_mode)
        icons = {ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::FillButtonDarkIcon, ImGui::AirbrushButtonDarkIcon};
    else
        icons = tool_ids;

    std::array<wxString, 5> tool_tips = {_L("Circle"), _L("Sphere"), _L("Triangle"), _L("Fill"), _L("Airbrush")};
    for (int i = 0; i < tool_ids.size(); i++) {
        std::string  str_label = std::string("##");
        std::wstring btn_name = icons[i] + boost::nowide::widen(str_label);

        //Orio: on the row of their label, right after it.
        ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + m_imgui->calc_text_size(m_desc.at("tool_type")).x + space_size +
                        (empty_button_width + m_imgui->scaled(1.75f)) * i);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
        if (m_current_tool == tool_ids[i]) {
            ImGui::PushStyleColor(ImGuiCol_Button, m_is_dark_mode ? ImVec4(26 / 255.0f, 53 / 255.0f, 80 / 255.0f, 1.00f) : ImVec4(0.86f, 0.99f, 0.91f, 1.00f)); // r, g, b, a
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, m_is_dark_mode ? ImVec4(26 / 255.0f, 53 / 255.0f, 80 / 255.0f, 1.00f) : ImVec4(0.86f, 0.99f, 0.91f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, m_is_dark_mode ? ImVec4(26 / 255.0f, 53 / 255.0f, 80 / 255.0f, 1.00f) : ImVec4(0.86f, 0.99f, 0.91f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.00f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0);
        }
        bool btn_clicked = ImGui::Button(into_u8(btn_name).c_str());
        if (m_current_tool == tool_ids[i])
        {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
        ImGui::PopStyleVar(1);

        if (btn_clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto& triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }

        if (ImGui::IsItemHovered()) {
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
        }
    }

    if (m_current_tool != old_tool)
        this->tool_changed(old_tool, m_current_tool);
    //Orio: the airbrush keeps its own radius (bigger circles), the other brushes theirs (whatever chose the tool).
    if (const bool airbrush = m_current_tool == ImGui::AirbrushButtonIcon; airbrush != m_airbrush) {
        std::swap(m_cursor_radius, m_other_radius);
        m_airbrush = airbrush;
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool == ImGui::CircleButtonIcon) {
        ImGui::Separator();
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        m_tool_type = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::SphereButtonIcon) {
        ImGui::Separator();
        m_cursor_type = TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::FillButtonIcon) {
        ImGui::Separator();
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type = ToolType::SMART_FILL;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("smart_fill_angle"));
        std::string format_str = std::string("%.f") + I18N::translate_utf8("", "Face angle threshold, placed after the number with no whitespace in between.");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        if (m_imgui->bbl_slider_float_style("##smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, format_str.data(), 1.0f, true))
            for (auto& triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::TriangleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;
    } else if (m_current_tool == ImGui::AirbrushButtonIcon) {
        //Orio: the circle brush which sprays dots (see m_airbrush): its radius, up to bigger circles, and its density.
        ImGui::Separator();
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        m_tool_type   = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, AirbrushRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
        m_cursor_radius = std::clamp(m_cursor_radius, CursorRadiusMin, AirbrushRadiusMax);

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("airbrush_density"));
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(format_wxstr(_L("Dots sprayed per cm² at each pass; passing again adds some. Their size follows the point "
                                             "distance of the selected layer: %1% to %2% mm."),
                                          wxString::Format("%.1f", m_airbrush_dot), wxString::Format("%.1f", 2.f * m_airbrush_dot)),
                             max_tooltip_width);
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##airbrush_density", &m_airbrush_density, AirbrushDensityMin, AirbrushDensityMax, "%.0f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##airbrush_density_input", &m_airbrush_density, 0.5f, 0.0f, 0.0f, "%.0f");
        m_airbrush_density = std::clamp(m_airbrush_density, AirbrushDensityMin, AirbrushDensityMax);
    }

    //ImGui::AlignTextToFramePadding();

    if (m_current_tool != ImGui::GapFillIcon) {
        ImGui::Separator();
        render_clipping_label(m_desc.at("clipping_of_view"), m_desc.at("reset_direction"));

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        bool b_bbl_slider_float = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);

        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_drag_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (b_bbl_slider_float || b_drag_input) m_c->object_clipper()->set_position(clp_dist, true);
    }

    //Orio: the layers of the painting, up to the end of the slider rows.
    ImGui::Separator();
    render_layers(sliders_left_width, sliders_width, drag_left_width - sliders_left_width - sliders_width, 1.5f * slider_icon_width);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    // Orio: under the window where it is (it can be moved).
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + ImGui::GetWindowPos().y;
    show_tooltip_information(caption_max, ImGui::GetWindowPos().x, get_cur_y);

    float f_scale = m_parent.get_main_toolbar_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);
        ModelObject *        mo  = m_c->selection_info()->model_object();
        int                  idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }

        update_model_object();
        m_parent.set_as_dirty();
    }
    ImGui::PopStyleVar(2);
    m_imgui_end_pos[0] = m_imgui_start_pos[0] + ImGui::GetWindowWidth();
    m_imgui_end_pos[1] = m_imgui_start_pos[1] + ImGui::GetWindowHeight();
    GizmoImguiEnd();

    // BBS
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoFuzzySkin::tool_changed(wchar_t old_tool, wchar_t new_tool)
{
    if ((old_tool == ImGui::GapFillIcon && new_tool == ImGui::GapFillIcon) ||
        (old_tool != ImGui::GapFillIcon && new_tool != ImGui::GapFillIcon))
        return;

    for (auto& selector_ptr : m_triangle_selectors) {
        TriangleSelectorPatch* tsp = dynamic_cast<TriangleSelectorPatch*>(selector_ptr.get());
        tsp->set_filter_state(new_tool == ImGui::GapFillIcon);
    }
}

//Orio: a setting of the object for its fuzzy skin painting: its own, else that of the sheet of the plate shown, else that of
// the print settings (as the Coloring tool)...
static const ConfigOption *fuzzy_skin_object_option(const ModelObject &mo, const char *key)
{
    if (const ConfigOption *opt = mo.config.option(key); opt != nullptr)
        return opt;
    PartPlateList &plates    = wxGetApp().plater()->get_partplate_list();
    const auto    *sheet_tab = dynamic_cast<TabPlateSheet *>(wxGetApp().get_plate_sheet_tab());
    const int      sheet_idx = sheet_tab != nullptr ? sheet_tab->plate_index() : -1;
    PartPlate     *sheet     = sheet_idx >= 0 && sheet_idx < plates.get_plate_count() ? plates.get_plate(sheet_idx) : nullptr;
    return sheet != nullptr && sheet->has_sheet() ? sheet->process_option(key) : wxGetApp().preset_bundle->prints.get_edited_preset().config.option(key);
}

// ... and of one of its layers: that of the layer, else that of the object.
static const ConfigOption *fuzzy_skin_layer_option(const ModelObject &mo, int state, const char *key)
{
    if (const FuzzySkinLayer *layer = mo.fuzzy_skin_layer(state); layer != nullptr)
        if (const ConfigOption *opt = layer->config.option(key); opt != nullptr)
            return opt;
    return fuzzy_skin_object_option(mo, key);
}

//Orio: see the header. One line per layer, as the lines of the Modifiers tool: the button which selects it (the brush
// paints the selected layer, its eraser only erases its painting), its color, its name, then "Erase this painting" and the cross
// which deletes it (not the last one). The selected line shows the fuzzy skin settings of its layer below it, framed in blue with
// it. "Add a layer" follows the lines.
void GLGizmoFuzzySkin::render_layers(float label_column, float slider_width, float field_gap, float field_width)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    // The layers shown: those of the object, or the state 1 with the settings of the object (painted before the layers).
    FuzzySkinLayers layers = mo->fuzzy_skin_layers;
    if (layers.empty())
        layers.push_back({ 1, {} });
    std::sort(layers.begin(), layers.end(), [](const FuzzySkinLayer &l, const FuzzySkinLayer &r) { return l.state < r.state; });
    if (std::none_of(layers.begin(), layers.end(), [this](const FuzzySkinLayer &l) { return l.state == m_layer_state; }))
        m_layer_state = layers.front().state;
    for (std::unique_ptr<TriangleSelectorGUI> &selector : m_triangle_selectors)
        selector->set_erase_only_state(EnforcerBlockerType(m_layer_state));
    // The dots of the airbrush follow the point distance of the selected layer: 2.5 to 5 times it (smaller, a dot would get too few
    // points of fuzzy skin and stay smooth).
    if (const ConfigOption *distance = fuzzy_skin_layer_option(*mo, m_layer_state, "fuzzy_skin_point_distance"); distance != nullptr)
        m_airbrush_dot = 2.5f * float(distance->getFloat());
    if (! ImGui::IsAnyItemActive())
        m_layer_snapshot = false;

    const ImGuiStyle &style    = ImGui::GetStyle();
    const float       font     = m_imgui->get_font_size();
    const float       row_x    = ImGui::GetCursorPosX();
    const float       row_h    = ImGui::GetFrameHeight();
    const float       spacing  = style.ItemSpacing.x;
    const float       pastille = 0.8f * font;
    const float       width    = label_column + slider_width + field_gap + field_width - row_x;
    const float       tip_w    = ImGui::GetFontSize() * 20.0f;
    // k-th button from the end of the line (0: the cross)
    auto at_end = [&](int k) { ImGui::SameLine(row_x + width - float(k + 1) * row_h - float(k) * spacing); };

    std::optional<int> select, erase, remove;
    for (const FuzzySkinLayer &layer : layers) {
        ImGui::PushID(layer.state);
        const bool   selected = layer.state == m_layer_state;
        const ImVec2 top      = ImGui::GetCursorScreenPos();
        if (selected)
            for (const ImGuiCol col : {ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive, ImGuiCol_Border})
                ImGui::PushStyleColor(col, ImGuiWrapper::COL_BLUE_LIGHT);
        bool clicked = icon_button(m_select_icon, m_select_icon_dark, "modifier_select", row_h);
        if (selected)
            ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(selected ? _L("The brush paints with this layer.") : _L("Paint with this layer."), tip_w);
        ImGui::SameLine();
        const ImVec2                pos   = ImGui::GetCursorScreenPos();
        const std::array<float, 4> &color = fuzzy_skin_layer_color(layer.state);
        ImGui::Dummy(ImVec2(pastille, row_h));
        const float y = pos.y + 0.5f * (row_h - pastille);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + pastille, y + pastille),
                                                  ImGui::GetColorU32(ImVec4(color[0], color[1], color[2], 1.f)), 2.f);
        ImGui::SameLine();
        // the name, clickable too, without highlight (the selection shows by the frame)
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
        for (const ImGuiCol col : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive})
            ImGui::PushStyleColor(col, ImVec4(0.f, 0.f, 0.f, 0.f));
        const float name_width = std::max(font, width - (ImGui::GetCursorPosX() - row_x) - 2.f * (row_h + spacing));
        clicked |= ImGui::Selectable(into_u8(format_wxstr(_L("Fuzzy skin layer %1%"), layer.state)).c_str(), false,
                                     ImGuiSelectableFlags_NoPadWithHalfSpacing, ImVec2(name_width, row_h));
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (clicked && ! selected)
            select = layer.state;
        at_end(1);
        if (icon_button(m_erase_icon, m_erase_icon_dark, "erase", row_h))
            erase = layer.state;
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Erase the painting of this layer (the layer stays)."), tip_w);
        at_end(0);
        if (cross_button(layers.size() > 1, row_h))
            remove = layer.state;
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(layers.size() > 1 ? _L("Delete this layer and its painting.") : _L("The last layer cannot be deleted."), tip_w);
        if (selected) {
            render_layer_settings(*mo, layer.state, label_column, slider_width, field_gap, field_width);
            const ImVec2 pad(0.5f * style.ItemSpacing.x, 0.5f * style.ItemSpacing.y);
            const float  bottom = ImGui::GetCursorScreenPos().y - style.ItemSpacing.y;
            ImGui::GetWindowDrawList()->AddRect(ImVec2(top.x - pad.x, top.y - pad.y), ImVec2(top.x + width + pad.x, bottom + pad.y),
                                                ImGui::GetColorU32(ImGuiWrapper::COL_BLUE_LIGHT), style.FrameRounding, 0, std::max(1.f, 0.1f * font));
        }
        ImGui::PopID();
    }

    const bool full = layers.size() >= size_t(FUZZY_SKIN_LAYERS_MAX);
    m_imgui->disabled_begin(full);
    const bool add = m_imgui->button(_L("Add a layer"));
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        m_imgui->tooltip(full ? format_wxstr(_L("%1% layers at most."), FUZZY_SKIN_LAYERS_MAX) :
                                _L("A new layer, with its own fuzzy skin settings: only its painting gets them."), tip_w);

    // The layers change after their lines are drawn. (A line selected above the selected one, at once, left this frame without
    // settings shown: the window, sized by the content of the frame before, then hid the next lines for a moment.)
    if (select) {
        m_layer_state   = *select;
        m_layout_frames = 2;
    }
    if (add && ! full)
        add_layer(*mo);
    if (erase)
        erase_layer_painting(*erase);
    if (remove)
        delete_layer(*mo, *remove);
    // The window takes the size of its content one frame later: two frames drawn at once after a change of the lines.
    if (m_layout_frames > 0) {
        --m_layout_frames;
        m_imgui->set_requires_extra_frame();
    }
}

//Orio: the fuzzy skin settings of the layer with this state, as in the "Others" page of the settings, with its rules (the
// settings of the noise follow its type, the mode needs the Arachne wall generator). Those it does not set: the ones of the
// object, of the sheet of the plate shown, or of the print settings. A setting changed goes into the layer (one snapshot per
// gesture), the walls excepted: the contours and holes, as the painting always did, unless the layer chooses all the walls.
void GLGizmoFuzzySkin::render_layer_settings(ModelObject &mo, int state, float label_column, float slider_width, float field_gap, float field_width)
{
    auto object_option = [&mo](const char *key) { return fuzzy_skin_object_option(mo, key); };
    // (looked up each time: a setting changed may move the layers of the object)
    auto option = [&mo, state](const char *key) { return fuzzy_skin_layer_option(mo, state, key); };
    auto set = [this, &mo, state](const std::function<void(DynamicPrintConfig &)> &apply) {
        if (! m_layer_snapshot) {
            wxGetApp().plater()->take_snapshot("Change fuzzy skin layer settings", UndoRedo::SnapshotType::GizmoAction);
            m_layer_snapshot = true;
        }
        apply(layer_to_edit(mo, state).config);
        layers_changed();
    };
    const float tip_w = ImGui::GetFontSize() * 20.0f;
    // The labels of the settings, their column (after the longest one, at least that of the sliders of the window) and the end
    // of the rows (that of the slider rows of the window).
    const wxString walls_label    = _L("Walls to roughen"), thickness_label = _L("Skin thickness"), distance_label = _L("Point distance"),
                   noise_label    = _L("Noise type"), size_label = _L("Feature size"), octaves_label = _L("Octaves"),
                   persist_label  = _L("Persistence"), mode_label = _L("Generator mode");
    float          labels_width   = 0.f;
    for (const wxString *text : { &walls_label, &thickness_label, &distance_label, &noise_label, &size_label, &octaves_label, &persist_label, &mode_label })
        labels_width = std::max(labels_width, m_imgui->calc_text_size(*text).x);
    const float right  = label_column + slider_width + field_gap + field_width;
    const float column = std::max(label_column, ImGui::GetCursorPosX() + labels_width + m_imgui->get_style_scaling() * 8.f);
    const float slider = std::max(m_imgui->scaled(2.f), right - field_width - field_gap - column);
    auto label = [&](const wxString &text, const char *key) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(text);
        if (const ConfigOptionDef *def = print_config_def.get(key); ImGui::IsItemHovered() && def != nullptr && ! def->tooltip.empty())
            m_imgui->tooltip(_L(def->tooltip.c_str()), tip_w);
        ImGui::SameLine(column);
    };
    auto float_row = [&](const char *key, const wxString &text, float v_min, float v_max, const char *format, bool integer) {
        const ConfigOption *opt = option(key);
        if (opt == nullptr)
            return;
        float value = integer ? float(opt->getInt()) : float(opt->getFloat());
        label(text, key);
        ImGui::PushItemWidth(slider);
        // (not clamped while shown: a value of the object out of the slider stays shown as it is)
        bool changed = m_imgui->bbl_slider_float_style(std::string("##") + key, &value, v_min, v_max, format, 1.0f, false);
        ImGui::PopItemWidth();
        ImGui::SameLine(column + slider + field_gap);
        ImGui::PushItemWidth(field_width);
        changed |= ImGui::BBLDragFloat((std::string("##") + key + "_input").c_str(), &value, integer ? 0.1f : 0.05f, v_min, v_max, format);
        ImGui::PopItemWidth();
        if (changed) {
            value = std::clamp(value, v_min, v_max);
            set([key, value, integer](DynamicPrintConfig &cfg) {
                if (integer)
                    cfg.set_key_value(key, new ConfigOptionInt(int(std::lround(value))));
                else
                    cfg.set_key_value(key, new ConfigOptionFloat(value));
            });
        }
    };
    // a choice among some values of an enum setting (its def->enum_values)
    auto combo_row = [&](const char *key, const wxString &text, const std::vector<std::string> &values, const std::string &current) {
        const ConfigOptionDef *def = print_config_def.get(key);
        if (def == nullptr)
            return;
        auto label_of = [def](const std::string &value) {
            for (size_t i = 0; i < def->enum_values.size() && i < def->enum_labels.size(); ++i)
                if (def->enum_values[i] == value)
                    return into_u8(_L(def->enum_labels[i].c_str()));
            return value;
        };
        label(text, key);
        ImGui::PushItemWidth(right - column);
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        if (ImGui::BBLBeginCombo((std::string("##") + key).c_str(), label_of(current).c_str(), 0)) {
            for (const std::string &value : values)
                if (ImGui::Selectable(label_of(value).c_str(), value == current) && value != current)
                    set([key, value](DynamicPrintConfig &cfg) { cfg.set_deserialize_strict(key, value); });
            ImGui::EndCombo();
        }
        ImGuiWrapper::pop_combo_style();
        ImGui::PopItemWidth();
    };

    // An object refusing the fuzzy skin ignores its painting.
    if (const ConfigOption *type = object_option("fuzzy_skin"); type != nullptr && FuzzySkinType(type->getInt()) == FuzzySkinType::Disabled_fuzzy)
        m_imgui->warning_text_wrapped(_L("Fuzzy skin is disabled for this object: its painting has no effect."),
                                      right - ImGui::GetCursorPosX());

    const FuzzySkinLayer *layer = mo.fuzzy_skin_layer(state);
    combo_row("fuzzy_skin", walls_label, { "all", "allwalls" },
              layer != nullptr && layer->config.has("fuzzy_skin") ? layer->config.opt_serialize("fuzzy_skin") : std::string("all"));
    float_row("fuzzy_skin_thickness", thickness_label, 0.f, 1.f, "%.2f", false);
    float_row("fuzzy_skin_point_distance", distance_label, 0.1f, 5.f, "%.2f", false);
    if (const ConfigOption *opt = option("fuzzy_skin_first_layer"); opt != nullptr) {
        bool first_layer = opt->getBool();
        if (m_imgui->bbl_checkbox(_L("On the first layer"), first_layer))
            set([first_layer](DynamicPrintConfig &cfg) { cfg.set_key_value("fuzzy_skin_first_layer", new ConfigOptionBool(first_layer)); });
        if (const ConfigOptionDef *def = print_config_def.get("fuzzy_skin_first_layer"); ImGui::IsItemHovered() && def != nullptr)
            m_imgui->tooltip(_L(def->tooltip.c_str()), tip_w);
    }
    const ConfigOption *noise_opt = option("fuzzy_skin_noise_type");
    const NoiseType     noise     = noise_opt != nullptr ? NoiseType(noise_opt->getInt()) : NoiseType::Classic;
    if (noise_opt != nullptr)
        combo_row("fuzzy_skin_noise_type", noise_label, { "classic", "perlin", "billow", "ridgedmulti", "voronoi" }, noise_opt->serialize());
    if (noise != NoiseType::Classic)
        float_row("fuzzy_skin_scale", size_label, 0.1f, 20.f, "%.2f", false);
    if (noise == NoiseType::Perlin || noise == NoiseType::Billow || noise == NoiseType::RidgedMulti)
        float_row("fuzzy_skin_octaves", octaves_label, 1.f, 10.f, "%.0f", true);
    if (noise == NoiseType::Perlin || noise == NoiseType::Billow)
        float_row("fuzzy_skin_persistence", persist_label, 0.01f, 1.f, "%.2f", false);
    if (const ConfigOption *generator = object_option("wall_generator");
        generator != nullptr && PerimeterGeneratorType(generator->getInt()) == PerimeterGeneratorType::Arachne)
        if (const ConfigOption *mode = option("fuzzy_skin_mode"); mode != nullptr)
            combo_row("fuzzy_skin_mode", mode_label, { "displacement", "extrusion", "combined" }, mode->serialize());
}

// A small icon button (resources/images/<name>.svg, <name>_dark.svg), as those of the lines of the Modifiers tool.
bool GLGizmoFuzzySkin::icon_button(ImTextureID &light, ImTextureID &dark, const std::string &name, float size)
{
    ImTextureID &icon = m_is_dark_mode ? dark : light;
    if (icon == nullptr) {
        const std::string file = Slic3r::resources_dir() + "/images/" + name + (m_is_dark_mode ? "_dark.svg" : ".svg");
        const unsigned    px   = unsigned(std::max(16.f, size * 2.f));
        if (!IMTexture::load_from_svg_file(file, px, px, icon))
            icon = nullptr;
    }
    const float padding = std::max(1.f, 0.12f * size);
    ImGui::PushID(name.c_str());
    const bool clicked = icon != nullptr ? m_imgui->image_button(icon, ImVec2(size - 2.f * padding, size - 2.f * padding), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), int(padding)) :
                                           ImGui::Button("?", ImVec2(size, size));
    ImGui::PopID();
    return clicked;
}

// The little cross at the end of a line, as in the Modifiers tool: red when hovered, greyed when disabled.
bool GLGizmoFuzzySkin::cross_button(bool enabled, float size)
{
    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton("##delete", ImVec2(size, size)) && enabled;
    const bool   hovered = enabled && ImGui::IsItemHovered();
    const ImU32  color   = !enabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled) :
                           hovered  ? ImGui::GetColorU32(ImVec4(0.95f, 0.3f, 0.3f, 1.f)) :
                                      ImGui::GetColorU32(ImGuiCol_Text);
    const float  margin  = 0.28f * size;
    ImDrawList  *draw    = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(pos.x + margin, pos.y + margin), ImVec2(pos.x + size - margin, pos.y + size - margin), color, 1.5f);
    draw->AddLine(ImVec2(pos.x + size - margin, pos.y + margin), ImVec2(pos.x + margin, pos.y + size - margin), color, 1.5f);
    return clicked;
}

FuzzySkinLayer &GLGizmoFuzzySkin::layer_to_edit(ModelObject &mo, int state)
{
    if (mo.fuzzy_skin_layers.empty())
        mo.fuzzy_skin_layers.push_back({ 1, {} });
    auto it = std::find_if(mo.fuzzy_skin_layers.begin(), mo.fuzzy_skin_layers.end(), [state](const FuzzySkinLayer &l) { return l.state == state; });
    if (it != mo.fuzzy_skin_layers.end())
        return *it;
    mo.fuzzy_skin_layers.push_back({ state, {} });
    return mo.fuzzy_skin_layers.back();
}

// Only the painting of this layer: the other layers and their paintings stay.
void GLGizmoFuzzySkin::erase_layer_painting(int state)
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Erase fuzzy skin layer painting", UndoRedo::SnapshotType::GizmoAction);
    for (std::unique_ptr<TriangleSelectorGUI> &selector : m_triangle_selectors) {
        selector->replace_state(EnforcerBlockerType(state), EnforcerBlockerType::NONE);
        selector->request_update_render_data(true);
    }
    update_model_object();
    m_parent.set_as_dirty();
}

// A new layer, with the first state free: no layer and nothing painted with it (a painting kept without its layer, after objects
// were merged, is not taken over). Without stored layers, the state 1 is the one shown.
void GLGizmoFuzzySkin::add_layer(ModelObject &mo)
{
    int state = 0;
    for (int s = 1; s <= FUZZY_SKIN_LAYERS_MAX && state == 0; ++s)
        if ((s != 1 || ! mo.fuzzy_skin_layers.empty()) && mo.fuzzy_skin_layer(s) == nullptr &&
            std::none_of(m_triangle_selectors.begin(), m_triangle_selectors.end(),
                         [s](const std::unique_ptr<TriangleSelectorGUI> &selector) { return selector->has_facets(EnforcerBlockerType(s)); }))
            state = s;
    if (state == 0)
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Add fuzzy skin layer", UndoRedo::SnapshotType::GizmoAction);
    if (mo.fuzzy_skin_layers.empty())
        mo.fuzzy_skin_layers.push_back({ 1, {} });
    mo.fuzzy_skin_layers.push_back({ state, {} });
    m_layer_state = state;
    layers_changed();
}

// The layer and its painting (never the last layer shown, see render_layers()).
void GLGizmoFuzzySkin::delete_layer(ModelObject &mo, int state)
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Delete fuzzy skin layer", UndoRedo::SnapshotType::GizmoAction);
    for (std::unique_ptr<TriangleSelectorGUI> &selector : m_triangle_selectors) {
        selector->replace_state(EnforcerBlockerType(state), EnforcerBlockerType::NONE);
        selector->request_update_render_data(true);
    }
    update_model_object();
    mo.fuzzy_skin_layers.erase(std::remove_if(mo.fuzzy_skin_layers.begin(), mo.fuzzy_skin_layers.end(),
                                              [state](const FuzzySkinLayer &l) { return l.state == state; }),
                               mo.fuzzy_skin_layers.end());
    layers_changed();
}

// The settings of the painted regions changed: sliced again (see Print::apply()).
void GLGizmoFuzzySkin::layers_changed()
{
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
    m_layout_frames = 2;
}

void GLGizmoFuzzySkin::show_tooltip_information(float caption_max, float x, float y)
{
    //Orio: shown by the keyboard icon right of the name of the tool, in the title bar of its window.
    std::vector<std::pair<wxString, wxString>> shortcuts;
    auto draw_text_with_caption = [&shortcuts](const wxString &caption, const wxString &text) { shortcuts.emplace_back(caption, text); };

    std::vector<std::string> tip_items;
    switch (m_tool_type) {
        case ToolType::BRUSH:
            tip_items = {"add_fuzzyskin",  "remove", "cursor_size", "clipping_of_view"};
            break;
        case ToolType::BUCKET_FILL:
            break;
        case ToolType::SMART_FILL:
            tip_items = {"add_fuzzyskin", "remove", "smart_fill_angle", "clipping_of_view"};
            break;
        default:
            break;
    }
    for (const auto &t : tip_items)
        draw_text_with_caption(m_desc.at(t + "_caption") + ": ", m_desc.at(t));

    m_imgui->tool_window_shortcuts(shortcuts);
}

//BBS: remove const
void GLGizmoFuzzySkin::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->fuzzy_skin_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());

        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

//BBS
void GLGizmoFuzzySkin::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject* mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    //BBS: add timestamp logic
    m_volume_timestamps.clear();

    int volume_id = -1;
    std::vector<std::array<float, 4>> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    //Orio: one color per layer, indexed by its state (a state beyond the layers, from elsewhere: the last color, the
    // patches are indexed up to ExtruderMax).
    ebt_colors.insert(ebt_colors.end(), fuzzy_skin_layer_colors.begin(), fuzzy_skin_layer_colors.end());
    ebt_colors.resize(size_t(EnforcerBlockerType::ExtruderMax) + 1, fuzzy_skin_layer_colors.back());
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        ++volume_id;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor, so we don't need it to perform it again in deserialize().
        m_triangle_selectors.back()->deserialize(mv->fuzzy_skin_facets.get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
        //Orio: the eraser only erases the painting of the selected layer (see render_layers()).
        m_triangle_selectors.back()->set_erase_only_state(EnforcerBlockerType(m_layer_state));
        //BBS: add timestamp logic
        m_volume_timestamps.emplace_back(mv->fuzzy_skin_facets.timestamp());
    }
}

PainterGizmoType GLGizmoFuzzySkin::get_painter_type() const
{
    return PainterGizmoType::FUZZY_SKIN;
}

wxString GLGizmoFuzzySkin::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    // BBS remove _L()
    wxString action_name;
    if (shift_down)
        action_name = ("Unselect all");
    else {
        if (button_down == Button::Left)
            action_name = ("Add fuzzy skin");
        else
            action_name = ("Remove fuzzy skin");
    }
    return action_name;
}

} // namespace Slic3r::GUI
