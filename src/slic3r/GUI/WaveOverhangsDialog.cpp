#include "WaveOverhangsDialog.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Slic3r {
namespace GUI {

namespace {

// The two drawings, in logical units (DIP).
constexpr double VIEW_W   = 316.;
constexpr double VIEW_H   = 266.;
constexpr double LEGEND_Y = 252.;

// The overhang drawn: a square pillar (its half width, its corner radius), the slab around it overhanging by OVERHANG, in mm.
constexpr double PILLAR   = 3.;
constexpr double PILLAR_R = 1.;
constexpr double OVERHANG = 4.;

// Seen from above: px per mm, the center of the pillar.
constexpr double TOP_MM = 15.;
constexpr double CX     = 158.;
constexpr double CY     = 122.;

// In section: px per mm (the same both ways, the beads are round), the edge of the pillar, its cut on the left, the middle of the
// layer of the waves, the bottom of the drawing.
constexpr double SEC_MM   = 56.;
constexpr double SX0      = 64.;
constexpr double SX_LEFT  = 14.;
constexpr double Y_WAVE   = 150.;
constexpr double Y_GROUND = 236.;

constexpr double HANDLE_R   = 6.5;
constexpr double RESET_SIZE = 16.;
constexpr double TOP_LEGEND_W = 700.;
constexpr double TOP_LEGEND_H = 24.;
constexpr double CAPTION_W  = 560.;
constexpr double CAPTION_H  = 36.;
constexpr int    MAX_DRAWN  = 120; // waves drawn at most (a tiny spacing)
constexpr double WAVES_SECONDS = 1.5;
constexpr double FLOOR_SECONDS = 1.2;
constexpr int    ID_ANIMATION = wxID_HIGHEST + 1;
constexpr int    ID_TIPS      = wxID_HIGHEST + 2;

// The colors of the Preview (GCodeRenderer/BaseRenderer.cpp, Extrusion_Role_Colors): overhang wall for the waves, outer wall, solid
// infill for the floor, bridge for what the waves leave, support.
const wxColour WAVE_COLOUR(31, 31, 255);
const wxColour WALL_COLOUR(255, 125, 56);
const wxColour FLOOR_COLOUR(150, 84, 204);
const wxColour BRIDGE_COLOUR(77, 128, 186);
const wxColour SUPPORT_COLOUR(0, 255, 0);

// Orio : accent du theme (bleu en clair, ORION en sombre).
wxColour accent() { return wxGetApp().dark_mode() ? wxColour(0, 194, 255) : wxColour(8, 119, 232); }

using Points = std::vector<wxPoint2DDouble>;

wxColour with_alpha(const wxColour &c, unsigned char alpha) { return wxColour(c.Red(), c.Green(), c.Blue(), alpha); }

wxColour darker(const wxColour &c, double t)
{
    auto d = [t](unsigned char x) { return (unsigned char) std::lround(x * (1. - t)); };
    return wxColour(d(c.Red()), d(c.Green()), d(c.Blue()));
}

wxColour text_colour(bool secondary = false)
{
    if (wxGetApp().dark_mode())
        return secondary ? wxColour(176, 176, 176) : wxColour(239, 239, 240);
    return secondary ? wxColour(107, 107, 107) : wxColour(38, 46, 48);
}

wxString num(double value) { return double_to_string(value, 2); }

bool parse_number(wxString text, double &value)
{
    text.Replace(",", ".");
    text.Trim().Trim(false);
    return text.ToCDouble(&value);
}

void fill_polygon(wxGraphicsContext &gc, const Points &pts, const wxColour &fill, const wxColour &outline)
{
    if (pts.size() < 3)
        return;
    wxGraphicsPath path = gc.CreatePath();
    path.MoveToPoint(pts.front());
    for (size_t i = 1; i < pts.size(); ++i)
        path.AddLineToPoint(pts[i]);
    path.CloseSubpath();
    gc.SetBrush(wxBrush(fill));
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(outline, 1.)));
    gc.DrawPath(path);
}

// Text centered on y, left aligned (align < 0), centered (0) or right aligned (> 0) on x, in pixels. Returns its width.
double draw_text(wxGraphicsContext &gc, const wxString &text, double x, double y, const wxColour &colour, const wxFont &font, int align)
{
    gc.SetFont(font, colour);
    double w = 0., h = 0.;
    gc.GetTextExtent(text, &w, &h);
    gc.DrawText(text, align < 0 ? x : align == 0 ? x - w / 2. : x - w, y - h / 2.);
    return w;
}

// A round handle, with arrows telling it moves left and right, or up and down.
void draw_handle(wxGraphicsContext &gc, double x, double y, const wxColour &bg, bool vertical)
{
    const double r = HANDLE_R, a = r * 0.72;
    gc.SetBrush(wxBrush(bg));
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(accent(), 2.)));
    gc.DrawEllipse(x - r, y - r, 2. * r, 2. * r);
    if (vertical) {
        fill_polygon(gc, {{x, y - a}, {x - 3.5, y - a + 4.}, {x + 3.5, y - a + 4.}}, accent(), accent());
        fill_polygon(gc, {{x, y + a}, {x - 3.5, y + a - 4.}, {x + 3.5, y + a - 4.}}, accent(), accent());
    } else {
        fill_polygon(gc, {{x - a, y}, {x - a + 4., y - 3.5}, {x - a + 4., y + 3.5}}, accent(), accent());
        fill_polygon(gc, {{x + a, y}, {x + a - 4., y - 3.5}, {x + a - 4., y + 3.5}}, accent(), accent());
    }
}

// A rounded square around the center of the pillar, offset by `offset` mm from its edge, in px seen from above. It starts on the
// right side and goes around clockwise (on the screen), closed.
Points ring_points(double offset)
{
    const double half = (PILLAR + offset) * TOP_MM, r = std::max(0.05, PILLAR_R + offset) * TOP_MM, c = std::max(0., half - r);
    const double signs[4][2] = {{1., 1.}, {-1., 1.}, {-1., -1.}, {1., -1.}};
    Points       pts;
    for (int q = 0; q < 4; ++q)
        for (int i = 0; i <= 8; ++i) {
            const double a = (q + i / 8.) * PI / 2.;
            pts.emplace_back(CX + signs[q][0] * c + r * std::cos(a), CY + signs[q][1] * c + r * std::sin(a));
        }
    pts.push_back(pts.front());
    return pts;
}

double ring_length(double offset)
{
    const double half = PILLAR + offset, r = std::max(0.05, PILLAR_R + offset);
    return 8. * std::max(0., half - r) + 2. * PI * r;
}

// Seen from above, in arcs: the arc of radius r (mm) of the fan grown from the middle of the side `side` of the pillar (0 right, then
// clockwise on the screen), a new fan from the top of the previous one every max_radius mm (0 = a single fan). Kept on its side of
// the diagonals of the pillar.
Points arc_points(int side, double r, double max_radius)
{
    const double nx[4] = {1., 0., -1., 0.}, ny[4] = {0., 1., 0., -1.};
    r                  = std::max(0., r); // the first wave goes onto the pillar with a large overlap
    const double start = max_radius > 0. ? std::floor(r / max_radius) * max_radius : 0., sub = r - start;
    Points       pts;
    for (int i = 0; i <= 24; ++i) {
        const double a = PI * i / 24., out = PILLAR + start + sub * std::sin(a), along = sub * std::cos(a);
        if (std::abs(along) <= out)
            pts.emplace_back(CX + (nx[side] * out - ny[side] * along) * TOP_MM, CY + (ny[side] * out + nx[side] * along) * TOP_MM);
    }
    return pts;
}

// The handle of the maximum arc radius, seen from above: true to scale up to the edge of the slab, then compressed (log) up to
// R_HANDLE_MAX at R_HANDLE_END mm from the pillar, near the side of the drawing; 0 = no limit, at the end too.
constexpr double R_HANDLE_END = 6.8;
constexpr double R_HANDLE_MAX = 500.;
double radius_to_handle(double radius)
{
    if (radius <= 0.)
        return R_HANDLE_END;
    if (radius <= OVERHANG)
        return radius;
    return std::min(R_HANDLE_END, OVERHANG + std::log(radius / OVERHANG) / std::log(R_HANDLE_MAX / OVERHANG) * (R_HANDLE_END - OVERHANG));
}
double handle_to_radius(double mm)
{
    const double r = mm <= OVERHANG ? std::max(0.5, mm)
                                    : OVERHANG * std::pow(R_HANDLE_MAX / OVERHANG, std::min(1., (mm - OVERHANG) / (R_HANDLE_END - OVERHANG)));
    return r < 10. ? std::round(r * 10.) / 10. : std::round(r);
}

// The first `fraction` of a polyline, reversed if asked.
void stroke_part(wxGraphicsContext &gc, Points pts, double fraction, bool reversed)
{
    if (reversed)
        std::reverse(pts.begin(), pts.end());
    const double end = std::clamp(fraction, 0., 1.) * (pts.size() - 1);
    if (end <= 0.)
        return;
    wxGraphicsPath path = gc.CreatePath();
    path.MoveToPoint(pts.front());
    size_t i = 1;
    for (; i <= size_t(end); ++i)
        path.AddLineToPoint(pts[i]);
    if (i < pts.size() && end > i - 1) {
        const double t = end - (i - 1);
        path.AddLineToPoint(pts[i - 1].m_x + (pts[i].m_x - pts[i - 1].m_x) * t, pts[i - 1].m_y + (pts[i].m_y - pts[i - 1].m_y) * t);
    }
    gc.StrokePath(path);
}

} // namespace

WaveOverhangsDialog::WaveOverhangsDialog(wxWindow *parent, Tab *tab)
    : DPIDialog(parent, wxID_ANY, _L("Wave overhangs"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_tab(tab)
{
    SetBackgroundColour(*wxWHITE);
    read_values();
    build();
    wxGetApp().UpdateDlgDarkUI(this);
    // The play button: its round icon on the background of the window.
    m_play->SetBackgroundColor(GetBackgroundColour());
    m_play->SetBorderColor(GetBackgroundColour());
    refresh();
    m_timer.SetOwner(this, ID_ANIMATION);
    m_tip_timer.SetOwner(this, ID_TIPS);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { animation_step(); }, ID_ANIMATION);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { resume_tips(); }, ID_TIPS);
    CentreOnParent();
}

WaveOverhangsDialog::~WaveOverhangsDialog()
{
    m_timer.Stop();
    m_tip_timer.Stop();
}

const std::vector<std::string> &WaveOverhangsDialog::keys()
{
    static const std::vector<std::string> keys = {"wave_overhangs", "wave_overhangs_instead_of_bridges", "wave_overhang_pattern",
                                                  "wave_overhang_outer_perimeters", "wave_overhang_floor_layers",
                                                  "support_remaining_areas_after_wave_overhangs", "wave_overhang_line_spacing",
                                                  "wave_overhang_perimeter_overlap", "wave_overhang_flow_mm3_per_mm",
                                                  "wave_overhang_minimum_width", "wave_overhang_min_length", "wave_overhang_print_speed",
                                                  "wave_overhang_travel_speed", "wave_overhang_max_iterations",
                                                  "wave_overhang_shape", "wave_overhang_arc_max_radius"};
    return keys;
}

// As ConfigManipulation::toggle_print_fff_options() greys the waves.
wxString WaveOverhangsDialog::problem(const DynamicPrintConfig &config)
{
    if (config.has("wall_loops") && config.opt_int("wall_loops") <= 0)
        return _L("the part has no wall (Strength > Walls)");
    if (config.has("detect_overhang_wall") && !config.opt_bool("detect_overhang_wall"))
        return _L("the overhang walls are not detected (Quality > Overhangs)");
    if (config.has("spiral_mode") && config.opt_bool("spiral_mode"))
        return _L("the spiral vase mode is on");
    return {};
}

WaveOverhangsDialog::Values WaveOverhangsDialog::values_of(const DynamicPrintConfig &config) const
{
    Values v;
    auto   flag = [&config](const char *key, bool &value) { if (config.has(key)) value = config.opt_bool(key); };
    auto   integer = [&config](const char *key, int &value) { if (config.has(key)) value = config.opt_int(key); };
    auto   real = [&config](const char *key, double &value) { if (config.has(key)) value = config.opt_float(key); };
    flag("wave_overhangs", v.waves);
    flag("wave_overhangs_instead_of_bridges", v.bridges);
    if (config.has("wave_overhang_pattern"))
        v.pattern = std::clamp(int(config.opt_enum<WaveOverhangPattern>("wave_overhang_pattern")), 0, 2);
    integer("wave_overhang_outer_perimeters", v.walls);
    integer("wave_overhang_floor_layers", v.floor);
    flag("support_remaining_areas_after_wave_overhangs", v.support);
    real("wave_overhang_line_spacing", v.spacing);
    real("wave_overhang_perimeter_overlap", v.overlap);
    real("wave_overhang_flow_mm3_per_mm", v.flow);
    real("wave_overhang_minimum_width", v.min_width);
    real("wave_overhang_min_length", v.min_length);
    real("wave_overhang_print_speed", v.speed);
    real("wave_overhang_travel_speed", v.travel);
    integer("wave_overhang_max_iterations", v.max_waves);
    if (config.has("wave_overhang_shape"))
        v.shape = std::clamp(int(config.opt_enum<WaveOverhangShape>("wave_overhang_shape")), 0, 1);
    real("wave_overhang_arc_max_radius", v.arc_radius);
    return v;
}

void WaveOverhangsDialog::read_values()
{
    PresetBundle             &bundle    = *wxGetApp().preset_bundle;
    auto                     *model_tab = dynamic_cast<TabPrintModel *>(m_tab);
    const DynamicPrintConfig &config    = *m_tab->get_config();
    if (config.has("layer_height") && config.opt_float("layer_height") > 0.)
        m_layer_height = config.opt_float("layer_height");
    const DynamicPrintConfig full = bundle.full_config();
    if (auto *nozzle = full.option<ConfigOptionFloatsNullable>("nozzle_diameter"); nozzle != nullptr && !nozzle->values.empty() && nozzle->get_at(0) > 0.)
        m_nozzle = nozzle->get_at(0);
    m_supports_on = config.has("enable_support") && config.opt_bool("enable_support");
    m_problem     = problem(config);
    m_v           = values_of(config);
    // The defaults: the system profile (or the saved one without system profile); for objects, what they inherit.
    if (model_tab != nullptr)
        m_default = values_of(model_tab->inherited_config());
    else {
        const Preset *parent = bundle.prints.get_selected_preset_parent();
        m_default            = values_of(parent != nullptr ? parent->config : bundle.prints.get_selected_preset().config);
    }
}

bool WaveOverhangsDialog::editable(const std::string &key) const
{
    if (!m_tab->get_config()->has(key))
        return false;
    auto *model_tab = dynamic_cast<TabPrintModel *>(m_tab);
    return model_tab == nullptr || model_tab->has_key(key);
}

void WaveOverhangsDialog::apply()
{
    std::vector<std::pair<std::string, std::unique_ptr<ConfigOption>>> changes;
    auto add = [&changes](const char *key, ConfigOption *opt) { changes.emplace_back(key, std::unique_ptr<ConfigOption>(opt)); };
    add("wave_overhangs", new ConfigOptionBool(m_v.waves));
    add("wave_overhangs_instead_of_bridges", new ConfigOptionBool(m_v.bridges));
    add("wave_overhang_pattern", new ConfigOptionEnum<WaveOverhangPattern>(WaveOverhangPattern(m_v.pattern)));
    add("wave_overhang_outer_perimeters", new ConfigOptionInt(m_v.walls));
    add("wave_overhang_floor_layers", new ConfigOptionInt(m_v.floor));
    add("support_remaining_areas_after_wave_overhangs", new ConfigOptionBool(m_v.support));
    add("wave_overhang_line_spacing", new ConfigOptionFloat(m_v.spacing));
    add("wave_overhang_perimeter_overlap", new ConfigOptionFloat(m_v.overlap));
    add("wave_overhang_flow_mm3_per_mm", new ConfigOptionFloat(m_v.flow));
    add("wave_overhang_minimum_width", new ConfigOptionFloat(m_v.min_width));
    add("wave_overhang_min_length", new ConfigOptionFloat(m_v.min_length));
    add("wave_overhang_print_speed", new ConfigOptionFloat(m_v.speed));
    add("wave_overhang_travel_speed", new ConfigOptionFloat(m_v.travel));
    add("wave_overhang_max_iterations", new ConfigOptionInt(m_v.max_waves));
    add("wave_overhang_shape", new ConfigOptionEnum<WaveOverhangShape>(WaveOverhangShape(m_v.shape)));
    add("wave_overhang_arc_max_radius", new ConfigOptionFloat(m_v.arc_radius));
    DynamicPrintConfig &config = *m_tab->get_config();
    // The objects (parts, height ranges): the changed values written into them at once, those back to the inherited value removed
    // from them (TabPrintModel::set_object_options()), a single undo step.
    if (auto *model_tab = dynamic_cast<TabPrintModel *>(m_tab); model_tab != nullptr) {
        DynamicPrintConfig values;
        for (auto &[key, value] : changes)
            if (editable(key) && !(*config.option(key) == *value))
                values.set_key_value(key, value.release());
        if (!values.empty()) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Wave overhangs");
            model_tab->set_object_options(values);
        }
        return;
    }
    // As if each value was typed in its field.
    for (auto &[key, value] : changes) {
        if (!editable(key) || *config.option(key) == *value)
            continue;
        config.set_key_value(key, value.release());
        m_tab->update_dirty();
        m_tab->on_value_change(key, boost::any());
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The window

Button *WaveOverhangsDialog::make_button(const wxString &text, bool primary, int min_width)
{
    auto *btn = new Button(this, text);
    style_button(btn, primary);
    btn->SetFont(Label::Body_12);
    btn->SetMinSize(wxSize(FromDIP(min_width), FromDIP(26)));
    btn->SetCornerRadius(FromDIP(12));
    return btn;
}

void WaveOverhangsDialog::style_button(Button *btn, bool primary)
{
    const bool dark = wxGetApp().dark_mode();
    if (primary) {
        btn->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour(5, 96, 194), StateColor::Pressed),
                                           std::pair<wxColour, int>(wxColour(43, 140, 242), StateColor::Hovered),
                                           std::pair<wxColour, int>(wxColour(8, 116, 224), StateColor::Normal)));
        btn->SetBorderColor(wxColour(8, 116, 224));
        btn->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Normal)));
    } else {
        const wxColour normal = dark ? wxColour(32, 32, 36) : *wxWHITE;
        btn->SetBackgroundColor(StateColor(std::pair<wxColour, int>(dark ? wxColour(80, 80, 85) : wxColour(206, 206, 206), StateColor::Pressed),
                                           std::pair<wxColour, int>(dark ? wxColour(62, 62, 67) : wxColour(238, 238, 238), StateColor::Hovered),
                                           std::pair<wxColour, int>(normal, StateColor::Normal)));
        btn->SetBorderColor(dark ? wxColour(120, 120, 125) : wxColour(38, 46, 48));
        btn->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour(172, 172, 172), StateColor::Disabled),
                                     std::pair<wxColour, int>(text_colour(), StateColor::Normal)));
    }
    btn->Refresh();
}

wxPanel *WaveOverhangsDialog::make_canvas(double logical_width, double logical_height, std::function<void(wxGraphicsContext &, double)> paint)
{
    auto *panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(int(logical_width)), FromDIP(int(logical_height))));
    panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    panel->Bind(wxEVT_PAINT, [this, panel, logical_width, paint](wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(panel);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            paint(*gc, panel->GetClientSize().x / logical_width);
        }
    });
    return panel;
}

wxWindow *WaveOverhangsDialog::make_reset(std::function<bool()> modified, std::function<void()> reset)
{
    wxPanel *panel = make_canvas(RESET_SIZE, RESET_SIZE, [modified](wxGraphicsContext &gc, double scale) {
        if (!modified())
            return;
        // A circle open at the top, its arrow head at the right end, in the usual color of a modified setting.
        const wxColour colour = wxGetApp().get_label_clr_modified();
        const double   cx = 8., cy = 8., r = 5.;
        const double   a0 = -PI / 2. + 0.55, a1 = a0 + 2. * PI - 1.1;
        gc.PushState();
        gc.Scale(scale, scale);
        wxGraphicsPath arc = gc.CreatePath();
        arc.AddArc(cx, cy, r, a0, a1, true);
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour, 1.6).Cap(wxCAP_ROUND)));
        gc.StrokePath(arc);
        const double px0 = cx + r * std::cos(a0), py0 = cy + r * std::sin(a0);
        const double dx = std::sin(a0), dy = -std::cos(a0), nx = std::cos(a0), ny = std::sin(a0);
        fill_polygon(gc, {{px0 + dx * 3.2, py0 + dy * 3.2}, {px0 + nx * 2.6, py0 + ny * 2.6}, {px0 - nx * 2.6, py0 - ny * 2.6}}, colour, colour);
        gc.PopState();
    });
    panel->Bind(wxEVT_LEFT_DOWN, [this, modified, reset](wxMouseEvent &) {
        if (!modified())
            return;
        reset();
        touched();
        refresh();
    });
    m_resets.push_back({panel, std::move(modified), false});
    return panel;
}

wxWindow *WaveOverhangsDialog::make_help(const std::string &key)
{
    auto *help = new ScalableButton(this, wxID_ANY, "icon_qusetion", wxEmptyString, wxDefaultSize, wxDefaultPosition,
                                    wxBU_EXACTFIT | wxNO_BORDER, true);
    m_tips.emplace_back(help, key);
    return help;
}

Label *WaveOverhangsDialog::make_label(const wxString &text, const std::string &key, bool title)
{
    auto *label = new Label(this, title ? Label::Head_13 : Label::Body_13, text);
    if (!key.empty())
        m_tips.emplace_back(label, key);
    return label;
}

wxSizer *WaveOverhangsDialog::make_title(const wxString &text, const std::string &key)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(make_label(text, key, true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    row->Add(make_help(key), 0, wxALIGN_CENTER_VERTICAL);
    return row;
}

// A row of a column: the name on the left; on the right its help, a short field and its unit (all the fields of a column in
// line, their helps too).
wxSizer *WaveOverhangsDialog::make_field(const wxString &text, const std::string &key, const wxString &unit, std::function<void(double)> set, wxTextCtrl *&field)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(make_label(text, key), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->AddStretchSpacer();
    row->Add(make_help(key), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    auto *ctrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(52), -1), wxTE_RIGHT | wxTE_PROCESS_ENTER);
    ctrl->SetFont(Label::Body_13);
    field = ctrl;
    ctrl->Bind(wxEVT_TEXT, [this, set](wxCommandEvent &evt) {
        double value = 0.;
        if (!m_updating && parse_number(evt.GetString(), value)) {
            set(value);
            touched();
            refresh();
        }
        evt.Skip();
    });
    // The value as kept (rounded, within its range) once the field is left.
    ctrl->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &evt) {
        CallAfter([this]() { refresh(); });
        evt.Skip();
    });
    ctrl->Bind(wxEVT_TEXT_ENTER, [ctrl](wxCommandEvent &) { ctrl->Navigate(); });
    row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    // Wide enough for "mm3/mm": the fields of the columns stay in line.
    auto *unit_label = new Label(this, Label::Body_13, unit);
    unit_label->SetMinSize(wxSize(FromDIP(50), -1));
    row->Add(unit_label, 0, wxALIGN_CENTER_VERTICAL);
    return row;
}

void WaveOverhangsDialog::build()
{
    const int pad  = FromDIP(16);
    auto     *main = new wxBoxSizer(wxVERTICAL);

    // At the top, the Beta pill and what the waves do (or why they cannot be printed).
    m_legend = make_canvas(TOP_LEGEND_W, TOP_LEGEND_H, [this](wxGraphicsContext &gc, double scale) { paint_legend(gc, scale); });
    main->Add(m_legend, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    // The three columns side by side, aligned at the top, thin lines between them; rows of the same height from a column to the other.
    const int row_h   = FromDIP(30);
    auto     *columns = new wxBoxSizer(wxHORIZONTAL);
    auto      column  = [this, columns](bool first) {
        if (!first) {
            auto *line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(1), -1));
            line->SetBackgroundStyle(wxBG_STYLE_PAINT);
            line->Bind(wxEVT_PAINT, [line](wxPaintEvent &) {
                wxPaintDC dc(line);
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(wxGetApp().dark_mode() ? wxColour(235, 235, 235) : wxColour(205, 205, 205)));
                dc.DrawRectangle(line->GetClientRect());
            });
            columns->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(14));
        }
        auto *col = new wxBoxSizer(wxVERTICAL);
        columns->Add(col, 0, wxALIGN_TOP);
        return col;
    };
    auto row = [this, row_h](wxBoxSizer *col, wxSizer *content) {
        content->SetMinSize(wxSize(-1, row_h));
        col->Add(content, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    };
    // The arrow at the end of a row (after a stretch for the titles and the boxes, so that the arrows of a column are in line).
    auto with_reset = [this](wxSizer *line, bool stretch, std::function<bool()> modified, std::function<void()> reset) {
        if (stretch)
            line->AddStretchSpacer();
        line->Add(make_reset(std::move(modified), std::move(reset)), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
        return line;
    };
    auto differs = [](double a, double b) { return std::abs(a - b) > 1e-4; };
    // A field of a value of the config: greyed if this tab has not it (a part or a modifier).
    auto field = [this, row, with_reset](wxBoxSizer *col, const wxString &text, const std::string &key, const wxString &unit,
                                         std::function<void(double)> set, wxTextCtrl *&ctrl, std::function<bool()> modified,
                                         std::function<void()> reset) {
        row(col, with_reset(make_field(text, key, unit, std::move(set), ctrl), false, std::move(modified), std::move(reset)));
        ctrl->Enable(editable(key));
    };
    auto count = [](double v, int lo, int hi) { return std::clamp(int(std::lround(v)), lo, hi); };

    // Where and how: the three choices, the pattern, the walls kept, the floor, the supports.
    wxBoxSizer *how = column(true);
    row(how, with_reset(make_title(_L("Where and how"), "where"), true, [this]() { return m_v.where() != m_default.where(); },
                        [this]() { m_v.waves = m_default.waves; m_v.bridges = m_default.bridges; }));
    const wxString wheres[3] = {_L("Nowhere"), _L("Overhangs only"), _L("Overhangs and bridges")};
    for (int i = 0; i < 3; ++i) {
        m_where_btn[i] = make_button(wheres[i], false);
        m_where_btn[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) {
            if (m_v.where() == i)
                return;
            // Nowhere keeps the bridges choice for later.
            m_v.waves = i > 0;
            if (i > 0)
                m_v.bridges = i == 2;
            touched();
            refresh();
        });
        m_where_btn[i]->Enable(editable("wave_overhangs") && (i < 2 || editable("wave_overhangs_instead_of_bridges")));
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        line->Add(m_where_btn[i], 1, wxALIGN_CENTER_VERTICAL);
        row(how, line);
    }
    {
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        line->Add(make_label(_L("Wave shape"), "wave_overhang_shape"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        line->AddStretchSpacer();
        line->Add(make_help("wave_overhang_shape"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        m_shape = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(106), -1), 0, nullptr, wxCB_READONLY);
        m_shape->Append(_L("Waves"));
        m_shape->Append(_L("Arcs"));
        m_shape->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
            m_v.shape = std::clamp(m_shape->GetSelection(), 0, 1);
            touched();
            refresh();
        });
        m_shape->Enable(editable("wave_overhang_shape"));
        line->Add(m_shape, 0, wxALIGN_CENTER_VERTICAL);
        row(how, with_reset(line, false, [this]() { return m_v.shape != m_default.shape; }, [this]() { m_v.shape = m_default.shape; }));
    }
    {
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        line->Add(make_label(_L("Pattern"), "pattern"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        line->AddStretchSpacer();
        line->Add(make_help("pattern"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        m_pattern = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(106), -1), 0, nullptr, wxCB_READONLY);
        m_pattern->Append(_L("Monotonic"));
        m_pattern->Append(_L("Zigzag"));
        m_pattern->Append(_L("Smart"));
        m_pattern->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
            m_v.pattern = std::clamp(m_pattern->GetSelection(), 0, 2);
            touched();
            refresh();
        });
        m_pattern->Enable(editable("wave_overhang_pattern"));
        line->Add(m_pattern, 0, wxALIGN_CENTER_VERTICAL);
        row(how, with_reset(line, false, [this]() { return m_v.pattern != m_default.pattern; }, [this]() { m_v.pattern = m_default.pattern; }));
    }
    field(how, _L("Walls kept around"), "wave_overhang_outer_perimeters", wxEmptyString, [this, count](double v) { m_v.walls = count(v, 0, 10); },
          m_walls, [this]() { return m_v.walls != m_default.walls; }, [this]() { m_v.walls = m_default.walls; });
    field(how, _L("Floor layers"), "wave_overhang_floor_layers", wxEmptyString, [this, count](double v) { m_v.floor = count(v, 0, 20); },
          m_floor, [this]() { return m_v.floor != m_default.floor; }, [this]() { m_v.floor = m_default.floor; });
    {
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        m_support  = new ::CheckBox(this);
        m_support->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &evt) {
            m_v.support = m_support->GetValue();
            touched();
            refresh();
            evt.Skip();
        });
        // Supports are needed for this one: greyed without them, as on the Support page.
        m_support->Enable(m_supports_on && editable("support_remaining_areas_after_wave_overhangs"));
        Label *label = make_label(_L("Supports under the rest"), "support");
        label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
            if (!m_support->IsEnabled())
                return;
            m_v.support = !m_v.support;
            touched();
            refresh();
        });
        line->Add(m_support, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        line->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        line->Add(make_help("support"), 0, wxALIGN_CENTER_VERTICAL);
        row(how, with_reset(line, true, [this]() { return m_v.support != m_default.support; }, [this]() { m_v.support = m_default.support; }));
    }

    // The beads: spacing, overlap, flow and its advice, the two minimums.
    wxBoxSizer *beads = column(false);
    row(beads, make_title(_L("Beads"), "beads"));
    field(beads, _L("Wave spacing"), "wave_overhang_line_spacing", "mm", [this](double v) { m_v.spacing = std::clamp(v, 0.05, 2.); }, m_spacing,
          [this, differs]() { return differs(m_v.spacing, m_default.spacing); }, [this]() { m_v.spacing = m_default.spacing; });
    field(beads, _L("Overlap with the walls"), "wave_overhang_perimeter_overlap", "mm", [this](double v) { m_v.overlap = std::clamp(v, 0., 2.); },
          m_overlap, [this, differs]() { return differs(m_v.overlap, m_default.overlap); }, [this]() { m_v.overlap = m_default.overlap; });
    field(beads, _L("Wave flow"), "wave_overhang_flow_mm3_per_mm", L"mm\u00B3/mm", [this](double v) { m_v.flow = std::clamp(v, 0.02, 1.5); },
          m_flow, [this, differs]() { return differs(m_v.flow, m_default.flow); }, [this]() { m_v.flow = m_default.flow; });
    {
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        m_advice   = new Label(this, Label::Body_12, wxEmptyString);
        m_advice->SetForegroundColour(text_colour(true));
        m_tips.emplace_back(m_advice, "wave_overhang_flow_mm3_per_mm");
        m_apply_advice = make_button(_L("Apply"), false, 60);
        m_apply_advice->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            m_v.flow = recommended_flow();
            touched();
            refresh();
        });
        line->Add(m_advice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        line->AddStretchSpacer();
        line->Add(m_apply_advice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(20));
        row(beads, line);
    }
    field(beads, _L("Minimum width"), "wave_overhang_minimum_width", "mm", [this](double v) { m_v.min_width = std::clamp(v, 0., 20.); },
          m_min_width, [this, differs]() { return differs(m_v.min_width, m_default.min_width); }, [this]() { m_v.min_width = m_default.min_width; });
    field(beads, _L("Minimum length"), "wave_overhang_min_length", "mm", [this](double v) { m_v.min_length = std::clamp(v, 0., 50.); },
          m_min_length, [this, differs]() { return differs(m_v.min_length, m_default.min_length); },
          [this]() { m_v.min_length = m_default.min_length; });
    field(beads, _L("Maximum arc radius"), "wave_overhang_arc_max_radius", "mm", [this](double v) { m_v.arc_radius = std::clamp(v, 0., 500.); },
          m_arc_radius, [this, differs]() { return differs(m_v.arc_radius, m_default.arc_radius); },
          [this]() { m_v.arc_radius = m_default.arc_radius; });

    // The speed and the limits, and what the waves cost.
    wxBoxSizer *limits = column(false);
    row(limits, make_title(_L("Speed and limits"), "limits"));
    field(limits, _L("Wave speed"), "wave_overhang_print_speed", "mm/s", [this](double v) { m_v.speed = std::clamp(v, 0.1, 500.); }, m_speed,
          [this, differs]() { return differs(m_v.speed, m_default.speed); }, [this]() { m_v.speed = m_default.speed; });
    field(limits, _L("Travel between waves"), "wave_overhang_travel_speed", "mm/s", [this](double v) { m_v.travel = std::clamp(v, 1., 1000.); },
          m_travel, [this, differs]() { return differs(m_v.travel, m_default.travel); }, [this]() { m_v.travel = m_default.travel; });
    field(limits, _L("Maximum number of waves"), "wave_overhang_max_iterations", wxEmptyString,
          [this, count](double v) { m_v.max_waves = count(v, 0, 500); }, m_max_waves, [this]() { return m_v.max_waves != m_default.max_waves; },
          [this]() { m_v.max_waves = m_default.max_waves; });
    {
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        m_time     = new Label(this, Label::Body_13, wxEmptyString);
        m_tips.emplace_back(m_time, "time");
        line->Add(m_time, 0, wxALIGN_CENTER_VERTICAL);
        row(limits, line);
    }
    main->Add(columns, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP, pad);

    // Under them the two drawings side by side, then the play button and the step of the animation.
    main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    auto *views = new wxBoxSizer(wxHORIZONTAL);
    m_top       = make_canvas(VIEW_W, VIEW_H, [this](wxGraphicsContext &gc, double scale) { paint_top(gc, scale); });
    m_section   = make_canvas(VIEW_W, VIEW_H, [this](wxGraphicsContext &gc, double scale) { paint_section(gc, scale); });
    for (wxPanel *view : {m_top, m_section}) {
        for (auto type : {wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_MOTION, wxEVT_LEAVE_WINDOW})
            view->Bind(type, [this, view](wxMouseEvent &evt) { on_mouse(view, evt); });
        view->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
            m_drag = Drag::None;
            resume_tips_later();
        });
    }
    views->Add(m_top, 0, wxRIGHT, FromDIP(8));
    views->Add(m_section, 0);
    main->Add(views, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(8));

    auto *play = new wxBoxSizer(wxHORIZONTAL);
    m_play     = new Button(this, wxEmptyString, "media_play", wxBORDER_NONE, 26);
    m_tips.emplace_back(m_play, "play");
    m_play->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { start_animation(); });
    m_caption = make_canvas(CAPTION_W, CAPTION_H, [this](wxGraphicsContext &gc, double scale) {
        // On two lines when too long for one (the hint of the marks, in French): cut at the last space that fits.
        wxString first = m_shown_caption, second;
        double   w = 0., h = 0.;
        gc.SetFont(Label::Body_12, text_colour());
        gc.GetTextExtent(first, &w, &h);
        while (w > CAPTION_W * scale) {
            const int space = first.Find(' ', true);
            if (space == wxNOT_FOUND)
                break;
            second = first.Mid(space + 1) + (second.empty() ? "" : " ") + second;
            first.Truncate(space);
            gc.GetTextExtent(first, &w, &h);
        }
        const double mid = CAPTION_H * scale / 2.;
        if (second.empty())
            draw_text(gc, first, 0., mid, text_colour(), Label::Body_12, -1);
        else {
            draw_text(gc, first, 0., mid - h / 2., text_colour(), Label::Body_12, -1);
            draw_text(gc, second, 0., mid + h / 2., text_colour(), Label::Body_12, -1);
        }
    });
    play->Add(m_play, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    play->Add(m_caption, 0, wxALIGN_CENTER_VERTICAL);
    main->Add(play, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));

    // At the bottom: everything back to the defaults on the left, OK and Cancel on the right.
    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    // Only the arrows made before this one: its own is added to m_resets too (calling itself = stack overflow).
    auto any = [this, n = m_resets.size()]() {
        for (size_t i = 0; i < n; ++i)
            if (m_resets[i].modified())
                return true;
        return false;
    };
    Button *reset_all = make_button(_L("Reset all"), false);
    reset_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_v = m_default;
        touched();
        refresh();
    });
    m_tips.emplace_back(reset_all, "reset_all");
    buttons->Add(make_reset(any, [this]() { m_v = m_default; }), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    buttons->Add(reset_all, 0);
    buttons->AddStretchSpacer();
    Button *ok     = make_button(_L("OK"), true);
    Button *cancel = make_button(_L("Cancel"), false);
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_timer.Stop();
        apply();
        EndModal(wxID_OK);
    });
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    buttons->Add(ok, 0, wxRIGHT, FromDIP(8));
    buttons->Add(cancel, 0);
    main->Add(buttons, 0, wxEXPAND | wxALL, pad);

    SetSizer(main);
    main->SetSizeHints(this);
    Layout();
}

void WaveOverhangsDialog::touched()
{
    m_anim_done = false;
    if (m_animating) {
        m_timer.Stop();
        m_animating  = false;
        m_anim_waves = m_anim_floor = -1.;
    }
}

void WaveOverhangsDialog::refresh()
{
    m_updating = true;
    // Not the field being typed in: it keeps the text as typed.
    auto set_text = [](wxTextCtrl *ctrl, const wxString &text) {
        if (ctrl != nullptr && wxWindow::FindFocus() != ctrl && ctrl->GetValue() != text)
            ctrl->ChangeValue(text);
    };
    set_text(m_walls, wxString::Format("%d", m_v.walls));
    set_text(m_floor, wxString::Format("%d", m_v.floor));
    set_text(m_spacing, num(m_v.spacing));
    set_text(m_overlap, num(m_v.overlap));
    set_text(m_flow, num(m_v.flow));
    set_text(m_min_width, num(m_v.min_width));
    set_text(m_min_length, num(m_v.min_length));
    set_text(m_speed, num(m_v.speed));
    set_text(m_travel, num(m_v.travel));
    set_text(m_max_waves, wxString::Format("%d", m_v.max_waves));
    set_text(m_arc_radius, num(m_v.arc_radius));
    if (m_pattern->GetSelection() != m_v.pattern)
        m_pattern->SetSelection(m_v.pattern);
    if (m_shape->GetSelection() != m_v.shape)
        m_shape->SetSelection(m_v.shape);
    // The radius is only for the arcs, greyed with the waves (as on the Quality page).
    if (const bool on = m_v.shape == int(WaveOverhangShape::Arcs) && editable("wave_overhang_arc_max_radius"); m_arc_radius->IsEnabled() != on)
        m_arc_radius->Enable(on);
    if (m_support->GetValue() != m_v.support)
        m_support->SetValue(m_v.support);
    if (m_styled_where != m_v.where()) {
        m_styled_where = m_v.where();
        for (int i = 0; i < 3; ++i)
            style_button(m_where_btn[i], m_v.where() == i);
    }
    const double recommended = recommended_flow();
    if (const wxString text = wxString::Format(_L("Advised for a %s mm nozzle: %s"), num(m_nozzle), num(recommended)); m_advice->GetLabel() != text)
        m_advice->SetLabel(text);
    if (const bool on = std::abs(m_v.flow - recommended) > 0.005 && editable("wave_overhang_flow_mm3_per_mm"); m_apply_advice->IsEnabled() != on)
        m_apply_advice->Enable(on);
    {
        const double seconds = m_v.waves ? extra_seconds() : 0.;
        const wxString time  = seconds < 60. ? wxString::Format(_L("%d s"), int(std::lround(seconds)))
                                             : wxString::Format(_L("%d min"), int(std::lround(seconds / 60.)));
        if (const wxString text = wxString::Format(_L("About +%s for this overhang"), time); m_time->GetLabel() != text)
            m_time->SetLabel(text);
    }
    show_caption();
    // The red arrows, painted again when they appear or go.
    for (Reset &reset : m_resets)
        if (const bool on = reset.modified(); on != reset.shown) {
            reset.shown = on;
            reset.panel->SetCursor(on ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
            if (on)
                reset.panel->SetToolTip(_L("Back to the default value"));
            else
                reset.panel->UnsetToolTip();
            reset.panel->Refresh();
        }
    for (auto &[window, key] : m_tips)
        if (const wxString text = tip(key); window->GetToolTipText() != text)
            window->SetToolTip(text);
    if (!tips_paused() && !m_view_tip.empty())
        for (wxPanel *view : {m_top, m_section})
            if (view->HasCapture() || view->GetClientRect().Contains(view->ScreenToClient(wxGetMousePosition())))
                if (const wxString text = tip(m_view_tip); view->GetToolTipText() != text)
                    view->SetToolTip(text);
    m_legend->Refresh();
    m_top->Refresh();
    m_section->Refresh();
    Layout();
    m_updating = false;
}

void WaveOverhangsDialog::show_caption()
{
    if (const wxString text = caption(); text != m_shown_caption) {
        m_shown_caption = text;
        m_caption->Refresh();
    }
}

bool WaveOverhangsDialog::tips_paused() const { return m_drag != Drag::None || std::chrono::steady_clock::now() < m_tips_resume; }

void WaveOverhangsDialog::pause_tips()
{
    m_tip_timer.Stop();
    m_view_tip.clear();
    m_top->UnsetToolTip();
    m_section->UnsetToolTip();
}

void WaveOverhangsDialog::resume_tips_later()
{
    m_tips_resume = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    m_tip_timer.StartOnce(2000);
}

void WaveOverhangsDialog::resume_tips()
{
    if (m_drag != Drag::None)
        return;
    m_tips_resume = std::chrono::steady_clock::time_point();
    // The tooltip of what is under the mouse, if it is over a drawing.
    for (wxPanel *view : {m_top, m_section}) {
        const wxPoint p = view->ScreenToClient(wxGetMousePosition());
        if (!m_animating && view->GetClientRect().Contains(p)) {
            const double scale = view->GetClientSize().x / VIEW_W;
            m_view_tip         = region_at(view, p.x / scale, p.y / scale);
            if (!m_view_tip.empty())
                view->SetToolTip(tip(m_view_tip));
        }
    }
}

wxString WaveOverhangsDialog::caption() const
{
    if (!m_v.waves)
        return _L("No waves: the overhangs are printed as usual.");
    if (m_animating) {
        if (m_anim_floor <= 0.)
            return wxString::Format(_L("Wave %d / %d"), std::min(waves_printed(), int(m_anim_waves) + 1), waves_printed());
        return wxString::Format(_L("Floor layer %d / %d"), std::min(m_v.floor, int(m_anim_floor) + 1), m_v.floor);
    }
    if (m_anim_done)
        return m_v.floor > 0 ? _L("The floor covers the waves: the next layers rest on it.") : _L("The top layers will cover the waves.");
    return _L("Drag the marks: the 2nd wave sets the spacing, the 1st one the overlap, a bead its flow, the floor its layers.");
}

wxString WaveOverhangsDialog::tip(const std::string &key) const
{
    if (key == "play")
        return _L("Show the waves being laid, then the floor");
    if (key == "reset_all")
        return _L("All the values of this window back to the default ones");
    if (key == "where")
        return _L("Nowhere: the overhangs are printed as usual (walls, bridges, supports).\n"
                  "Overhangs only: the unsupported part of the overhangs is replaced by waves grown from the supported edge; simple "
                  "flat spans stay regular bridges.\n"
                  "Overhangs and bridges: no bridge at all in the region, the waves and solid infill instead.\n"
                  "Needs at least one wall; not used in spiral vase mode nor on tilted layers.");
    if (key == "pattern" || key == "wave_overhang_pattern")
        return _L("Monotonic: every wave printed in the same direction.\nZigzag: back and forth, the waves connected into a meander.\n"
                  "Smart: each new wave starts from its better-supported end.");
    if (key == "wave_overhang_shape" || key == "wave_overhang_arc_max_radius")
        return _(print_config_def.get(key)->tooltip);
    if (key == "wave_overhang_outer_perimeters")
        return _L("Number of outer walls kept at the edge of the overhang (in orange on the drawings); the rest is replaced by waves. "
                  "1 is usually enough. 0 = waves only, no wall at the edge.");
    if (key == "wave_overhang_floor_layers")
        return _L("Number of solid layers (in purple) placed right above the waves: they give the overhang its strength. "
                  "0 = no specific floor: the top shell layers apply.");
    if (key == "support")
        return m_supports_on ? _L("Checked, supports are only generated under what the waves did not cover (beyond the maximum number of "
                                  "waves). Unchecked, supports are generated as if there were no waves.")
                             : _L("Supports are disabled (Support page): nothing is generated under the overhangs.");
    if (key == "beads")
        return _L("The wave lines: how far apart, how close to the walls, how much plastic. They hang in the air, each one on the "
                  "previous one.");
    if (key == "wave_overhang_line_spacing")
        return _L("Distance between the centers of two neighbouring waves. Smaller: the beads overlap more and hold better, but more "
                  "waves to print. Drag the second wave on the drawing seen from above.");
    if (key == "wave_overhang_perimeter_overlap")
        return _L("How far the first wave goes onto the supported part (the pillar here), fewer gaps along it. The waves never go "
                  "beyond the outer walls of the overhang. Drag the first wave on the drawing seen from above.");
    if (key == "wave_overhang_flow_mm3_per_mm")
        return wxString::Format(_L("Plastic laid per millimetre of wave. The bead hangs in the air: the layer height does not change "
                                   "it. 0.15 is calibrated for a 0.4 mm nozzle; for other nozzles 0.15 x (nozzle / 0.4)%s: about %s "
                                   "here. Raise it if the waves look thin or broken, lower it if they blob together. Drag the top "
                                   "of a bead on the section."),
                                L"\u00B2", num(recommended_flow()));
    if (key == "wave_overhang_minimum_width")
        return _L("Where the region of the waves narrows to less than this width, it is split there first, so that no fragile thin "
                  "branch of waves forms. Larger: more splits. Not shown on the drawings.");
    if (key == "wave_overhang_min_length")
        return _L("Overhangs whose outline is shorter than this get no waves: small overhangs are fine with normal walls. "
                  "0 = no limit. Not shown on the drawings.");
    if (key == "limits")
        return _L("The waves are printed slowly, so that each one cools and holds before the next one.");
    if (key == "wave_overhang_print_speed")
        return _L("Print speed of the waves. Slow: they cool and stick better to the previous one. Also on the Speed page.");
    if (key == "wave_overhang_travel_speed")
        return _L("Speed of the moves without extruding between the waves. Also on the Speed page.");
    if (key == "wave_overhang_max_iterations")
        return _L("Most waves laid in each overhang. 0 = no limit: the waves grow until the overhang is covered. A limit bounds the "
                  "print time of a large overhang: beyond it, the rest is printed as usual (bridge, or on supports).");
    if (key == "time")
        return wxString::Format(_L("Time added by the waves of the overhang drawn (a %s mm slab around a %s mm pillar): %d waves at %s "
                                   "mm/s. The real time of your part is shown after slicing."),
                                num(OVERHANG), num(2. * PILLAR), waves_printed(), num(m_v.speed));
    if (key == "spacing_handle")
        return _L("Drag left or right: the spacing of the waves.");
    if (key == "overlap_handle")
        return _L("Drag up or down: how far the first wave goes onto the pillar (the overlap).");
    if (key == "flow_handle")
        return _L("Drag up or down: the size of the bead, so its flow.");
    if (key == "floor_handle")
        return _L("Drag up or down: the number of floor layers.");
    if (key == "walls_handle")
        return _L("Drag left or right: the number of walls kept at the edge of the overhang.");
    if (key == "radius_handle")
        return wxString::Format(_L("Drag left or right: the maximum arc radius (%s mm), where a new fan starts. True to scale on the "
                                   "slab; beyond its edge the scale is compressed, up to 500 mm."),
                                num(m_v.arc_radius));
    if (key == "pillar")
        return _L("The supported part: here a pillar, under the slab that overhangs.");
    if (key == "waves" && m_v.shape == int(WaveOverhangShape::Arcs))
        return wxString::Format(_L("The arcs (in the overhang color of the Preview): fans of %d arcs grown from the middle of each side of "
                                   "the pillar, each arc hanging on the previous one; a new fan starts at each maximum arc radius."),
                                waves_printed());
    if (key == "waves")
        return wxString::Format(_L("The waves (in the overhang color of the Preview): %d rings around the pillar, each one hanging on "
                                   "the previous one."),
                                waves_printed());
    if (key == "walls")
        return _L("The outer walls kept at the edge of the overhang.");
    if (key == "rest")
        return m_supports_on && m_v.support ? _L("Beyond the maximum number of waves: printed as usual, on supports.")
                                            : _L("Beyond the maximum number of waves: printed as usual, as a bridge.");
    if (key == "floor")
        return wxString::Format(_L("The floor: %d solid layers over the waves."), m_v.floor);
    if (key == "supports")
        return _L("Supports under the overhang.");
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------
// The diagram

double WaveOverhangsDialog::wave_at(int k) const { return -m_v.overlap + (k + 0.5) * m_v.spacing; }
// The waves stop at the walls kept at the edge: the overlap is only on the supported side (as the engine does).
double WaveOverhangsDialog::waves_end() const { return OVERHANG - m_v.walls * m_nozzle * 1.125; }
int    WaveOverhangsDialog::waves_needed() const
{
    return std::clamp(int(std::floor((waves_end() + m_v.overlap) / std::max(0.05, m_v.spacing))), 0, MAX_DRAWN);
}
int    WaveOverhangsDialog::waves_printed() const { return m_v.max_waves > 0 ? std::min(waves_needed(), m_v.max_waves) : waves_needed(); }
double WaveOverhangsDialog::bead() const { return 2. * std::sqrt(m_v.flow / PI); }
// As the tooltip of the option tells: 0.15 for a 0.4 mm nozzle, scaled by the square of the nozzle.
double WaveOverhangsDialog::recommended_flow() const { return std::round(0.15 * std::pow(m_nozzle / 0.4, 2.) * 100.) / 100.; }

double WaveOverhangsDialog::extra_seconds() const
{
    double length = 0.;
    for (int k = 0; k < waves_printed(); ++k)
        length += ring_length(wave_at(k));
    return length / std::max(0.1, m_v.speed) + waves_printed() * m_v.spacing / std::max(1., m_v.travel);
}

void WaveOverhangsDialog::paint_legend(wxGraphicsContext &gc, double scale)
{
    const bool dark = wxGetApp().dark_mode();
    const double h  = TOP_LEGEND_H * scale;
    // The Beta pill.
    gc.SetFont(Label::Body_12, dark ? wxColour(0, 194, 255) : wxColour(8, 116, 224));
    double w = 0., th = 0.;
    gc.GetTextExtent(_L("Beta"), &w, &th);
    const double pw = w + 16. * scale, ph = 18. * scale;
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(dark ? wxColour(16, 52, 88) : wxColour(220, 238, 254)));
    gc.DrawRoundedRectangle(0., (h - ph) / 2., pw, ph, ph / 2.);
    gc.DrawText(_L("Beta"), 8. * scale, (h - th) / 2.);
    const wxString text = m_problem.empty() ? _L("Print an overhang without supports, as rings leaning on each other.")
                                            : wxString::Format(_L("The waves cannot be printed: %s."), m_problem);
    draw_text(gc, text, pw + 10. * scale, h / 2., m_problem.empty() ? text_colour() : wxColour(230, 81, 0), Label::Body_13, -1);
}

// The pale veil of "Nowhere", over a drawing.
void WaveOverhangsDialog::paint_veil(wxGraphicsContext &gc)
{
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(with_alpha(GetBackgroundColour(), 200)));
    gc.DrawRectangle(0., 0., VIEW_W, VIEW_H);
    draw_text(gc, _L("The overhangs are printed as usual"), VIEW_W / 2., VIEW_H / 2., text_colour(), Label::Body_13, 0);
}

// Seen from above: the slab around the pillar, the waves as rings (the 2nd one sets the spacing, the 1st one the overlap), the walls
// kept at its edge, what the maximum leaves tinted, the floor hatched over them.
void WaveOverhangsDialog::paint_top(wxGraphicsContext &gc, double scale)
{
    const bool     dark = wxGetApp().dark_mode();
    const wxColour bg   = GetBackgroundColour();
    const wxColour slab = dark ? wxColour(52, 56, 64) : wxColour(232, 238, 246);
    const wxColour line = text_colour(true);
    gc.PushState();
    gc.Scale(scale, scale);
    auto fill_ring = [&gc](double offset, const wxColour &fill, const wxColour &outline) {
        Points pts = ring_points(offset);
        pts.pop_back();
        fill_polygon(gc, pts, fill, outline);
    };
    const double w = m_nozzle * 1.125;
    fill_ring(OVERHANG, slab, line);
    const int needed = waves_needed(), printed = waves_printed();
    if (m_v.waves) {
        // Beyond the maximum, printed as usual: on supports, or as a bridge.
        if (printed < needed) {
            fill_ring(OVERHANG - m_v.walls * w, with_alpha(m_supports_on && m_v.support ? SUPPORT_COLOUR : BRIDGE_COLOUR, 110), slab);
            fill_ring(printed > 0 ? wave_at(printed - 1) + m_v.spacing / 2. : 0., slab, slab);
        }
        const double shown = m_anim_waves < 0. ? printed : m_anim_waves;
        const bool arcs = m_v.shape == int(WaveOverhangShape::Arcs);
        for (int k = 0; k < printed && k < shown; ++k) {
            const bool reversed = m_v.pattern == int(WaveOverhangPattern::ZigZag) && k % 2 == 1;
            // Rings around the pillar; in arcs, a fan on each of its sides.
            for (int side = 0; side < (arcs ? 4 : 1); ++side) {
                const Points pts = arcs ? arc_points(side, wave_at(k), m_v.arc_radius) : ring_points(wave_at(k));
                if (pts.size() < 2)
                    continue;
                gc.SetBrush(*wxTRANSPARENT_BRUSH);
                gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(WAVE_COLOUR, std::max(1., bead() * TOP_MM)).Cap(wxCAP_BUTT).Join(wxJOIN_ROUND)));
                stroke_part(gc, pts, shown - k, reversed);
                gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(with_alpha(*wxWHITE, 110), 0.8)));
                stroke_part(gc, pts, shown - k, reversed);
            }
        }
        for (int j = 0; j < m_v.walls; ++j) {
            gc.SetBrush(*wxTRANSPARENT_BRUSH);
            gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(WALL_COLOUR, w * TOP_MM).Join(wxJOIN_ROUND)));
            const Points pts = ring_points(OVERHANG - (j + 0.5) * w);
            gc.StrokeLines(pts.size(), pts.data());
        }
        // The floor over them, hatched (once laid in the animation).
        if (m_v.floor > 0 && (m_anim_waves < 0. || m_anim_floor >= 1.)) {
            const double half = (PILLAR + OVERHANG) * TOP_MM;
            gc.Clip(CX - half, CY - half, 2. * half, 2. * half);
            gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(with_alpha(FLOOR_COLOUR, 120), 1.2)));
            for (double d = -2. * half; d < 2. * half; d += 9.)
                gc.StrokeLine(CX + d - half, CY + half, CX + d + half, CY - half);
            gc.ResetClip();
        }
    }
    fill_ring(0., dark ? wxColour(110, 110, 115) : wxColour(176, 176, 176), line);
    if (m_v.waves && m_anim_waves < 0. && printed >= 1) {
        draw_handle(gc, CX, CY - (PILLAR + wave_at(0)) * TOP_MM, bg, true);
        if (printed >= 2)
            draw_handle(gc, CX + (PILLAR + wave_at(1)) * TOP_MM, CY, bg, false);
        // In arcs, on the edge of the first fan on the left.
        if (m_v.shape == int(WaveOverhangShape::Arcs))
            draw_handle(gc, CX - (PILLAR + radius_to_handle(m_v.arc_radius)) * TOP_MM, CY, bg, false);
    }
    draw_text(gc, _L("Seen from above (diagram)"), VIEW_W / 2., LEGEND_Y, line, Label::Body_12, 0);
    if (!m_v.waves)
        paint_veil(gc);
    gc.PopState();
}

// In section, from the pillar to the edge of the slab: the beads, round, each one hanging on the previous one (the top of the 2nd one
// sets the flow), the walls kept at the edge, the floor over them (its top sets its layers), supports under what is left.
void WaveOverhangsDialog::paint_section(wxGraphicsContext &gc, double scale)
{
    const bool     dark  = wxGetApp().dark_mode();
    const wxColour bg    = GetBackgroundColour();
    const wxColour line  = text_colour(true);
    const wxColour solid = dark ? wxColour(110, 110, 115) : wxColour(176, 176, 176);
    const double   w = m_nozzle * 1.125, hpx = m_layer_height * SEC_MM, r = bead() / 2. * SEC_MM;
    const double   sxe = SX0 + OVERHANG * SEC_MM, base = Y_WAVE - std::max(hpx / 2., r);
    const int      printed = waves_printed(), needed = std::max(printed, waves_needed());
    const double   rest_x  = printed > 0 ? SX0 + (wave_at(printed - 1) + m_v.spacing / 2.) * SEC_MM : SX0;
    gc.PushState();
    gc.Scale(scale, scale);
    // The pillar, its layers in thin lines, its outer wall.
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(solid));
    gc.DrawRectangle(SX_LEFT, base, SX0 - SX_LEFT, Y_GROUND - base);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(with_alpha(bg, 120), 1.)));
    for (double y = Y_WAVE + hpx / 2.; y < Y_GROUND; y += hpx)
        gc.StrokeLine(SX_LEFT, y, SX0, y);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(WALL_COLOUR));
    gc.DrawRectangle(SX0 - w * SEC_MM, Y_WAVE + hpx / 2., w * SEC_MM, Y_GROUND - Y_WAVE - hpx / 2.);
    if (m_v.waves) {
        // Supports: under what the waves leave, or under all the overhang when unchecked.
        const double support_x = m_v.support ? rest_x : SX0;
        if (m_supports_on && (!m_v.support || printed < needed)) {
            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.SetBrush(wxBrush(with_alpha(SUPPORT_COLOUR, dark ? 120 : 150)));
            for (double x = support_x + 2.; x < sxe - 2.; x += 9.)
                gc.DrawRectangle(x, Y_WAVE + std::max(hpx / 2., r) + 3., 3., Y_GROUND - Y_WAVE - std::max(hpx / 2., r) - 3.);
        }
        if (printed < needed) {
            gc.SetBrush(wxBrush(BRIDGE_COLOUR));
            gc.DrawRectangle(rest_x, Y_WAVE - hpx / 2., sxe - m_v.walls * w * SEC_MM - rest_x, hpx);
        }
        const double shown = m_anim_waves < 0. ? printed : m_anim_waves;
        for (int k = 0; k < printed && k + 1 <= shown + 1e-9; ++k) {
            const double x = SX0 + wave_at(k) * SEC_MM;
            gc.SetBrush(wxBrush(WAVE_COLOUR));
            gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(darker(WAVE_COLOUR, 0.4), 1.)));
            gc.DrawEllipse(x - r, Y_WAVE - r, 2. * r, 2. * r);
        }
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(WALL_COLOUR));
        for (int j = 0; j < m_v.walls; ++j)
            gc.DrawRoundedRectangle(sxe - (j + 1) * w * SEC_MM, Y_WAVE - hpx / 2., w * SEC_MM, hpx, hpx / 2.);
    }
    // The floor (or the top layers, dashed, without floor), as many layers as fit; over the pillar too, the slab is solid there.
    const int    fits   = std::max(1, int((base - 8.) / hpx));
    const int    layers = !m_v.waves ? 0 : m_anim_waves >= 0. ? std::min(m_v.floor, int(std::max(0., m_anim_floor))) : m_v.floor;
    const double top    = base - std::min(layers, fits) * hpx;
    for (int j = 0; j < std::min(layers, fits); ++j) {
        gc.SetBrush(wxBrush(FLOOR_COLOUR));
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(with_alpha(bg, 160), 1.)));
        gc.DrawRectangle(SX_LEFT, base - (j + 1) * hpx, sxe - SX_LEFT, hpx);
    }
    if (m_v.waves && m_v.floor == 0) {
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(line, 1.).Style(wxPENSTYLE_SHORT_DASH)));
        gc.DrawRectangle(SX_LEFT, base - hpx, sxe - SX_LEFT, hpx);
    }
    if (m_v.floor > fits && layers > fits)
        draw_text(gc, wxString::Format("x%d", m_v.floor), sxe + 4., top + hpx / 2., line, Label::Body_12, -1);
    if (m_v.waves && m_anim_waves < 0.) {
        if (printed >= 1)
            draw_handle(gc, SX0 + wave_at(std::min(printed - 1, 1)) * SEC_MM, Y_WAVE - r, bg, true);
        draw_handle(gc, sxe - 16., top, bg, true);
        draw_handle(gc, walls_handle_x(), walls_handle_y(), bg, false);
    }
    draw_text(gc, _L("Section (diagram)"), VIEW_W / 2., LEGEND_Y, line, Label::Body_12, 0);
    if (!m_v.waves)
        paint_veil(gc);
    gc.PopState();
}

WaveOverhangsDialog::Drag WaveOverhangsDialog::handle_at(wxPanel *view, double x, double y) const
{
    if (!m_v.waves || m_animating)
        return Drag::None;
    auto close_to = [x, y](double hx, double hy) { return std::hypot(x - hx, y - hy) <= HANDLE_R + 3.; };
    const int printed = waves_printed();
    if (view == m_top) {
        if (printed >= 2 && close_to(CX + (PILLAR + wave_at(1)) * TOP_MM, CY) && editable("wave_overhang_line_spacing"))
            return Drag::Spacing;
        if (printed >= 1 && close_to(CX, CY - (PILLAR + wave_at(0)) * TOP_MM) && editable("wave_overhang_perimeter_overlap"))
            return Drag::Overlap;
        if (m_v.shape == int(WaveOverhangShape::Arcs) && printed >= 1 && close_to(CX - (PILLAR + radius_to_handle(m_v.arc_radius)) * TOP_MM, CY) &&
            editable("wave_overhang_arc_max_radius"))
            return Drag::Radius;
        return Drag::None;
    }
    const double hpx = m_layer_height * SEC_MM, r = bead() / 2. * SEC_MM, base = Y_WAVE - std::max(hpx / 2., r);
    const int    fits = std::max(1, int((base - 8.) / hpx));
    if (printed >= 1 && close_to(SX0 + wave_at(std::min(printed - 1, 1)) * SEC_MM, Y_WAVE - r) && editable("wave_overhang_flow_mm3_per_mm"))
        return Drag::Flow;
    if (close_to(SX0 + OVERHANG * SEC_MM - 16., base - std::min(m_v.floor, fits) * hpx) && editable("wave_overhang_floor_layers"))
        return Drag::Floor;
    if (close_to(walls_handle_x(), walls_handle_y()) && editable("wave_overhang_outer_perimeters"))
        return Drag::Walls;
    return Drag::None;
}

// Under the layer of the waves, at the inner edge of the walls kept.
double WaveOverhangsDialog::walls_handle_x() const { return SX0 + std::max(0., OVERHANG - m_v.walls * m_nozzle * 1.125) * SEC_MM; }
double WaveOverhangsDialog::walls_handle_y() const
{
    return Y_WAVE + std::max(m_layer_height * SEC_MM / 2., bead() / 2. * SEC_MM) + HANDLE_R + 3.;
}

std::string WaveOverhangsDialog::region_at(wxPanel *view, double x, double y) const
{
    if (!m_v.waves || y > LEGEND_Y - 10.)
        return {};
    switch (handle_at(view, x, y)) {
    case Drag::Spacing: return "spacing_handle";
    case Drag::Overlap: return "overlap_handle";
    case Drag::Flow: return "flow_handle";
    case Drag::Floor: return "floor_handle";
    case Drag::Walls: return "walls_handle";
    case Drag::Radius: return "radius_handle";
    default: break;
    }
    const double w = m_nozzle * 1.125;
    // The distance from the edge of the pillar, in mm.
    double mm;
    if (view == m_top)
        mm = std::max(std::abs(x - CX), std::abs(y - CY)) / TOP_MM - PILLAR;
    else {
        const double hpx = m_layer_height * SEC_MM, r = bead() / 2. * SEC_MM, base = Y_WAVE - std::max(hpx / 2., r);
        mm = (x - SX0) / SEC_MM;
        if (x < SX_LEFT || mm > OVERHANG)
            return {};
        if (y < base)
            return y > base - std::max(1, m_v.floor) * hpx ? "floor" : std::string();
        if (y > Y_WAVE + std::max(hpx / 2., r))
            return mm < 0. ? "pillar" : m_supports_on ? "supports" : std::string();
    }
    if (mm < 0.)
        return "pillar";
    if (mm > OVERHANG)
        return {};
    if (mm > OVERHANG - m_v.walls * w)
        return "walls";
    return waves_printed() < waves_needed() && mm > wave_at(waves_printed() - 1) + m_v.spacing / 2. ? "rest" : "waves";
}

void WaveOverhangsDialog::on_mouse(wxPanel *view, wxMouseEvent &evt)
{
    const double scale = view->GetClientSize().x / VIEW_W;
    const double x = evt.GetX() / scale, y = evt.GetY() / scale;
    const double hpx = m_layer_height * SEC_MM;
    if (evt.LeftDown()) {
        m_drag      = handle_at(view, x, y);
        m_drag_view = view;
        // The offset of the mouse from the center of the handle, along the way it moves.
        const double r = bead() / 2. * SEC_MM, base = Y_WAVE - std::max(hpx / 2., r);
        switch (m_drag) {
        case Drag::Spacing: m_grab = x - (CX + (PILLAR + wave_at(1)) * TOP_MM); break;
        case Drag::Overlap: m_grab = y - (CY - (PILLAR + wave_at(0)) * TOP_MM); break;
        case Drag::Flow: m_grab = y - (Y_WAVE - r); break;
        case Drag::Floor: m_grab = y - (base - std::min(m_v.floor, std::max(1, int((base - 8.) / hpx))) * hpx); break;
        case Drag::Walls: m_grab = x - walls_handle_x(); break;
        case Drag::Radius: m_grab = x - (CX - (PILLAR + radius_to_handle(m_v.arc_radius)) * TOP_MM); break;
        default: break;
        }
        if (m_drag != Drag::None) {
            touched();
            pause_tips();
            view->CaptureMouse();
        }
    } else if (evt.LeftUp()) {
        if (view->HasCapture())
            view->ReleaseMouse();
        if (m_drag != Drag::None) {
            m_drag = Drag::None;
            resume_tips_later();
        }
    } else if (evt.Dragging() && m_drag != Drag::None && view == m_drag_view) {
        const Values old = m_v;
        if (m_drag == Drag::Spacing) {
            const double mm = (x - m_grab - CX) / TOP_MM - PILLAR;
            m_v.spacing     = std::round(std::clamp((mm + m_v.overlap) / 1.5, 0.05, 2.) * 100.) / 100.;
        } else if (m_drag == Drag::Overlap) {
            const double mm = (CY - (y - m_grab)) / TOP_MM - PILLAR;
            m_v.overlap     = std::round(std::clamp(m_v.spacing / 2. - mm, 0., 2.) * 100.) / 100.;
        } else if (m_drag == Drag::Flow) {
            const double d = 2. * (Y_WAVE - (y - m_grab)) / SEC_MM;
            m_v.flow       = std::round(std::clamp(PI * d * d / 4., 0.02, 1.5) * 100.) / 100.;
        } else if (m_drag == Drag::Floor) {
            const double base = Y_WAVE - std::max(hpx / 2., bead() / 2. * SEC_MM);
            m_v.floor         = std::clamp(int(std::lround((base - (y - m_grab)) / hpx)), 0, std::min(20, std::max(1, int((base - 8.) / hpx))));
        } else if (m_drag == Drag::Walls) {
            // As many walls as fit in the slab drawn (the field goes up to 10).
            const double w = m_nozzle * 1.125;
            m_v.walls      = std::clamp(int(std::lround((SX0 + OVERHANG * SEC_MM - (x - m_grab)) / (w * SEC_MM))), 0, std::min(10, int(OVERHANG / w)));
        } else if (m_drag == Drag::Radius) {
            m_v.arc_radius = handle_to_radius((CX - (x - m_grab)) / TOP_MM - PILLAR);
        }
        // Only when the rounded value moved: the mouse moves far more often.
        if (m_v.spacing != old.spacing || m_v.overlap != old.overlap || m_v.flow != old.flow || m_v.floor != old.floor ||
            m_v.walls != old.walls || m_v.arc_radius != old.arc_radius)
            refresh();
    }
    // The cursor, and the tooltip of what is under the mouse (none while dragging, and 2 s after).
    const Drag hover = m_drag != Drag::None ? m_drag : handle_at(view, x, y);
    const bool sideways = hover == Drag::Spacing || hover == Drag::Walls || hover == Drag::Radius;
    view->SetCursor(sideways ? wxCursor(wxCURSOR_SIZEWE) : hover != Drag::None ? wxCursor(wxCURSOR_SIZENS) : wxNullCursor);
    if (!tips_paused()) {
        const std::string region = evt.Leaving() || m_animating ? std::string() : region_at(view, x, y);
        if (region != m_view_tip || view->GetToolTipText() != tip(region)) {
            m_view_tip = region;
            if (region.empty())
                view->UnsetToolTip();
            else
                view->SetToolTip(tip(region));
        }
    }
    evt.Skip();
}

// ---------------------------------------------------------------------------------------------------------------------
// The animation: the waves in their order, then the floor layer by layer.

void WaveOverhangsDialog::start_animation()
{
    if (!m_v.waves || waves_printed() == 0)
        return;
    m_animating  = true;
    m_anim_done  = false;
    m_anim_start = std::chrono::steady_clock::now();
    m_anim_waves = 0.;
    m_anim_floor = 0.;
    pause_tips();
    m_timer.Start(16);
    animation_step();
}

void WaveOverhangsDialog::animation_step()
{
    if (!m_animating)
        return;
    const double t       = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_anim_start).count();
    const double floor_s = m_v.floor > 0 ? FLOOR_SECONDS : 0.;
    if (t < WAVES_SECONDS) {
        m_anim_waves = t / WAVES_SECONDS * waves_printed();
        m_anim_floor = 0.;
    } else if (t < WAVES_SECONDS + floor_s) {
        m_anim_waves = waves_printed();
        m_anim_floor = (t - WAVES_SECONDS) / floor_s * m_v.floor;
    } else {
        m_timer.Stop();
        m_animating  = false;
        m_anim_done  = true;
        m_anim_waves = m_anim_floor = -1.;
        resume_tips_later();
    }
    show_caption();
    m_top->Refresh();
    m_section->Refresh();
}

void WaveOverhangsDialog::on_dpi_changed(const wxRect &)
{
    for (wxWindow *child : GetChildren())
        if (auto *btn = dynamic_cast<Button *>(child)) {
            if (btn != m_play) {
                btn->SetMinSize(wxSize(FromDIP(btn == m_apply_advice ? 60 : 84), FromDIP(26)));
                btn->SetCornerRadius(FromDIP(12));
            }
            btn->Rescale();
        }
    m_legend->SetMinSize(wxSize(FromDIP(int(TOP_LEGEND_W)), FromDIP(int(TOP_LEGEND_H))));
    m_top->SetMinSize(wxSize(FromDIP(int(VIEW_W)), FromDIP(int(VIEW_H))));
    m_section->SetMinSize(wxSize(FromDIP(int(VIEW_W)), FromDIP(int(VIEW_H))));
    m_caption->SetMinSize(wxSize(FromDIP(int(CAPTION_W)), FromDIP(int(CAPTION_H))));
    for (Reset &reset : m_resets)
        reset.panel->SetMinSize(wxSize(FromDIP(int(RESET_SIZE)), FromDIP(int(RESET_SIZE))));
    Layout();
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
