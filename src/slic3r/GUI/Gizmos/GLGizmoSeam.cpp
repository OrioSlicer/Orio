#include "GLGizmoSeam.hpp"

#include "libslic3r/Model.hpp"

//#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/ScarfSeamDialog.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <GL/glew.h>


namespace Slic3r::GUI {



void GLGizmoSeam::on_shutdown()
{
    m_parent.toggle_model_objects_visibility(true);
}



bool GLGizmoSeam::on_init()
{
    m_shortcut_key = WXK_CONTROL_P;
    const wxString ctrl                = GUI::shortkey_ctrl_prefix();
    const wxString alt                 = GUI::shortkey_alt_prefix();
    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"] = _L("Section view");
    m_desc["reset_direction"]  = _L("Reset direction");
    m_desc["cursor_size_caption"]= ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]      = _L("Brush size");
    m_desc["cursor_type"]      = _L("Brush shape");
    m_desc["enforce_caption"]  = _L("Left mouse button");
    m_desc["enforce"]          = _L("Enforce seam");
    m_desc["block_caption"]    = _L("Right mouse button");
    m_desc["block"]            = _L("Block seam");
    m_desc["remove_caption"]   = _L("Shift + Left mouse button");
    m_desc["remove"]           = _L("Erase");
    m_desc["remove_all"]       = _L("Erase all painting");
    m_desc["circle"]           = _L("Circle");
    m_desc["sphere"]           = _L("Sphere");

    return true;
}

GLGizmoSeam::GLGizmoSeam(GLCanvas3D& parent, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{

}

void GLGizmoSeam::data_changed(bool is_serializing) {
    set_painter_gizmo_data(m_parent.get_selection());
}


std::string GLGizmoSeam::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        return _u8L("Seam painting") + ":\n" + _u8L("Please select single object.");
    } else {
        return _u8L("Seam painting");
    }
}



void GLGizmoSeam::render_painter_gizmo() const
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
bool GLGizmoSeam::on_key_down_select_tool_type(int keyCode) {
    switch (keyCode)
    {
    case 'S':
        m_current_tool = ImGui::SphereButtonIcon;
        break;
    case 'C':
        m_current_tool = ImGui::CircleButtonIcon;
        break;
    default:
        return false;
        break;
    }
    return true;
}

std::string GLGizmoSeam::get_icon_filename(bool b_dark_mode) const
{
    return b_dark_mode ? "toolbar_seam_dark.svg" : "toolbar_seam.svg";
}

void GLGizmoSeam::render_triangles(const Selection& selection) const
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
        shader->set_uniform("slope.volume_world_normal_matrix", static_cast<Matrix3f>(trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>()));
        shader->set_uniform("slope.normal_z", normal_z);
        m_triangle_selectors[mesh_id]->render(m_imgui, trafo_matrix);

        if (is_left_handed)
            glsafe(::glFrontFace(GL_CCW));
    }
}

void GLGizmoSeam::show_tooltip_information(float caption_max, float x, float y)
{
    //Orio: shown by the keyboard icon right of the name of the tool, in the title bar of its window.
    std::vector<std::pair<wxString, wxString>> shortcuts;
    auto draw_text_with_caption = [&shortcuts](const wxString &caption, const wxString &text) { shortcuts.emplace_back(caption, text); };

    for (const auto &t : std::array<std::string, 5>{"enforce", "block", "remove", "cursor_size", "clipping_of_view"}) draw_text_with_caption(m_desc.at(t + "_caption") + ": ", m_desc.at(t));
    m_imgui->tool_window_shortcuts(shortcuts);
}

void GLGizmoSeam::tool_changed(wchar_t old_tool, wchar_t new_tool)
{
    if ((old_tool == ImGui::GapFillIcon && new_tool == ImGui::GapFillIcon) ||
        (old_tool != ImGui::GapFillIcon && new_tool != ImGui::GapFillIcon))
        return;

    for (auto& selector_ptr : m_triangle_selectors) {
        TriangleSelectorPatch* tsp = dynamic_cast<TriangleSelectorPatch*>(selector_ptr.get());
        tsp->set_filter_state(new_tool == ImGui::GapFillIcon);
    }
}

void GLGizmoSeam::on_render_input_window(float x, float y, float bottom_limit)
{
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
    const float approx_height = m_imgui->scaled(12.5f);
    y = std::min(y, bottom_limit - approx_height);
    //BBS: GUI refactor: move gizmo to the right
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif
    //m_imgui->set_next_window_pos(x, y, ImGuiCond_Always);

    wchar_t old_tool = m_current_tool;
    //BBS
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());

    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size = m_imgui->get_style_scaling() * 8;
    const float empty_button_width      = m_imgui->calc_button_size("").x;

    float caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 6>{"enforce", "block", "remove", "cursor_size", "clipping_of_view"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }

    //Orio: the sliders (and the brush icons) just after the longest label.
    const float sliders_left_width = ImGui::GetStyle().WindowPadding.x + space_size +
        std::max({m_imgui->calc_text_size(m_desc.at("cursor_type")).x, m_imgui->calc_text_size(m_desc.at("cursor_size")).x,
                  clipping_label_width(m_desc.at("clipping_of_view")), m_imgui->calc_text_size(_L("Rotate horizontally")).x});
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    const float minimal_slider_width = m_imgui->scaled(4.f);
    const float sliders_width = m_imgui->scaled(7.0f);
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_left_width + sliders_width - space_size;
    float       window_width      = minimal_slider_width + sliders_left_width + slider_icon_width;
    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    float             textbox_width       = 1.5 * slider_icon_width;
    SliderInputLayout slider_input_layout = {sliders_left_width, sliders_width, drag_left_width, textbox_width};

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("cursor_type"));
    std::array<wchar_t, 2> tool_ids = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon };
    std::array<wchar_t, 2> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon};
    else
        icons = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon };
    std::array<wxString, 2> tool_tips = { _L("Circle"), _L("Sphere")};
    for (int i = 0; i < tool_ids.size(); i++) {
        std::string  str_label = std::string("##");
        std::wstring btn_name = icons[i] + boost::nowide::widen(str_label);

        //Orio: on the row of their label, right after it.
        ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + m_imgui->calc_text_size(m_desc.at("cursor_type")).x + space_size +
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

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool == ImGui::CircleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        m_tool_type = ToolType::BRUSH;
    } else if (m_current_tool == ImGui::SphereButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;
    }

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("cursor_size"));
    ImGui::SameLine(sliders_left_width);

    ImGui::PushItemWidth(sliders_width);
    m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");

    ImGui::Separator();
    render_clipping_label(m_desc.at("clipping_of_view"), m_desc.at("reset_direction"));

    auto clp_dist = float(m_c->object_clipper()->get_position());
    ImGui::SameLine(sliders_left_width);

    ImGui::PushItemWidth(sliders_width);
    bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);

    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");
    if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position(clp_dist, true); }

    ImGui::Separator();

    if (!wxGetApp().plater()->get_camera().is_looking_front()){
        m_is_front_view = false;
    }
    auto vertical_only = m_vertical_only;
    if (m_imgui->bbl_checkbox(_L("Vertical"), vertical_only)) {
        m_vertical_only = vertical_only;
    }
    auto is_front_view = m_is_front_view;
    m_imgui->bbl_checkbox(_L("View: keep horizontal"), is_front_view);
    if (m_is_front_view != is_front_view){
        m_is_front_view = is_front_view;
        if (m_is_front_view) {
            update_front_view_radian();
            change_camera_view_angle(m_front_view_radian);
        }
    }
    m_imgui->disabled_begin(!m_is_front_view);
    if (render_slider_double_input_by_format(slider_input_layout, _u8L("Rotate horizontally"), m_front_view_radian, -180.f, 180.f, 0, DoubleShowType::DEGREE)) {
       change_camera_view_angle(m_front_view_radian);
    }
    m_imgui->disabled_end();
    ImGui::Separator();
    if (m_parent.is_volumes_selected_and_sinking()) {
        m_imgui->warning_text_wrapped(_L("Warning") + ":" + _L("Painting below the build plate is not allowed.") +
                                          _L("The white outline indicates the position of the build plate at Z = 0."),
                                      window_width + m_imgui->scaled(3));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    // Orio: under the window where it is (it can be moved).
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + ImGui::GetWindowPos().y;
    show_tooltip_information(caption_max, ImGui::GetWindowPos().x, get_cur_y);

    float f_scale = m_parent.get_main_toolbar_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    //Orio: the window of the scarf seams (ScarfSeamDialog) for the painted object, centered on its row, between the
    // horizontal rotation and "Erase all painting".
    const wxString scarf_label = _L("Scarf seam settings");
    ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x, (ImGui::GetWindowWidth() - m_imgui->calc_button_size(scarf_label).x) / 2.f));
    if (m_imgui->button(scarf_label))
        // Not while the canvas is drawn: after it.
        wxGetApp().CallAfter([this]() { open_scarf_seam_dialog(); });
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Set the scarf seam of this object: where, the slope, the nozzle start, the gap."), max_tooltip_width);

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);
        ModelObject         *mo  = m_c->selection_info()->model_object();
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

    //BBS
    ImGuiWrapper::pop_toolbar_style();
}

//Orio: the object tab on the painted object (as when it is selected in the list of the objects), then the window of the
// scarf seams on it: OK writes into the object settings, one undo step.
void GLGizmoSeam::open_scarf_seam_dialog()
{
    if (!m_c || !m_c->selection_info())
        return;
    ModelObject   *mo  = m_c->selection_info()->model_object();
    TabPrintModel *tab = dynamic_cast<TabPrintModel *>(wxGetApp().get_model_tab());
    if (mo == nullptr || tab == nullptr)
        return;
    const auto &configs = tab->get_object_configs();
    if (configs.size() != 1 || configs.begin()->first != mo)
        tab->set_model_config({{mo, &mo->config}});
    ScarfSeamDialog dlg(wxGetApp().mainframe, tab);
    dlg.ShowModal();
    m_parent.set_as_dirty();
}

// BBS
void GLGizmoSeam::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == Off) {
        ModelObject* mo = m_c->selection_info()->model_object();
        if (mo) Slic3r::save_object_mesh(*mo);
    }
}

//BBS: remove const
void GLGizmoSeam::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->seam_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}


//BBS: add logic to distinguish the first_time_update and later_update
void GLGizmoSeam::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject* mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();

    int volume_id = -1;
    std::vector<std::array<float, 4>> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        ++volume_id;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();

        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor, so we don't need it to perform it again in deserialize().
        m_triangle_selectors.back()->deserialize(mv->seam_facets.get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
    }
}


PainterGizmoType GLGizmoSeam::get_painter_type() const
{
    return PainterGizmoType::SEAM;
}

wxString GLGizmoSeam::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    wxString action_name;
    if (shift_down)
        action_name = _L("Remove selection");
    else {
        if (button_down == Button::Left)
            action_name = _L("Enforce seam");
        else
            action_name = _L("Block seam");
    }
    return action_name;
}

} // namespace Slic3r::GUI
