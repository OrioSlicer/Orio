#include "GLGizmoFdmSupports.hpp"

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
//Orio: manual supports enabled when painting (enable_manual_supports()).
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/UndoRedo.hpp"


#include <GL/glew.h>

#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {

//Orio: supports painting, color of each painted style, in the order of PAINTED_SUPPORT_STYLES.
static constexpr std::array<std::array<float, 4>, PAINTED_SUPPORT_STYLES.size()> painted_style_colors {{
    TriangleSelectorGUI::enforcers_color, // classic, snug: lavender (as before)
    { 0.40f, 0.70f, 0.90f, 1.f },         // classic, grid: sky blue
    { 0.580f, 0.800f, 0.545f, 1.f },      // tree, default: soft green (as before)
    { 0.90f, 0.86f, 0.38f, 1.f },         // tree, slim: lemon yellow
    { 0.25f, 0.55f, 0.30f, 1.f },         // tree, strong: dark green
    { 0.35f, 0.78f, 0.72f, 1.f },         // tree, hybrid: turquoise
    { 0.60f, 0.42f, 0.78f, 1.f },         // tree, organic: purple
}};

// Index in PAINTED_SUPPORT_STYLES of a painted state.
static size_t painted_style_index(EnforcerBlockerType state)
{
    for (size_t i = 0; i < PAINTED_SUPPORT_STYLES.size(); ++ i)
        if (PAINTED_SUPPORT_STYLES[i].state == state)
            return i;
    return 0;
}

// Name of a painted style, in its painting mode (classic or tree).
static wxString painted_style_label(size_t idx)
{
    switch (PAINTED_SUPPORT_STYLES[idx].style) {
    case smsSnug:        return _L("Snug");
    case smsGrid:        return _L("Grid");
    case smsTreeSlim:    return _CTX(L_CONTEXT("Slim", "Supports painting"), "Supports painting");
    case smsTreeStrong:  return _CTX(L_CONTEXT("Strong", "Supports painting"), "Supports painting");
    case smsTreeHybrid:  return _CTX(L_CONTEXT("Hybrid", "Supports painting"), "Supports painting");
    case smsTreeOrganic: return _CTX(L_CONTEXT("Organic", "Supports painting"), "Supports painting");
    default:             return _L("Default");
    }
}

GLGizmoFdmSupports::GLGizmoFdmSupports(GLCanvas3D& parent, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, sprite_id), m_current_tool(ImGui::SphereButtonIcon)
{
    //Orio: no Circle tool in the supports painting (removed at the request of the user), Sphere by default.
    m_tool_type = ToolType::BRUSH;
    m_cursor_type = TriangleSelector::CursorType::SPHERE;
}

void GLGizmoFdmSupports::data_changed(bool is_serializing) {
    set_painter_gizmo_data(m_parent.get_selection());
}

void GLGizmoFdmSupports::on_shutdown()
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

    m_highlight_by_angle_threshold_deg = 0.f;
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

//BBS: add on_open
void GLGizmoFdmSupports::on_opening()
{
    m_angle_threshold_deg = 40;
    m_parent.set_slope_normal_angle(90.f - m_angle_threshold_deg);
    if (! m_parent.is_using_slope()) {
        m_parent.use_slope(true);
        m_parent.set_as_dirty();
    }
    m_print_instance.print_object = NULL;
    m_print_instance.model_instance = NULL;
    m_edit_state = state_idle;

    m_volume_ready = false;
    m_volume_valid = false;
}

std::string GLGizmoFdmSupports::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        return _u8L("Supports Painting") + ":\n" + _u8L("Please select single object.");
    } else {
        return _u8L("Supports Painting");
    }
}

bool GLGizmoFdmSupports::on_init()
{
    // BBS
    m_shortcut_key = WXK_CONTROL_L;
    const wxString ctrl                = GUI::shortkey_ctrl_prefix();
    const wxString alt                 = GUI::shortkey_alt_prefix();
    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]      = _L("Section view");
    m_desc["reset_direction"]       = _L("Reset direction");
    m_desc["cursor_size_caption"]   = _L("Ctrl + Mouse wheel");
    m_desc["cursor_size"]           = _L("Pen size");
    m_desc["enforce_caption"]       = _L("Left mouse button");
    m_desc["enforce"]               = _L("Enforce supports");
    m_desc["block_caption"]         = _L("Right mouse button");
    m_desc["block"]                 = _L("Block supports");
    m_desc["remove_caption"]        = _L("Shift + Left mouse button");
    m_desc["remove"]                = _L("Erase");
    m_desc["remove_all"]            = _L("Erase all painting");
    m_desc["highlight_by_angle"]    = _L("Highlight overhang areas");
    m_desc["gap_fill"]              = _L("Gap fill");
    m_desc["perform"]               = _L("Perform");
    m_desc["gap_area_caption"]      = ctrl +_L("Mouse wheel");
    m_desc["gap_area"]              = _L("Gap area");
    m_desc["tool_type"]             = _L("Tool type");
    m_desc["smart_fill_angle_caption"] = ctrl + _L("Mouse wheel");
    m_desc["smart_fill_angle"]      = _L("Smart fill angle");
    m_desc["on_overhangs_only"] = _L("On overhangs only");
    //Orio: painting mode, classic or tree supports.
    m_desc["painting_mode"]         = _L("Painted supports");
    m_desc["mode_classic"]          = _CTX(L_CONTEXT("Classic", "Supports painting"), "Supports painting");
    m_desc["mode_tree"]             = _CTX(L_CONTEXT("Tree", "Supports painting"), "Supports painting");
    m_desc["paint_style"]           = _L("Style");
    m_desc["enforce_classic"]       = _L("Enforce classic supports");
    m_desc["enforce_tree"]          = _L("Enforce tree supports");

    memset(&m_print_instance, 0, sizeof(m_print_instance));
    return true;
}

void GLGizmoFdmSupports::render_painter_gizmo() const
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);
    //BBS: draw support volumes
    if (m_volume_ready && m_support_volume && (m_edit_state != state_generating))
    {
        m_support_volume->set_render_color({ 0.f, 0.7f, 0.f, 0.7f });

        const auto &                      camera = m_parent.get_active_camera();
        std::vector<std::array<float, 4>> colors = m_parent.get_active_colors();
        m_support_volume->render(camera, colors, m_parent.get_ref_model());
    }

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

// BBS
bool GLGizmoFdmSupports::on_key_down_select_tool_type(int keyCode) {
    switch (keyCode)
    {
    case 'F':
        m_current_tool = ImGui::FillButtonIcon;
        break;
    case 'S':
        m_current_tool = ImGui::SphereButtonIcon;
        break;
    case 'C':
        //Orio: was the Circle tool (removed), still swallowed so that C does not open another tool.
        break;
    case 'G':
        m_current_tool = ImGui::GapFillIcon;
        break;
    default:
        return false;
        break;
    }
    return true;
}

std::string GLGizmoFdmSupports::get_icon_filename(bool b_dark_mode) const
{
    return b_dark_mode ? "toolbar_support_dark.svg" : "toolbar_support.svg";
}

// BBS
void GLGizmoFdmSupports::render_triangles(const Selection& selection) const
{
    ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    const auto& shader = wxGetApp().get_shader("mm_gouraud");
    if (!shader)
        return;
    wxGetApp().bind_shader(shader);
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);
    ScopeGuard guard([shader]() { if (shader) wxGetApp().unbind_shader(); });

    const ModelObject* mo = m_c->selection_info()->model_object();
    int                mesh_id = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        ++mesh_id;

        const Transform3d trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();

        bool is_left_handed = trafo_matrix.matrix().determinant() < 0.;
        if (is_left_handed)
            glsafe(::glFrontFace(GL_CW));

        const Camera& camera = wxGetApp().plater()->get_camera();
        const Transform3d matrix = camera.get_view_matrix() * trafo_matrix;
        shader->set_uniform("view_model_matrix", matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("normal_matrix", (Matrix3d)matrix.matrix().block(0, 0, 3, 3).inverse().transpose());

        float normal_z = -::cos(Geometry::deg2rad(m_highlight_by_angle_threshold_deg));
        Matrix3f normal_matrix = static_cast<Matrix3f>(trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>());

        shader->set_uniform("volume_world_matrix", trafo_matrix);
        shader->set_uniform("volume_mirrored", is_left_handed);
        shader->set_uniform("slope.actived", m_parent.is_using_slope());
        shader->set_uniform("slope.volume_world_normal_matrix", normal_matrix);
        shader->set_uniform("slope.normal_z", normal_z);
        m_triangle_selectors[mesh_id]->render(m_imgui, trafo_matrix);

        if (is_left_handed)
            glsafe(::glFrontFace(GL_CCW));
    }
}

void GLGizmoFdmSupports::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == On) {
        m_support_threshold_angle = -1;
    }
    else if (get_state() == Off) {
        ModelObject* mo = m_c->selection_info()->model_object();
        if (mo) Slic3r::save_object_mesh(*mo);
    }
}

void GLGizmoFdmSupports::on_render_input_window(float x, float y, float bottom_limit)
{
    init_print_instance();
    if (!m_c) {
        return;
    }
    const auto& p_selection_info = m_c->selection_info();
    if (!p_selection_info) {
        return;
    }
    const auto& p_model_object = p_selection_info->model_object();
    if (!p_model_object)
        return;
    m_imgui_start_pos[0] = x;
    m_imgui_start_pos[1] = y;
    // BBS
    wchar_t old_tool = m_current_tool;

    int support_threshold_angle = get_selection_support_threshold_angle();
    // when support painting tool is on, reset highlight threshold angle
    if (m_support_threshold_angle == -1) {
        m_highlight_by_angle_threshold_deg = support_threshold_angle;
        m_parent.set_slope_normal_angle(90.f - m_highlight_by_angle_threshold_deg);
        //Orio: painting mode of the support type of the object (tree supports for tree(auto), classic ones
        // for normal(auto)), the last one used for manual supports.
        const DynamicPrintConfig &obj_cfg = p_model_object->config.get();
        const DynamicPrintConfig &glb_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        const SupportType support_type = obj_cfg.option("support_type") ? obj_cfg.opt_enum<SupportType>("support_type") : glb_cfg.opt_enum<SupportType>("support_type");
        if (support_type == stTreeAuto)
            m_paint_tree_supports = true;
        else if (support_type == stNormalAuto)
            m_paint_tree_supports = false;
    }
    m_support_threshold_angle = support_threshold_angle;

    const float approx_height = m_imgui->scaled(23.f);
    y = std::min(y, bottom_limit - approx_height);

    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);

    //BBS
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size = m_imgui->get_style_scaling() * 8;
    const float on_overhangs_only_width  = m_imgui->calc_text_size(m_desc["on_overhangs_only"]).x + m_imgui->scaled(1.5f);
    const float remove_btn_width        = m_imgui->calc_text_size(m_desc.at("remove_all")).x + m_imgui->scaled(1.5f);
    const float filter_btn_width        = m_imgui->calc_text_size(m_desc.at("perform")).x + m_imgui->scaled(1.5f);
    const float buttons_width           = remove_btn_width + filter_btn_width + m_imgui->scaled(1.5f);
    const float empty_button_width      = m_imgui->calc_button_size("").x;

    const float tips_width           = m_imgui->calc_text_size(_L("Auto support threshold angle: ") + " 90 ").x + m_imgui->scaled(1.5f);
    const float minimal_slider_width = m_imgui->scaled(4.f);

    float caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 5>{"enforce", "block", "remove", "cursor_size", "clipping_of_view"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }
    total_text_max += caption_max + m_imgui->scaled(1.f);
    caption_max += m_imgui->scaled(1.f);

    //Orio: the sliders (and the tool icons) just after the longest label shown for the current tool.
    const char *tool_label = m_current_tool == ImGui::FillButtonIcon ? "smart_fill_angle" : (m_current_tool == ImGui::GapFillIcon ? "gap_area" : "cursor_size");
    const float sliders_left_width = ImGui::GetStyle().WindowPadding.x + space_size +
        std::max({m_imgui->calc_text_size(m_desc.at("tool_type")).x, m_imgui->calc_text_size(m_desc.at(tool_label)).x,
                  m_imgui->calc_text_size(m_desc.at("highlight_by_angle")).x,
                  m_current_tool != ImGui::GapFillIcon ? clipping_label_width(m_desc.at("clipping_of_view")) : 0.f});
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    const float sliders_width = m_imgui->scaled(7.0f);
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_left_width + sliders_width - space_size;
    float       window_width    = minimal_slider_width + sliders_left_width + slider_icon_width;
    float drag_pos_times     = 0.7;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("tool_type"));
    //Orio: no Circle tool.
    std::array<wchar_t, 3> tool_ids = { ImGui::SphereButtonIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wchar_t, 3> icons;
    if (m_is_dark_mode)
        icons = { ImGui::SphereButtonDarkIcon, ImGui::FillButtonDarkIcon, ImGui::GapFillDarkIcon };
    else
        icons = { ImGui::SphereButtonIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };

    std::array<wxString, 3> tool_tips = { _L("Sphere"), _L("Fill"), _L("Gap Fill") };
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

    //Orio: painting mode, classic or tree supports.
    render_painting_mode(max_tooltip_width);

    m_imgui->bbl_checkbox(m_desc["on_overhangs_only"], m_paint_on_overhangs_only);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(format_wxstr(_L("Allows painting only on facets selected by: \"%1%\""), m_desc["highlight_by_angle"]), max_tooltip_width);
    ImGui::Separator();

    if (m_current_tool != old_tool)
        this->tool_changed(old_tool, m_current_tool);

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool == ImGui::SphereButtonIcon) {
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
    } else if (m_current_tool == ImGui::GapFillIcon) {
        m_tool_type = ToolType::GAP_FILL;
        m_cursor_type = TriangleSelector::CursorType::POINTER;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["gap_area"]);
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        std::string format_str = std::string("%.2f") + I18N::translate_utf8("", "Triangle patch area threshold,""triangle patch will be merged to neighbor if its area is less than threshold");
        m_imgui->bbl_slider_float_style("##gap_area", &TriangleSelectorPatch::gap_area, TriangleSelectorPatch::GapAreaMin, TriangleSelectorPatch::GapAreaMax, format_str.data(), 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##gap_area_input", &TriangleSelectorPatch::gap_area, 0.05f, 0.0f, 0.0f, "%.2f");
    }

    float position_before_text_y = ImGui::GetCursorPos().y;
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["highlight_by_angle"]);
    ImGui::AlignTextToFramePadding();
    float position_after_text_y = ImGui::GetCursorPos().y;
    ImGui::SameLine(sliders_left_width);

    float slider_height = m_imgui->get_slider_float_height();
    // Makes slider to be aligned to bottom of the multi-line text.
    float slider_start_position_y = std::max(position_before_text_y, position_after_text_y - slider_height);
    ImGui::SetCursorPosY(slider_start_position_y);

    std::string format_str = std::string("%.f");
    ImGui::PushItemWidth(sliders_width);
    wxString tooltip = _L("Highlight faces according to overhang angle.");
    if (m_imgui->bbl_slider_float_style("##angle_threshold_deg", &m_highlight_by_angle_threshold_deg, 0.f, 90.f, format_str.data(), 1.0f, true, tooltip)) {
        m_parent.set_slope_normal_angle(90.f - m_highlight_by_angle_threshold_deg);
        if (!m_parent.is_using_slope()) {
            m_parent.use_slope(true);
            m_parent.set_as_dirty();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (m_support_threshold_angle != 0) {
            wxString str_tooltip = (_L("Auto support threshold angle: ") + std::to_string(m_support_threshold_angle));
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, str_tooltip);
        } else {
            wxString s_tooltip = (_L("No auto support"));
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, s_tooltip);
        }
        ImGui::EndTooltip();
    }
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    ImGui::BBLDragFloat("##angle_threshold_deg_input", &m_highlight_by_angle_threshold_deg, 0.05f, 0.0f, 0.0f, "%.2f");

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

    ImGui::Separator();
    if (m_parent.is_volumes_selected_and_sinking()) {
        m_imgui->warning_text_wrapped(_L("Warning") + ":" + _L("Painting below the build plate is not allowed.") +
                                          _L("The white outline indicates the position of the build plate at Z = 0."),
                                      window_width+ m_imgui->scaled(3));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    // Orio: under the window where it is (it can be moved).
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + ImGui::GetWindowPos().y;
    show_tooltip_information(caption_max, ImGui::GetWindowPos().x, get_cur_y);

    float f_scale = m_parent.get_main_toolbar_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    // Perform button is for gap fill
    if (m_current_tool == ImGui::GapFillIcon) {
        if (m_imgui->button(m_desc.at("perform"))) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);

            for (int i = 0; i < m_triangle_selectors.size(); i++) {
                TriangleSelectorPatch* ts_mm = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[i].get());
                ts_mm->update_selector_triangles();
                ts_mm->request_update_render_data(true);
            }
            update_model_object();
            m_parent.set_as_dirty();
        }
        ImGui::SameLine();
    }

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
    //Orio: the rectangle of the window where it really is (it floats), none when it is detached (not over the view).
    if (m_imgui->tool_window_detached()) {
        m_imgui_start_pos = Vec2i(1, 1);
        m_imgui_end_pos   = Vec2i(0, 0);
    } else {
        const ImVec2 pos  = ImGui::GetWindowPos();
        m_imgui_start_pos = Vec2i(int(pos.x), int(pos.y));
        m_imgui_end_pos   = Vec2i(int(pos.x + ImGui::GetWindowWidth()), int(pos.y + ImGui::GetWindowHeight()));
    }
    GizmoImguiEnd();

    // BBS
    ImGuiWrapper::pop_toolbar_style();
}

//Orio: a row "Style" with the style painted in the mode, then the painting modes, classic or tree supports, each
// button after a square of the color of the style it paints.
void GLGizmoFdmSupports::render_painting_mode(float max_tooltip_width)
{
    const float  frame_height = ImGui::GetFrameHeight();
    const ImVec4 selected_bg  = m_is_dark_mode ? ImVec4(26 / 255.0f, 53 / 255.0f, 80 / 255.0f, 1.00f) : ImVec4(0.86f, 0.99f, 0.91f, 1.00f);
    // A square of the color of a style, vertically centered on a line of the given height.
    auto color_square = [frame_height](const ImVec2 &pos, float line_height, size_t style_idx) {
        const std::array<float, 4> &color = painted_style_colors[style_idx];
        const float                 top   = pos.y + 0.5f * (line_height - frame_height * 0.5f);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, top), ImVec2(pos.x + frame_height * 0.5f, top + frame_height * 0.5f),
            ImGui::GetColorU32(ImVec4(color[0], color[1], color[2], 1.f)), 2.f);
    };

    // The style painted in the mode, below "Tool type", its menu where the tool icons start: each style after a square of
    // its color. As wide for both modes.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("paint_style"));
    ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + m_imgui->get_style_scaling() * 8 +
                    std::max(m_imgui->calc_text_size(m_desc.at("tool_type")).x, m_imgui->calc_text_size(m_desc.at("paint_style")).x));
    const float square_space = frame_height * 0.5f + ImGui::GetStyle().ItemInnerSpacing.x;
    float       labels_width = 0.f;
    for (size_t idx = 0; idx < PAINTED_SUPPORT_STYLES.size(); ++ idx)
        labels_width = std::max(labels_width, m_imgui->calc_text_size(painted_style_label(idx)).x);
    // The frame of BBLBeginCombo: its width minus its arrows.
    ImGui::SetNextItemWidth(labels_width + 3.f * frame_height + 2.f * ImGui::GetStyle().FramePadding.x);
    const size_t current = painted_style_index(m_paint_state[m_paint_tree_supports]);
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    const bool open = ImGui::BBLBeginCombo("##paint_style", into_u8(painted_style_label(current)).c_str(), 0);
    if (open) {
        for (size_t idx = 0; idx < PAINTED_SUPPORT_STYLES.size(); ++ idx) {
            if (PAINTED_SUPPORT_STYLES[idx].tree != m_paint_tree_supports)
                continue;
            ImGui::PushID(int(idx));
            // Where the label of the line would be: its square, then its name.
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            // As the other menus of the tools: only the hovered line lights up, the choice is read in the field.
            if (ImGui::Selectable("##paint_style_item", false, 0, ImVec2(0.f, ImGui::GetTextLineHeight())))
                m_paint_state[m_paint_tree_supports] = PAINTED_SUPPORT_STYLES[idx].state;
            color_square(pos, ImGui::GetTextLineHeight(), idx);
            ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + square_space, pos.y), ImGui::GetColorU32(ImGuiCol_Text), into_u8(painted_style_label(idx)).c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGuiWrapper::pop_combo_style();
    if (! open && ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Style of the supports painted in this mode, each style with its color (see the keyboard icon of the window)."), max_tooltip_width);

    // Orio: without a label, the two modes at the left of the window, below the style.
    ImGui::AlignTextToFramePadding();
    for (const bool tree : { false, true }) {
        const bool selected = tree == m_paint_tree_supports;
        if (tree)
            ImGui::SameLine();
        color_square(ImGui::GetCursorScreenPos(), frame_height, painted_style_index(m_paint_state[tree]));
        ImGui::Dummy(ImVec2(frame_height * 0.5f, frame_height));
        ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, selected_bg);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected_bg);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, selected_bg);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.00f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
        }
        const bool clicked = ImGui::Button((into_u8(m_desc.at(tree ? "mode_tree" : "mode_classic")) + (tree ? "##paint_tree" : "##paint_classic")).c_str());
        if (selected) {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(1);
        }
        if (clicked)
            m_paint_tree_supports = tree;
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tree ? _L("The painted areas get tree supports.") : _L("The painted areas get normal (classic) supports."), max_tooltip_width);
    }
}

//Orio: the color of the style painted, see-through.
std::array<float, 4> GLGizmoFdmSupports::get_cursor_sphere_left_button_color() const
{
    std::array<float, 4> color = painted_style_colors[painted_style_index(this->get_left_button_state_type())];
    color[3] = 0.4f;
    return color;
}

void GLGizmoFdmSupports::tool_changed(wchar_t old_tool, wchar_t new_tool)
{
    if ((old_tool == ImGui::GapFillIcon && new_tool == ImGui::GapFillIcon) ||
        (old_tool != ImGui::GapFillIcon && new_tool != ImGui::GapFillIcon))
        return;

    for (auto& selector_ptr : m_triangle_selectors) {
        TriangleSelectorPatch* tsp = dynamic_cast<TriangleSelectorPatch*>(selector_ptr.get());
        tsp->set_filter_state(new_tool == ImGui::GapFillIcon);
    }
}

void GLGizmoFdmSupports::show_tooltip_information(float caption_max, float x, float y)
{
    //Orio: shown by the keyboard icon right of the name of the tool, in the title bar of its window.
    std::vector<std::pair<wxString, wxString>> shortcuts;
    auto draw_text_with_caption = [&shortcuts](const wxString &caption, const wxString &text) { shortcuts.emplace_back(caption, text); };

    std::vector<std::string> tip_items;
    switch (m_tool_type) {
        case ToolType::BRUSH:
            tip_items = {"enforce", "block", "remove", "cursor_size", "clipping_of_view"};
            break;
        case ToolType::BUCKET_FILL:
            break;
        case ToolType::SMART_FILL:
            tip_items = {"enforce", "block", "remove", "smart_fill_angle", "clipping_of_view"};
            break;
        case ToolType::GAP_FILL:
            tip_items = {"gap_area"};
            break;
        default:
            break;
    }
    //Orio: the left mouse button paints the supports of the painting mode, in the style chosen.
    const wxString enforce = m_desc.at(m_paint_tree_supports ? "enforce_tree" : "enforce_classic") + " (" +
                             painted_style_label(painted_style_index(this->get_left_button_state_type())) + ")";
    for (const auto &t : tip_items)
        draw_text_with_caption(m_desc.at(t + "_caption") + ": ", t == "enforce" ? enforce : m_desc.at(t));

    //Orio: below them, the colors of the painting: each style in its mode, the blocked supports.
    std::vector<std::pair<std::array<float, 4>, wxString>> legend;
    for (size_t idx = 0; idx < PAINTED_SUPPORT_STYLES.size(); ++ idx)
        legend.emplace_back(painted_style_colors[idx], m_desc.at(PAINTED_SUPPORT_STYLES[idx].tree ? "mode_tree" : "mode_classic") + " - " + painted_style_label(idx));
    legend.emplace_back(TriangleSelectorGUI::blockers_color, _L("Blocked supports"));
    m_imgui->tool_window_shortcuts(shortcuts, legend);
}

// BBS
int GLGizmoFdmSupports::get_selection_support_threshold_angle()
{
    auto sel_info = m_c->selection_info();
    if (sel_info == nullptr)
        return -1;

    const DynamicPrintConfig& obj_cfg = sel_info->model_object()->config.get();
    const DynamicPrintConfig& glb_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    bool enable_support = obj_cfg.option("enable_support") ? obj_cfg.opt_bool("enable_support") : glb_cfg.opt_bool("enable_support");
    SupportType support_type = obj_cfg.option("support_type") ? obj_cfg.opt_enum<SupportType>("support_type") : glb_cfg.opt_enum<SupportType>("support_type");
    int support_threshold_angle = obj_cfg.option("support_threshold_angle") ? obj_cfg.opt_int("support_threshold_angle") : glb_cfg.opt_int("support_threshold_angle");

    bool auto_support = enable_support && is_auto(support_type);
    return auto_support ? support_threshold_angle : 0;
}

void GLGizmoFdmSupports::select_facets_by_angle(float threshold_deg, bool block)
{
    float threshold = (float(M_PI)/180.f)*threshold_deg;
    const Selection& selection = m_parent.get_selection();
    const ModelObject* mo = m_c->selection_info()->model_object();
    const ModelInstance* mi = mo->instances[selection.get_instance_idx()];

    int mesh_id = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        ++mesh_id;

        const Transform3d trafo_matrix = mi->get_matrix(true) * mv->get_matrix(true);
        Vec3f down  = (trafo_matrix.inverse() * (-Vec3d::UnitZ())).cast<float>().normalized();
        Vec3f limit = (trafo_matrix.inverse() * Vec3d(std::sin(threshold), 0, -std::cos(threshold))).cast<float>().normalized();

        float dot_limit = limit.dot(down);

        // Now calculate dot product of vert_direction and facets' normals.
        int idx = 0;
        const indexed_triangle_set &its = mv->mesh().its;
        for (const stl_triangle_vertex_indices &face : its.indices) {
            if (its_face_normal(its, face).dot(down) > dot_limit) {
                m_triangle_selectors[mesh_id]->set_facet(idx, block ? EnforcerBlockerType::BLOCKER : this->get_left_button_state_type());
                m_triangle_selectors.back()->request_update_render_data();
            }
            ++ idx;
        }
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), block ? "Block supports by angle"
                                                    : "Add supports by angle");
    update_model_object();
    m_parent.set_as_dirty();
}

//BBS: remove const
void GLGizmoFdmSupports::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    //Orio: first supports painted on this object (painting blockers only does not count).
    const bool had_enforcers = this->has_painted_enforcers(*mo);
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->supported_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        if (! had_enforcers && this->has_painted_enforcers(*mo))
            this->enable_manual_supports(*mo);

        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());

        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }

    //BBS: invalid volume_support status
    invalid_support_volumes(true);
}

//Orio: see GLGizmoFdmSupports.hpp.
bool GLGizmoFdmSupports::has_painted_enforcers(const ModelObject &mo) const
{
    for (const ModelVolume *mv : mo.volumes)
        if (volume_has_painted_supports(*mv))
            return true;
    return false;
}

//Orio: see GLGizmoFdmSupports.hpp. Called after the snapshot of the painting action: undo restores the settings too.
void GLGizmoFdmSupports::enable_manual_supports(ModelObject &mo)
{
    const DynamicPrintConfig &obj_cfg        = mo.config.get();
    const DynamicPrintConfig &glb_cfg        = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const bool                enable_support = obj_cfg.option("enable_support") ? obj_cfg.opt_bool("enable_support") : glb_cfg.opt_bool("enable_support");
    const SupportType         support_type   = obj_cfg.option("support_type") ? obj_cfg.opt_enum<SupportType>("support_type") : glb_cfg.opt_enum<SupportType>("support_type");
    if (enable_support && support_type == stManual)
        return;
    if (! enable_support)
        mo.config.set_key_value("enable_support", new ConfigOptionBool(true));
    if (support_type != stManual)
        mo.config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stManual));

    // Settings of the object in the object list, and in the settings tab of the object if it shows them.
    ObjectVolumeID object_volume_id;
    object_volume_id.object = &mo;
    wxGetApp().obj_list()->object_config_options_changed(object_volume_id);
    if (TabPrintModel *tab = dynamic_cast<TabPrintModel*>(wxGetApp().get_model_tab()))
        tab->update_model_config();

    wxGetApp().plater()->get_notification_manager()->push_notification(NotificationType::CustomNotification,
        NotificationManager::NotificationLevel::RegularNotificationLevel,
        // GUI::format: the member GLGizmoBase::format(float, unsigned) hides it.
        GUI::format(_L("Supports painted on \"%1%\": its supports are enabled, type Manual (only the painted areas get supports)."), mo.name));
}

//BBS
void GLGizmoFdmSupports::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject* mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    //BBS: add timestamp logic
    m_volume_timestamps.clear();

    int volume_id = -1;
    //Orio: a color per painted state, indexed by state: the blocked supports, each painted style.
    std::vector<std::array<float, 4>> ebt_colors(size_t(EnforcerBlockerType::BLOCKER) + 1, GLVolume::NEUTRAL_COLOR);
    ebt_colors[size_t(EnforcerBlockerType::BLOCKER)] = TriangleSelectorGUI::blockers_color;
    for (size_t idx = 0; idx < PAINTED_SUPPORT_STYLES.size(); ++ idx) {
        const size_t state = size_t(PAINTED_SUPPORT_STYLES[idx].state);
        if (ebt_colors.size() <= state)
            ebt_colors.resize(state + 1, GLVolume::NEUTRAL_COLOR);
        ebt_colors[state] = painted_style_colors[idx];
    }
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        ++volume_id;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor, so we don't need it to perform it again in deserialize().
        m_triangle_selectors.back()->deserialize(mv->supported_facets.get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
        //BBS: add timestamp logic
        m_volume_timestamps.emplace_back(mv->supported_facets.timestamp());
    }

    //BBS: invalid volume_support status
    invalid_support_volumes(true);
}

PainterGizmoType GLGizmoFdmSupports::get_painter_type() const
{
    return PainterGizmoType::FDM_SUPPORTS;
}

wxString GLGizmoFdmSupports::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    // BBS remove _L()
    wxString action_name;
    if (shift_down)
        action_name = ("Unselect all");
    else {
        if (button_down == Button::Left)
            action_name = m_paint_tree_supports ? ("Enforce tree supports") : ("Enforce classic supports");
        else
            action_name = ("Block supports");
    }
    return action_name;
}

//BBS
void GLGizmoFdmSupports::init_print_instance()
{
    const PrintObject* print_object = NULL;
    PrintInstance print_instance = { 0 };
    const Print *print = m_parent.fff_print();

    if (!m_c->selection_info() || (m_print_instance.print_object))
    {
        //no selection or already got a print instance before
        return;
    }
    const ModelObject* model_object = m_c->selection_info()->model_object();
    int instance_index = m_c->selection_info()->get_active_instance();
    const ModelInstance* model_instance = model_object->instances[instance_index];

    //check the print
    if (!print)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",print invalid\n";
        return;
    }

    for (const PrintObject* object : print->objects())
    {
        if (object->model_object()->id() == model_object->id())
        {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ",found a PrintObject, id is" << model_object->id().id;
            print_object = object;
            break;
        }
    }

    //check the pring object
    if (!print_object)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",can not find a PrintObject\n";
        return;
    }

    //find the print instance
    for (const PrintInstance &instance : print_object->instances())
    {
        if (instance.model_instance->id() == model_instance->id())
        {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ",found a PrintInstance, id is" << model_instance->id().id;
            m_print_instance = instance;
            break;
        }
    }

    //check the pring object
    if (!m_print_instance.print_object)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",can not find a PrintInstance\n";
        return;
    }

    const PrintObjectConfig& config = m_print_instance.print_object->config();
    m_angle_threshold_deg = config.support_angle;
    m_is_tree_support = config.enable_support.value && is_tree(config.support_type.value);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",get support_angle "<< m_angle_threshold_deg<<", is_tree "<<m_is_tree_support;

    return;
}

void GLGizmoFdmSupports::invalid_support_volumes(bool invalid_step)
{
    std::unique_lock<std::mutex> lck(m_mutex);
    m_volume_valid = false;

    if ((invalid_step) && (m_edit_state == state_generating) && m_print_instance.print_object)
    {
        Print *print = m_print_instance.print_object->print();
        if (print) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "cancel the print";
            print->cancel();
        }
    }
    m_edit_state = state_idle;
    lck.unlock();

    return;
}

bool GLGizmoFdmSupports::need_regenerate_support_volumes()
{
    if (!m_support_volume)
        return true;

    const ModelObject* mo = m_c->selection_info()->model_object();

    if (m_object_id != m_print_instance.print_object->id().id)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",object_id changed from " << m_object_id << " to " << m_print_instance.print_object->id().id << ", need to regenerate";
        return true;
    }

    int volume_id = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        ++volume_id;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",volume_id "<<volume_id<<", record_timestamp "<< m_volume_timestamps[volume_id]
                <<", current_timestamp "<<mv->supported_facets.timestamp();
        if (m_volume_timestamps[volume_id] != mv->supported_facets.timestamp())
        {
            return true;
        }
    }

    return false;
}

void GLGizmoFdmSupports::update_support_volumes()
{
    //PrintInstance m_print_instance = get_current_print_instance();

    if ((!m_print_instance.print_object))
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",invalid param, m_volume_ready="<< m_volume_ready;
        return;
    }

    if (m_volume_valid || !need_regenerate_support_volumes())
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",no need to regenerate support volume, return directly";

        std::unique_lock<std::mutex> lck(m_mutex);
        m_volume_ready = true;
        m_volume_valid = true;
        m_edit_state = state_ready;
        lck.unlock();
        return;
    }

    //generate_support_preview in async mode
    std::unique_lock<std::mutex> lck(m_mutex);
    m_volume_ready = false;
    //destroy previous support volume
    if (m_support_volume)
    {
        delete m_support_volume;
        m_support_volume = NULL;
    }
    lck.unlock();

    if (m_thread.joinable()) {
        //join the thread in ui thread
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "try to join thread for 100 ms";
        auto ret = m_thread.try_join_for(boost::chrono::milliseconds(100));
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "join thread returns "<<ret;
    }
    m_cancel = false;
    m_thread = create_thread([this]{this->run_thread();});
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",created thread to generate support volumes";
    return;
}

void GLGizmoFdmSupports::run_thread()
{
    try {
        Print *print = m_print_instance.print_object->print();

        print->restart();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",before generate_support_preview";
        m_print_instance.print_object->generate_support_preview();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",after generate_support_preview";

        if (m_cancel)
        {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", cancelled";
            goto _finished;
        }

        std::unique_lock<std::mutex> lck(m_mutex);
        m_support_volume = new GLVolume(0.5f, 0.5f, 0.5f, 0.5f);
        //m_support_volume->is_support_part = true;
        m_support_volume->force_native_color = true;
        m_support_volume->set_render_color();
        lck.unlock();

        auto record_timestamp = [this]()
        {
            const ModelObject* mo = m_c->selection_info()->model_object();

            int volume_id = -1;
            for (const ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part())
                    continue;

                ++volume_id;
                m_volume_timestamps[volume_id] = mv->supported_facets.timestamp();
            }
        };

        if (m_cancel)
        {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", cancelled";
            goto _finished;
        }

        if (!m_print_instance.print_object->support_layers().size())
        {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",no support layer found, update status to 100%\n";
            print->set_status(100, L("Support Generated"));
            goto _finished;
        }
        for (const SupportLayer *support_layer : m_print_instance.print_object->support_layers())
        {
            for (const ExtrusionEntity *extrusion_entity : support_layer->support_fills.entities)
            {
                _3DScene::extrusionentity_to_verts(extrusion_entity, float(support_layer->print_z), m_print_instance.shift, *m_support_volume);
            }
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", finished extrusionentity_to_verts, update status to 100%";
        print->set_status(100, L("Support Generated"));

        record_timestamp();
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",exception catched, mostly cancelling from gui!";
        //wxTheApp->OnUnhandledException();
    }

_finished:
    std::unique_lock<std::mutex> lck(m_mutex);
    if (m_edit_state == state_generating)
        m_edit_state = state_ready;

    lck.unlock();
    m_parent.set_as_dirty();
    m_parent.post_event(SimpleEvent(wxEVT_PAINT));
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", finished all";
    return;
}

void GLGizmoFdmSupports::generate_support_volume()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",before finalize_geometry";
    m_support_volume->indexed_vertex_array->finalize_geometry(m_parent.is_initialized());

    std::unique_lock<std::mutex> lck(m_mutex);
    m_volume_ready = true;
    m_volume_valid = true;
    m_object_id = m_print_instance.print_object->id().id;
    lck.unlock();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",finished finalize_geometry";
}

} // namespace Slic3r::GUI
