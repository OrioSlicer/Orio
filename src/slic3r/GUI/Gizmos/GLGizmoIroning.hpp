#ifndef slic3r_GLGizmoIroning_hpp_
#define slic3r_GLGizmoIroning_hpp_

#include "GLGizmoPainterBase.hpp"

namespace Slic3r::GUI {

//Orio: "Ironing painting", paints the flat top surfaces to iron (ModelVolume::ironing_facets). Painting an object
// sets its ironing type to Manual: only the painted top surfaces are ironed. Made from GLGizmoFuzzySkin.
class GLGizmoIroning : public GLGizmoPainterBase
{
public:
    GLGizmoIroning(GLCanvas3D& parent, unsigned int sprite_id);
    void data_changed(bool is_serializing) override;
    void render_painter_gizmo() const override;

    bool on_key_down_select_tool_type(int keyCode);

    std::string get_icon_filename(bool is_dark_mode) const override;

    // Largest tilt from horizontal of a facet that can be painted, in degrees.
    static constexpr float MaxTiltDeg = 1.f;

protected:
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    std::string on_get_name_str() override { return "Ironing Painting"; }

    void render_triangles(const Selection& selection) const override;
    void on_set_state() override;
    void show_tooltip_information(float caption_max, float x, float y);
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return "Entering Ironing painting"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving Ironing painting"; }
    std::string get_action_snapshot_name() override { return "Ironing painting editing"; }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }
    // Only the facets facing up, see TriangleSelector::select_patch().
    float paint_angle_limit() const override { return -MaxTiltDeg; }

private:
    bool on_init() override;

    void update_model_object() override;
    void update_from_model_object(bool first_update) override;
    void on_opening() override {}
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override { return PainterGizmoType::IRONING; }
    // Sets the ironing type of the object to Manual, after the snapshot of the painting action: undo restores it too.
    void set_manual_ironing(ModelObject &mo);

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};
} // namespace Slic3r::GUI
#endif // slic3r_GLGizmoIroning_hpp_
