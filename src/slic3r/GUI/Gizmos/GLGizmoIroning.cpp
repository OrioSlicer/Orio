#include "GLGizmoIroning.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <GL/glew.h>

namespace Slic3r::GUI {

GLGizmoIroning::GLGizmoIroning(GLCanvas3D& parent, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, sprite_id)
{
    m_current_tool = ImGui::CircleButtonIcon;
    m_tool_type    = ToolType::BRUSH;
    m_cursor_type  = TriangleSelector::CursorType::CIRCLE;
}

void GLGizmoIroning::data_changed(bool is_serializing)
{
    set_painter_gizmo_data(m_parent.get_selection());
}

void GLGizmoIroning::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

std::string GLGizmoIroning::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off)
        return _u8L("Ironing painting") + ":\n" + _u8L("Please select single object.");
    return _u8L("Ironing painting");
}

bool GLGizmoIroning::on_init()
{
    m_shortcut_key = WXK_CONTROL_J;
    const wxString ctrl                = GUI::shortkey_ctrl_prefix();
    const wxString alt                 = GUI::shortkey_alt_prefix();
    m_desc["flat_only"]                = _L("Only the flat surfaces facing up");
    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]         = _L("Section view");
    m_desc["reset_direction"]          = _L("Reset direction");
    m_desc["cursor_size_caption"]      = ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]              = _L("Pen size");
    m_desc["add_ironing_caption"]      = _L("Left mouse button");
    m_desc["add_ironing"]              = _L("Add ironing");
    m_desc["remove_caption"]           = _L("Right mouse button");
    m_desc["remove"]                   = _L("Remove ironing");
    m_desc["remove_all"]               = _L("Erase all painting");
    m_desc["tool_type"]                = _L("Tool type");
    return true;
}

void GLGizmoIroning::render_painter_gizmo() const
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

bool GLGizmoIroning::on_key_down_select_tool_type(int keyCode)
{
    switch (keyCode) {
    case 'F': m_current_tool = ImGui::FillButtonIcon; break;
    case 'S': m_current_tool = ImGui::SphereButtonIcon; break;
    case 'C': m_current_tool = ImGui::CircleButtonIcon; break;
    default: return false;
    }
    return true;
}

std::string GLGizmoIroning::get_icon_filename(bool is_dark_mode) const
{
    return is_dark_mode ? "toolbar_ironing_dark.svg" : "toolbar_ironing.svg";
}

// Same as GLGizmoFuzzySkin (TriangleSelectorPatch needs the mm_gouraud shader).
void GLGizmoIroning::render_triangles(const Selection& selection) const
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

void GLGizmoIroning::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == Off) {
        ModelObject* mo = m_c->selection_info()->model_object();
        if (mo) Slic3r::save_object_mesh(*mo);
    }
}

void GLGizmoIroning::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c || !m_c->selection_info() || !m_c->selection_info()->model_object())
        return;
    m_imgui_start_pos[0] = x;
    m_imgui_start_pos[1] = y;

    const float approx_height = m_imgui->scaled(18.f);
    y = std::min(y, bottom_limit - approx_height);

    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size         = m_imgui->get_style_scaling() * 8;
    const float empty_button_width = m_imgui->calc_button_size("").x;

    float caption_max = 0.f;
    for (const auto &t : std::array<std::string, 4>{"add_ironing", "remove", "cursor_size", "clipping_of_view"})
        caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
    caption_max += m_imgui->scaled(1.f);

    // The sliders (and the tool icons) just after the longest label shown for the current tool.
    const float tool_label_width   = m_current_tool == ImGui::FillButtonIcon ? 0.f : m_imgui->calc_text_size(m_desc.at("cursor_size")).x;
    const float sliders_left_width = ImGui::GetStyle().WindowPadding.x + space_size +
        std::max({m_imgui->calc_text_size(m_desc.at("tool_type")).x, tool_label_width, clipping_label_width(m_desc.at("clipping_of_view"))});
    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;
    const float sliders_width     = m_imgui->scaled(7.0f);
    const float drag_left_width   = ImGui::GetStyle().WindowPadding.x + sliders_left_width + sliders_width - space_size;

    // What can be painted, for the beginners.
    m_imgui->text(m_desc.at("flat_only"));

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("tool_type"));
    std::array<wchar_t, 3> tool_ids = {ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::FillButtonIcon};
    std::array<wchar_t, 3> icons;
    if (m_is_dark_mode)
        icons = {ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::FillButtonDarkIcon};
    else
        icons = tool_ids;

    std::array<wxString, 3> tool_tips = {_L("Circle"), _L("Sphere"), _L("Fill")};
    for (int i = 0; i < int(tool_ids.size()); i++) {
        std::wstring btn_name = icons[i] + boost::nowide::widen(std::string("##"));

        // On the row of their label, right after it.
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
        if (m_current_tool == tool_ids[i]) {
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

        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        ImGui::Separator();
        m_cursor_type = m_current_tool == ImGui::CircleButtonIcon ? TriangleSelector::CursorType::CIRCLE : TriangleSelector::CursorType::SPHERE;
        m_tool_type   = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    } else {
        // Fill: a click paints the whole flat surface (its neighbours not facing up stop the filling).
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::SMART_FILL;
    }

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

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    // Under the window where it is (it can be moved).
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

    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoIroning::show_tooltip_information(float caption_max, float x, float y)
{
    // Shown by the keyboard icon right of the name of the tool, in the title bar of its window.
    std::vector<std::pair<wxString, wxString>> shortcuts;
    std::vector<std::string> tip_items = {"add_ironing", "remove"};
    if (m_tool_type == ToolType::BRUSH)
        tip_items.emplace_back("cursor_size");
    tip_items.emplace_back("clipping_of_view");
    for (const auto &t : tip_items)
        shortcuts.emplace_back(m_desc.at(t + "_caption") + ": ", m_desc.at(t));

    m_imgui->tool_window_shortcuts(shortcuts);
}

void GLGizmoIroning::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->ironing_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        if (mo->is_ironing_painted())
            this->set_manual_ironing(*mo);

        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());

        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoIroning::set_manual_ironing(ModelObject &mo)
{
    const DynamicPrintConfig &obj_cfg = mo.config.get();
    if (obj_cfg.option("ironing_type") && obj_cfg.opt_enum<IroningType>("ironing_type") == IroningType::Manual)
        return;
    mo.config.set_key_value("ironing_type", new ConfigOptionEnum<IroningType>(IroningType::Manual));

    // Settings of the object in the object list, and in the settings tab of the object if it shows them.
    ObjectVolumeID object_volume_id;
    object_volume_id.object = &mo;
    wxGetApp().obj_list()->object_config_options_changed(object_volume_id);
    if (TabPrintModel *tab = dynamic_cast<TabPrintModel*>(wxGetApp().get_model_tab()))
        tab->update_model_config();

    wxGetApp().plater()->get_notification_manager()->push_notification(NotificationType::CustomNotification,
        NotificationManager::NotificationLevel::RegularNotificationLevel,
        // GUI::format: the member GLGizmoBase::format(float, unsigned) hides it.
        GUI::format(_L("Ironing painted on \"%1%\": its ironing type is Manual (only the painted areas are ironed)."), mo.name));
}

void GLGizmoIroning::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject* mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();

    std::vector<std::array<float, 4>> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor, so we don't need it to perform it again in deserialize().
        m_triangle_selectors.back()->deserialize(mv->ironing_facets.get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
    }
}

wxString GLGizmoIroning::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    // BBS remove _L()
    if (shift_down)
        return "Unselect all";
    return button_down == Button::Left ? "Add ironing" : "Remove ironing";
}

} // namespace Slic3r::GUI
