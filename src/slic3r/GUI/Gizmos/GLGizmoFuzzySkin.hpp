#ifndef slic3r_GLGizmoFuzzySkin_hpp_
#define slic3r_GLGizmoFuzzySkin_hpp_

#include "GLGizmoPainterBase.hpp"
//BBS
#include "libslic3r/Print.hpp"
#include "libslic3r/ObjectID.hpp"

#include <boost/thread.hpp>
#include <imgui/imgui.h> // Orio : ImTextureID (icônes des lignes de calques)

namespace Slic3r::GUI {

class GLGizmoFuzzySkin : public GLGizmoPainterBase
{
public:
    GLGizmoFuzzySkin(GLCanvas3D& parent, unsigned int sprite_id);
    void data_changed(bool is_serializing) override;
    void render_painter_gizmo() const override;

    //BBS: add edit state
    enum EditState {
        state_idle = 0,
        state_generating = 1,
        state_ready
    };

    //BBS
    bool on_key_down_select_tool_type(int keyCode);

    std::string get_icon_filename(bool is_dark_mode) const override;

protected:
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    std::string on_get_name_str() override { return "FuzzySkin Painting"; }

    // BBS
    void render_triangles(const Selection& selection) const override;
    void on_set_state() override;
    void show_tooltip_information(float caption_max, float x, float y);
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return "Entering Paint-on supports"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving Paint-on supports"; }
    std::string get_action_snapshot_name() override { return "Paint-on supports editing"; }
    //Orio: the brush paints the selected layer, its color on the cursor.
    EnforcerBlockerType get_left_button_state_type() const override { return EnforcerBlockerType(m_layer_state); }
    EnforcerBlockerType get_right_button_state_type() const override;
    std::array<float, 4> get_cursor_sphere_left_button_color() const override;
    //Orio: the airbrush draws bigger circles (its dots are big, see m_airbrush).
    const float get_cursor_radius_max() const override { return m_airbrush ? AirbrushRadiusMax : CursorRadiusMax; }
    const float get_cursor_radius_step() const override { return m_airbrush ? 1.f : CursorRadiusStep; }
    static constexpr float AirbrushRadiusMax  = 25.f;
    static constexpr float AirbrushDensityMin = 1.f;
    static constexpr float AirbrushDensityMax = 40.f;
    // BBS
    wchar_t                           m_current_tool = 0;

private:
    bool on_init() override;

    //BBS: remove const
    void update_model_object() override;
    //BBS: add logic to distinguish the first_time_update and later_update
    void update_from_model_object(bool first_update) override;
    void tool_changed(wchar_t old_tool, wchar_t new_tool);
    //Orio: the layers of the fuzzy skin painting (ModelObject::fuzzy_skin_layers), as the lines of the Modifiers tool,
    // up to the end of the slider rows of the window (label_column + slider_width + field_gap + field_width).
    void render_layers(float label_column, float slider_width, float field_gap, float field_width);
    void render_layer_settings(ModelObject &mo, int state, float label_column, float slider_width, float field_gap, float field_width);
    bool icon_button(ImTextureID &light, ImTextureID &dark, const std::string &name, float size);
    bool cross_button(bool enabled, float size);
    // The layer of this state stored in the object, after the layers shown (none stored: the state 1) are.
    FuzzySkinLayer &layer_to_edit(ModelObject &mo, int state);
    void erase_layer_painting(int state);
    void add_layer(ModelObject &mo);
    void delete_layer(ModelObject &mo, int state);
    void layers_changed();
    void on_opening() override {}
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    bool m_volume_valid = false;

    //Orio: the selected layer, its state; one snapshot per gesture on a setting of a layer; frames still to draw at once
    // after a change of the lines (see render_layers()).
    int         m_layer_state    = 1;
    bool        m_layer_snapshot = false;
    int         m_layout_frames  = 0;
    // the radius of the brushes which are not chosen: the airbrush, or the others (it starts at 10 mm)
    float       m_other_radius   = 10.f;
    ImTextureID m_select_icon{nullptr};
    ImTextureID m_select_icon_dark{nullptr};
    ImTextureID m_erase_icon{nullptr};
    ImTextureID m_erase_icon_dark{nullptr};

    bool m_is_tree_support = false;
    bool m_cancel = false;
    size_t m_object_id;
    std::vector<ObjectBase::Timestamp> m_volume_timestamps;
    PrintInstance m_print_instance;
    mutable EditState m_edit_state;
    //thread
    boost::thread   m_thread;
    // Mutex and condition variable to synchronize m_thread with the UI thread.
    std::mutex      m_mutex;
    int m_generate_count;

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};
} // namespace Slic3r::GUI
#endif // slic3r_GLGizmoFuzzySkin_hpp_
