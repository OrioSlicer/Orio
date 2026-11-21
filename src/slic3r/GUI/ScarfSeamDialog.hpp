#ifndef slic3r_GUI_ScarfSeamDialog_hpp_
#define slic3r_GUI_ScarfSeamDialog_hpp_

#include "GUI_Utils.hpp"

#include <wx/colour.h>
#include <wx/timer.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

class Button;
class CheckBox;
class Label;
class wxGraphicsContext;
class wxPanel;
class wxSizer;
class wxTextCtrl;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

class Tab;

// The widgets are those of Widgets/ (global namespace), not the fields of Field.hpp (Slic3r::GUI::CheckBox).

// Orio: the window of the scarf seam settings, opened by the "Scarf seams" button of the process settings (global or
// of the objects: it replaces their ten lines of the Quality page) and of the filament settings (its four scarf settings).
// At the top, side by side and in aligned rows, three columns with their fields then their drawings: where (seen from above),
// the slope, the angle (a corner whose arm is turned, with examples). Under them the wall seen from the side, unrolled around
// the seam, its layers as beads of the filament (its color): the start of the layer rising as a slope (the nozzle, the length,
// the gap, the smoothness are dragged), the end of the loop going again over it while extruding less and less; under it the
// play button animates the three moments of the print. Every value explains itself in a tooltip. Applied to the tab on OK, as
// if typed in its fields (to the selected objects in the objects mode). Opened from the filament, the same window: the type,
// the start, the gap and the length are those of the filament, the other settings those of the process, common to all the
// filaments. "Associate with a filament" copies the type, the start, the gap and the length into the filaments checked in a
// small window, checks their "Use these settings" box and saves them (a copy for a Bambu filament).
class ScarfSeamDialog : public DPIDialog
{
public:
    ScarfSeamDialog(wxWindow *parent, Tab *tab);
    ~ScarfSeamDialog() override;

    // The settings of the process / object, and those of the filament, set by the window (Tab::update_scarf_seam_button()).
    static const std::vector<std::string> &process_keys();
    static const std::vector<std::string> &filament_keys();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct Values
    {
        int    type{1};       // SeamScarfType: none, contours, contours and holes
        bool   inner{true};   // seam_slope_inner_walls
        double length{10.};   // mm
        bool   entire{false}; // the slope all around the loop
        double start{0.1};    // height of the nozzle at the start, ratio of the layer height
        double gap{0.};       // mm
        int    steps{10};
        bool   smart{true};   // seam_slope_conditional
        int    angle{155};    // scarf_angle_threshold
    };
    enum class Drag { None, Length, Start, Gap, Steps, Angle };
    // The values of a process / object config; the type, the start, the gap and the length those of a filament config if given.
    Values values_of(const DynamicPrintConfig &process, const DynamicPrintConfig *filament, size_t index) const;

    // The tab of the process settings: this one, or the global one when opened from the filament.
    Tab *process_tab() const;
    void read_values();
    void apply();
    bool editable(Tab *tab, const std::string &key) const;
    // "Associate with a filament": the filaments of the project to check, then saved with these settings.
    void choose_filaments();
    void associate(const std::vector<std::string> &names);
    // The type, the start, the gap and the length into the config of a filament, its "Use these settings" box checked.
    void set_filament_scarf(DynamicPrintConfig &config) const;

    void build();
    // A panel painted by paint(gc, scale), its logical size shown zoom times larger (in DIP).
    wxPanel *make_canvas(double logical_width, double logical_height, std::function<void(wxGraphicsContext &, double)> paint, double zoom = 1.);
    wxWindow *make_help(const std::string &key);
    Label *make_label(const wxString &text, const std::string &key, bool title = false);
    wxSizer *make_title(const wxString &text, const std::string &key);
    wxSizer *make_check(const wxString &text, const std::string &key, std::function<void(bool)> set, ::CheckBox *&check);
    wxSizer *make_field(const wxString &text, const std::string &key, const wxString &unit, std::function<void(double)> set, wxTextCtrl *&field);
    Button *make_button(const wxString &text, bool primary, wxWindow *parent = nullptr);
    // A red circular arrow, shown while modified() (the value is not the default one), putting it back on a click.
    wxWindow *make_reset(std::function<bool()> modified, std::function<void()> reset);
    void style_button(Button *btn, bool primary);

    // A value was changed by the user: back to the hint, the animation stopped.
    void touched();
    // Sync the controls (but the field being typed in) and the drawings with m_v, touching only what changed (a drag calls it
    // at each move of the mouse).
    void refresh();
    // The hint or the step of the animation, painted again only when it changes.
    void show_caption();
    wxString caption() const;
    wxString tip(const std::string &key) const;

    // Tooltips of the drawings: hidden while a handle is dragged, back 2 s after it is released (they flickered).
    bool tips_paused() const;
    void pause_tips();
    void resume_tips_later();
    void resume_tips();

    // Geometry of the side view, in its logical units; positions along the wall in mm from the seam.
    double ramp_length() const;
    double slope_start() const;
    double slope_end() const;
    double ratio_at(double mm) const;
    double step_ratio_at(double mm) const;
    double px(double mm) const;
    double steps_x() const;
    Drag side_handle_at(double x, double y) const;
    std::string side_region_at(double x, double y) const;

    void paint_side(wxGraphicsContext &gc, double scale);
    void paint_where(wxGraphicsContext &gc, double scale);
    void paint_angle(wxGraphicsContext &gc, double scale);
    void on_side_mouse(wxMouseEvent &evt);
    void on_angle_mouse(wxMouseEvent &evt);

    void start_animation();
    void animation_step();

    Tab     *m_tab;
    // Opened from the filament settings: the type, the start, the gap and the length are those of the filament.
    bool     m_filament_mode{false};
    Values   m_v;
    Values   m_loaded;
    // The default values: those of the system profile (or of the saved one without system profile); for objects, those they inherit.
    Values   m_default;
    double   m_layer_height{0.2};
    double   m_nozzle{0.4};
    double   m_domain{30.}; // mm shown along the wall
    wxColour m_color;
    wxString m_filament_name;
    bool     m_updating{false};

    // Null when not shown (a part without the angle threshold).
    wxPanel     *m_where{nullptr};
    wxPanel     *m_side{nullptr};
    wxPanel     *m_angle{nullptr};
    Button      *m_type_btn[3]{nullptr, nullptr, nullptr};
    ::CheckBox  *m_inner{nullptr};
    ::CheckBox  *m_entire{nullptr};
    ::CheckBox  *m_smart{nullptr};
    wxTextCtrl  *m_length{nullptr};
    wxTextCtrl  *m_start{nullptr};
    wxTextCtrl  *m_gap{nullptr};
    wxTextCtrl  *m_steps{nullptr};
    wxTextCtrl  *m_angle_input{nullptr};
    wxPanel     *m_caption{nullptr};
    Button      *m_play{nullptr};
    struct Reset
    {
        wxPanel              *panel;
        std::function<bool()> modified;
        bool                  shown;
    };
    std::vector<Reset> m_resets;
    // The windows showing a tooltip, with its key (refreshed with the values).
    std::vector<std::pair<wxWindow *, std::string>> m_tips;
    // What is shown, to touch only what changed.
    int      m_styled_type{-1};
    int      m_where_state{-1};
    int      m_angle_state{-1};
    wxString m_shown_caption;

    Drag        m_drag{Drag::None};
    // Where the handle was grabbed, from its center: it follows the mouse without jumping to it.
    double      m_grab_dx{0.}, m_grab_dy{0.};
    std::string m_side_tip;
    int         m_phase{0}; // the step told above: 0 the hint, 1 to 4 the animation
    std::chrono::steady_clock::time_point m_tips_resume;

    wxTimer                               m_timer;
    wxTimer                               m_tip_timer;
    bool                                  m_animating{false};
    std::chrono::steady_clock::time_point m_anim_start;
    double                                m_clip0{0.}, m_clip1{0.}, m_clip2{0.}; // printed so far, logical units
    double                                m_nozzle_x{0.}, m_nozzle_y{0.};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ScarfSeamDialog_hpp_
