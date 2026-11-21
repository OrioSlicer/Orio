#ifndef slic3r_GUI_WaveOverhangsDialog_hpp_
#define slic3r_GUI_WaveOverhangsDialog_hpp_

#include "GUI_Utils.hpp"

#include <wx/timer.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

class Button;
class CheckBox;
class ComboBox;
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

// Orio: the window of the wave overhang settings, opened by the "Wave overhang settings" button of the process settings
// (global or of the objects: it replaces their ten lines of the Quality page). Three columns of settings in aligned rows (where and
// how, the beads, the speed and the limits), then two drawings of an overhang around a pillar: seen from above, the waves as rings
// around the pillar; in section, the beads hanging on each other under their floor. Some values are dragged on the drawings; the
// play button shows the waves laid one after the other, then the floor. Applied to the tab on OK, as if typed in its fields (to the
// selected objects in the objects mode). The drawings are a diagram, not the waves the slicer will generate.
class WaveOverhangsDialog : public DPIDialog
{
public:
    WaveOverhangsDialog(wxWindow *parent, Tab *tab);
    ~WaveOverhangsDialog() override;

    // The settings set by the window (Tab::update_wave_overhangs_button()).
    static const std::vector<std::string> &keys();
    // Why the waves cannot be printed with this config (no wall, no overhang wall detection, spiral vase), empty if they can.
    static wxString problem(const DynamicPrintConfig &config);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct Values
    {
        bool   waves{false};
        bool   bridges{false};  // wave_overhangs_instead_of_bridges
        int    pattern{2};      // WaveOverhangPattern
        int    shape{0};        // WaveOverhangShape
        double arc_radius{15.}; // wave_overhang_arc_max_radius, 0 = no limit
        int    walls{1};        // wave_overhang_outer_perimeters
        int    floor{2};
        bool   support{true};   // support_remaining_areas_after_wave_overhangs
        double spacing{0.35};
        double overlap{0.1};
        double flow{0.15};      // mm3/mm
        double min_width{0.7};
        double min_length{0.};
        double speed{2.};
        double travel{40.};
        int    max_waves{0};
        int    where() const { return !waves ? 0 : bridges ? 2 : 1; }
    };
    enum class Drag { None, Spacing, Overlap, Flow, Floor, Walls, Radius };
    Values values_of(const DynamicPrintConfig &config) const;

    void read_values();
    void apply();
    bool editable(const std::string &key) const;

    void build();
    // A panel painted by paint(gc, scale), its logical size in DIP.
    wxPanel *make_canvas(double logical_width, double logical_height, std::function<void(wxGraphicsContext &, double)> paint);
    wxWindow *make_help(const std::string &key);
    Label *make_label(const wxString &text, const std::string &key, bool title = false);
    wxSizer *make_title(const wxString &text, const std::string &key);
    wxSizer *make_field(const wxString &text, const std::string &key, const wxString &unit, std::function<void(double)> set, wxTextCtrl *&field);
    Button *make_button(const wxString &text, bool primary, int min_width = 84);
    // A red circular arrow, shown while modified() (the value is not the default one), putting it back on a click.
    wxWindow *make_reset(std::function<bool()> modified, std::function<void()> reset);
    void style_button(Button *btn, bool primary);

    // A value was changed by the user: the animation stopped.
    void touched();
    // Sync the controls (but the field being typed in) and the drawings with m_v.
    void refresh();
    void show_caption();
    wxString caption() const;
    wxString tip(const std::string &key) const;

    // Tooltips of the drawings: hidden while a handle is dragged, back 2 s after it is released (they flickered).
    bool tips_paused() const;
    void pause_tips();
    void resume_tips_later();
    void resume_tips();

    // The diagram, in mm from the edge of the pillar: where the wave k is centered, how many waves fill the overhang, how many are
    // printed (the maximum), the diameter of a bead (its section is the flow).
    double wave_at(int k) const;
    double waves_end() const;
    int    waves_needed() const;
    int    waves_printed() const;
    double bead() const;
    double recommended_flow() const;
    // Seconds added by the waves of the overhang drawn: the length of its rings at the wave speed, a travel between them.
    double extra_seconds() const;

    void paint_legend(wxGraphicsContext &gc, double scale);
    void paint_top(wxGraphicsContext &gc, double scale);
    void paint_section(wxGraphicsContext &gc, double scale);
    void paint_veil(wxGraphicsContext &gc);
    Drag handle_at(wxPanel *view, double x, double y) const;
    double walls_handle_x() const;
    double walls_handle_y() const;
    std::string region_at(wxPanel *view, double x, double y) const;
    void on_mouse(wxPanel *view, wxMouseEvent &evt);

    void start_animation();
    void animation_step();

    Tab     *m_tab;
    Values   m_v;
    // The default values: those of the system profile (or of the saved one without system profile); for objects, those they inherit.
    Values   m_default;
    double   m_layer_height{0.2};
    double   m_nozzle{0.4};
    bool     m_supports_on{false};
    wxString m_problem;
    bool     m_updating{false};

    wxPanel     *m_legend{nullptr};
    wxPanel     *m_top{nullptr};
    wxPanel     *m_section{nullptr};
    wxPanel     *m_caption{nullptr};
    Button      *m_where_btn[3]{nullptr, nullptr, nullptr};
    ::ComboBox  *m_pattern{nullptr};
    ::ComboBox  *m_shape{nullptr};
    wxTextCtrl  *m_arc_radius{nullptr};
    ::CheckBox  *m_support{nullptr};
    wxTextCtrl  *m_walls{nullptr};
    wxTextCtrl  *m_floor{nullptr};
    wxTextCtrl  *m_spacing{nullptr};
    wxTextCtrl  *m_overlap{nullptr};
    wxTextCtrl  *m_flow{nullptr};
    wxTextCtrl  *m_min_width{nullptr};
    wxTextCtrl  *m_min_length{nullptr};
    wxTextCtrl  *m_speed{nullptr};
    wxTextCtrl  *m_travel{nullptr};
    wxTextCtrl  *m_max_waves{nullptr};
    Label       *m_advice{nullptr};
    Button      *m_apply_advice{nullptr};
    Label       *m_time{nullptr};
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
    int      m_styled_where{-1};
    wxString m_shown_caption;

    Drag        m_drag{Drag::None};
    wxPanel    *m_drag_view{nullptr};
    // Where the handle was grabbed, from its center: it follows the mouse without jumping to it.
    double      m_grab{0.};
    std::string m_view_tip;
    std::chrono::steady_clock::time_point m_tips_resume;

    wxTimer                               m_timer;
    wxTimer                               m_tip_timer;
    bool                                  m_animating{false};
    std::chrono::steady_clock::time_point m_anim_start;
    // The waves laid so far (the integer part, then the part of the next one), then the floor layers; -1 when not animated.
    double                                m_anim_waves{-1.};
    double                                m_anim_floor{-1.};
    bool                                  m_anim_done{false};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_WaveOverhangsDialog_hpp_
