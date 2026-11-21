#include "ScarfSeamDialog.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "Field.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"

#include <wx/arrstr.h>
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

// The side view, in logical units (x along the wall, y down) scaled to the width of its panel: the layer printed, from the
// end of the loop coming to the seam (left) to the loop going on (right), over two layers. Heights exaggerated.
constexpr double SIDE_W   = 640.;
constexpr double XA       = 20.;  // the end of the loop, coming to the seam
constexpr double X0       = 90.;  // the seam
constexpr double X1       = 596.; // the loop goes on
constexpr double Y_TOP    = 100.;
constexpr double LAYER_H  = 44.;
constexpr double Y_BASE   = Y_TOP + LAYER_H;
constexpr double Y_LEGEND = Y_BASE + 2. * LAYER_H + 20.;
constexpr double SIDE_H   = Y_LEGEND + 14.;
// The slider of the slope smoothness, centered on the side view, at the height of the word "seam".
constexpr double STEPS_X0  = SIDE_W / 2. - 60.;
constexpr double STEPS_X1  = SIDE_W / 2. + 60.;
constexpr double STEPS_Y   = 26.;
constexpr int    STEPS_MAX = 40;
// "Where" seen from above, shown larger; the corner of the angle, its examples under it.
constexpr double WHERE_W    = 130.;
constexpr double WHERE_H    = 96.;
constexpr double WHERE_ZOOM = 1.35;
constexpr double HOLE_X     = 84.; // the hole of the part seen from above: its center and radius
constexpr double HOLE_Y     = 48.;
constexpr double HOLE_R     = 12.;
constexpr double ANGLE_W    = 250.;
constexpr double ANGLE_H    = 150.;
constexpr double CORNER_X   = 125.;
constexpr double CORNER_Y   = 82.;
constexpr double CORNER_ARM = 70.;
// The arrow putting a value back to its default.
constexpr double RESET_SIZE = 16.;
// The hint at the right of the play button: its width (wrapped to it) and the height of three lines.
constexpr double CAPTION_W  = 380.;
constexpr double CAPTION_H  = 50.;
constexpr double ANIMATION_SECONDS = 6.5;
constexpr int    ID_ANIMATION = wxID_HIGHEST + 1;
constexpr int    ID_TIPS      = wxID_HIGHEST + 2;

// Orio : accent du thème (bleu en clair, ORION en sombre) à la place du vert de Bambu.
wxColour accent() { return wxGetApp().dark_mode() ? wxColour(0, 194, 255) : wxColour(8, 119, 232); }

using Points = std::vector<wxPoint2DDouble>;

wxColour mix(const wxColour &a, const wxColour &b, double t, unsigned char alpha = 255)
{
    auto c = [t](unsigned char x, unsigned char y) { return (unsigned char) std::lround(x + (y - x) * t); };
    return wxColour(c(a.Red(), b.Red()), c(a.Green(), b.Green()), c(a.Blue(), b.Blue()), alpha);
}

double luminance(const wxColour &c) { return (0.299 * c.Red() + 0.587 * c.Green() + 0.114 * c.Blue()) / 255.; }

// The filament color, darkened or lightened when too close to the background (a white filament on a light theme).
wxColour visible(const wxColour &c, const wxColour &bg)
{
    const double lb = luminance(bg);
    return std::abs(luminance(c) - lb) >= 0.18 ? c : mix(c, lb > 0.5 ? *wxBLACK : *wxWHITE, 0.35);
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

// The mm shown along the wall: 30, or more for a longer slope.
double domain_for(double length) { return std::max(30., std::ceil(length * 1.25 / 10.) * 10.); }

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

void stroke_line(wxGraphicsContext &gc, double x1, double y1, double x2, double y2, const wxColour &colour, double width)
{
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour, width).Cap(wxCAP_ROUND)));
    gc.StrokeLine(x1, y1, x2, y2);
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

// A round handle, with arrows telling it moves left and right.
void draw_handle(wxGraphicsContext &gc, double x, double y, double r, const wxColour &bg, bool arrows)
{
    gc.SetBrush(wxBrush(bg));
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(accent(), 2.)));
    gc.DrawEllipse(x - r, y - r, 2. * r, 2. * r);
    if (arrows) {
        const double a = r * 0.72;
        fill_polygon(gc, {{x - a, y}, {x - a + 4., y - 3.5}, {x - a + 4., y + 3.5}}, accent(), accent());
        fill_polygon(gc, {{x + a, y}, {x + a - 4., y - 3.5}, {x + a - 4., y + 3.5}}, accent(), accent());
    }
}

// The nozzle, its tip at (x, y); the arrows tell it is dragged up and down.
void draw_nozzle(wxGraphicsContext &gc, double x, double y, bool arrows)
{
    const wxColour block(136, 135, 128), cone(180, 178, 169), tip(95, 94, 90);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(block));
    gc.DrawRoundedRectangle(x - 18., y - 62., 36., 22., 3.);
    fill_polygon(gc, {{x - 11., y - 40.}, {x + 11., y - 40.}, {x + 4., y - 5.}, {x - 4., y - 5.}}, cone, block);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(tip));
    gc.DrawRectangle(x - 4., y - 5., 8., 5.);
    if (arrows) {
        fill_polygon(gc, {{x + 26., y - 61.}, {x + 31., y - 54.}, {x + 21., y - 54.}}, accent(), accent());
        fill_polygon(gc, {{x + 26., y - 41.}, {x + 31., y - 48.}, {x + 21., y - 48.}}, accent(), accent());
    }
}

} // namespace

ScarfSeamDialog::ScarfSeamDialog(wxWindow *parent, Tab *tab)
    : DPIDialog(parent, wxID_ANY, _L("Scarf seams"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_tab(tab)
{
    SetBackgroundColour(*wxWHITE);
    read_values();
    if (m_filament_mode)
        SetTitle(wxString::Format(_L("Scarf seams of the filament \"%s\""), m_filament_name));
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

ScarfSeamDialog::~ScarfSeamDialog()
{
    m_timer.Stop();
    m_tip_timer.Stop();
}

const std::vector<std::string> &ScarfSeamDialog::process_keys()
{
    static const std::vector<std::string> keys = {"seam_slope_type", "seam_slope_start_height", "seam_slope_gap", "seam_slope_min_length",
                                                  "seam_slope_inner_walls", "seam_slope_entire_loop", "seam_slope_steps",
                                                  "seam_slope_conditional", "scarf_angle_threshold"};
    return keys;
}

const std::vector<std::string> &ScarfSeamDialog::filament_keys()
{
    static const std::vector<std::string> keys = {"filament_scarf_seam_type", "filament_scarf_height", "filament_scarf_gap", "filament_scarf_length"};
    return keys;
}

Tab *ScarfSeamDialog::process_tab() const { return m_filament_mode ? wxGetApp().get_tab(Preset::TYPE_PRINT) : m_tab; }

void ScarfSeamDialog::read_values()
{
    PresetBundle &bundle    = *wxGetApp().preset_bundle;
    auto         *model_tab = dynamic_cast<TabPrintModel *>(m_tab);
    m_filament_mode = m_tab->type() == Preset::TYPE_FILAMENT;
    // The process values: those of this tab, or of the global process when the filament is edited.
    Tab                      *process = process_tab();
    const DynamicPrintConfig &config  = process != nullptr ? *process->get_config() : bundle.prints.get_edited_preset().config;
    if (config.has("layer_height") && config.opt_float("layer_height") > 0.)
        m_layer_height = config.opt_float("layer_height");

    // The filament: the one edited, or the one of the object (the first one for the process), for its color and its scarf.
    const DynamicPrintConfig  full     = bundle.full_config();
    const DynamicPrintConfig &source   = m_filament_mode ? *m_tab->get_config() : full;
    size_t                    filament = 0;
    if (model_tab != nullptr && !model_tab->get_object_configs().empty()) {
        const DynamicPrintConfig &object = model_tab->get_object_configs().begin()->second->get();
        if (object.has("extruder") && object.opt_int("extruder") > 0)
            filament = size_t(object.opt_int("extruder") - 1);
    }
    if (auto *nozzle = full.option<ConfigOptionFloatsNullable>("nozzle_diameter"); nozzle != nullptr && !nozzle->values.empty() && nozzle->get_at(0) > 0.)
        m_nozzle = nozzle->get_at(0);
    if (auto *colours = source.option<ConfigOptionStrings>("filament_colour"); colours != nullptr && filament < colours->values.size())
        m_color = wxColour(colours->values[filament]);
    if (!m_color.IsOk())
        m_color = wxColour(239, 159, 39);
    if (m_filament_mode)
        m_filament_name = wxString::FromUTF8(bundle.filaments.get_edited_preset().name);
    else if (filament < bundle.filament_presets.size())
        m_filament_name = wxString::FromUTF8(bundle.filament_presets[filament]);

    // The type, the start, the gap and the length: those of the filament from the filament (used by its objects when its "Use these
    // settings" box is checked), else those of the process / object.
    m_v      = values_of(config, m_filament_mode ? &source : nullptr, filament);
    m_loaded = m_v;
    // The defaults: the system profile (or the saved one without system profile) of the process and of the filament; for objects,
    // what they inherit (the global process, or the object for a part).
    auto reference = [](PresetCollection &presets) -> const DynamicPrintConfig & {
        const Preset *parent = presets.get_selected_preset_parent();
        return parent != nullptr ? parent->config : presets.get_selected_preset().config;
    };
    if (model_tab != nullptr)
        m_default = values_of(model_tab->inherited_config(), nullptr, 0);
    else
        m_default = values_of(reference(bundle.prints), m_filament_mode ? &reference(bundle.filaments) : nullptr, 0);
    m_domain = domain_for(m_v.length);
}

ScarfSeamDialog::Values ScarfSeamDialog::values_of(const DynamicPrintConfig &process, const DynamicPrintConfig *filament, size_t index) const
{
    // Start height: mm or % of the layer height; gap: mm or % of the nozzle diameter (as GCode::extrude_loop() reads them).
    auto   ratio_of = [this](double value, bool percent) { return std::clamp(percent ? value / 100. : value / m_layer_height, 0., 0.95); };
    auto   gap_of   = [this](double value, bool percent) { return percent ? value / 100. * m_nozzle : value; };
    Values v;
    if (filament != nullptr) {
        if (auto *type = filament->option<ConfigOptionEnumsGeneric>("filament_scarf_seam_type"); type != nullptr && !type->values.empty())
            v.type = std::clamp(type->get_at(index), 0, 2);
        if (auto *height = filament->option<ConfigOptionFloatsOrPercents>("filament_scarf_height"); height != nullptr && !height->values.empty())
            v.start = ratio_of(height->get_at(index).value, height->get_at(index).percent);
        if (auto *gap = filament->option<ConfigOptionFloatsOrPercents>("filament_scarf_gap"); gap != nullptr && !gap->values.empty())
            v.gap = gap_of(gap->get_at(index).value, gap->get_at(index).percent);
        if (auto *length = filament->option<ConfigOptionFloats>("filament_scarf_length"); length != nullptr && !length->values.empty())
            v.length = length->get_at(index);
    } else {
        if (process.has("seam_slope_type"))
            v.type = std::clamp(int(process.opt_enum<SeamScarfType>("seam_slope_type")), 0, 2);
        if (const auto *start = process.option<ConfigOptionFloatOrPercent>("seam_slope_start_height"); start != nullptr)
            v.start = ratio_of(start->value, start->percent);
        if (const auto *gap = process.option<ConfigOptionFloatOrPercent>("seam_slope_gap"); gap != nullptr)
            v.gap = gap_of(gap->value, gap->percent);
        if (process.has("seam_slope_min_length"))
            v.length = process.opt_float("seam_slope_min_length");
    }
    if (process.has("seam_slope_inner_walls"))
        v.inner = process.opt_bool("seam_slope_inner_walls");
    if (process.has("seam_slope_entire_loop"))
        v.entire = process.opt_bool("seam_slope_entire_loop");
    if (process.has("seam_slope_steps"))
        v.steps = std::max(1, process.opt_int("seam_slope_steps"));
    if (process.has("seam_slope_conditional"))
        v.smart = process.opt_bool("seam_slope_conditional");
    if (process.has("scarf_angle_threshold"))
        v.angle = process.opt_int("scarf_angle_threshold");
    return v;
}

bool ScarfSeamDialog::editable(Tab *tab, const std::string &key) const
{
    if (tab == nullptr || !tab->get_config()->has(key))
        return false;
    auto *model_tab = dynamic_cast<TabPrintModel *>(tab);
    // A part or a modifier has no object option (the angle threshold).
    return model_tab == nullptr || model_tab->has_key(key);
}

void ScarfSeamDialog::apply()
{
    struct Change
    {
        Tab                          *tab;
        std::string                   key;
        std::unique_ptr<ConfigOption> value;
    };
    std::vector<Change> changes;
    auto add     = [&changes](Tab *tab, const char *key, ConfigOption *opt) { changes.push_back({tab, key, std::unique_ptr<ConfigOption>(opt)}); };
    auto changed = [](double a, double b) { return std::abs(a - b) > 1e-6; };
    // The settings of the process (from the filament too: they are common to all the filaments).
    if (Tab *process = process_tab(); process != nullptr) {
        add(process, "seam_slope_inner_walls", new ConfigOptionBool(m_v.inner));
        add(process, "seam_slope_entire_loop", new ConfigOptionBool(m_v.entire));
        add(process, "seam_slope_steps", new ConfigOptionInt(m_v.steps));
        add(process, "seam_slope_conditional", new ConfigOptionBool(m_v.smart));
        add(process, "scarf_angle_threshold", new ConfigOptionInt(m_v.angle));
    }
    if (m_filament_mode) {
        // The four vectors of the filament, all their values set; written when changed, to keep how they were given (mm or %).
        DynamicPrintConfig &config    = *m_tab->get_config();
        auto                vector_of = [&config](const char *key) { return config.option(key)->clone(); };
        if (m_v.type != m_loaded.type) {
            auto *opt = static_cast<ConfigOptionEnumsGeneric *>(vector_of("filament_scarf_seam_type"));
            std::fill(opt->values.begin(), opt->values.end(), m_v.type);
            add(m_tab, "filament_scarf_seam_type", opt);
        }
        if (changed(m_v.start, m_loaded.start)) {
            auto *opt = static_cast<ConfigOptionFloatsOrPercents *>(vector_of("filament_scarf_height"));
            std::fill(opt->values.begin(), opt->values.end(), FloatOrPercent(std::round(m_v.start * 100.), true));
            add(m_tab, "filament_scarf_height", opt);
        }
        if (changed(m_v.gap, m_loaded.gap)) {
            auto *opt = static_cast<ConfigOptionFloatsOrPercents *>(vector_of("filament_scarf_gap"));
            std::fill(opt->values.begin(), opt->values.end(), FloatOrPercent(m_v.gap, false));
            add(m_tab, "filament_scarf_gap", opt);
        }
        if (changed(m_v.length, m_loaded.length)) {
            auto *opt = static_cast<ConfigOptionFloats *>(vector_of("filament_scarf_length"));
            std::fill(opt->values.begin(), opt->values.end(), m_v.length);
            add(m_tab, "filament_scarf_length", opt);
        }
    } else {
        // Written when changed, to keep how they were given (mm or %).
        if (m_v.type != m_loaded.type)
            add(m_tab, "seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType(m_v.type)));
        if (changed(m_v.start, m_loaded.start))
            add(m_tab, "seam_slope_start_height", new ConfigOptionFloatOrPercent(std::round(m_v.start * 100.), true));
        if (changed(m_v.gap, m_loaded.gap))
            add(m_tab, "seam_slope_gap", new ConfigOptionFloatOrPercent(m_v.gap, false));
        if (changed(m_v.length, m_loaded.length))
            add(m_tab, "seam_slope_min_length", new ConfigOptionFloat(m_v.length));
    }
    // The objects (parts, height ranges): the changed values written into them at once, those back to the inherited value removed
    // from them (TabPrintModel::set_object_options()), a single undo step.
    if (auto *model_tab = dynamic_cast<TabPrintModel *>(m_tab); model_tab != nullptr) {
        DynamicPrintConfig values;
        for (Change &change : changes)
            if (editable(change.tab, change.key) && !(*change.tab->get_config()->option(change.key) == *change.value))
                values.set_key_value(change.key, change.value.release());
        if (!values.empty()) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Scarf seams");
            model_tab->set_object_options(values);
        }
        return;
    }
    // As if each value was typed in its field.
    for (Change &change : changes) {
        if (!editable(change.tab, change.key))
            continue;
        DynamicPrintConfig &config = *change.tab->get_config();
        if (*config.option(change.key) == *change.value)
            continue;
        config.set_key_value(change.key, change.value.release());
        change.tab->update_dirty();
        change.tab->on_value_change(change.key, boost::any());
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Associate with a filament

void ScarfSeamDialog::set_filament_scarf(DynamicPrintConfig &config) const
{
    // All the values of the vectors (a filament preset has one, or one per extruder variant).
    if (auto *opt = config.option<ConfigOptionEnumsGeneric>("filament_scarf_seam_type"); opt != nullptr)
        std::fill(opt->values.begin(), opt->values.end(), m_v.type);
    if (auto *opt = config.option<ConfigOptionFloatsOrPercents>("filament_scarf_height"); opt != nullptr)
        std::fill(opt->values.begin(), opt->values.end(), FloatOrPercent(std::round(m_v.start * 100.), true));
    if (auto *opt = config.option<ConfigOptionFloatsOrPercents>("filament_scarf_gap"); opt != nullptr)
        std::fill(opt->values.begin(), opt->values.end(), FloatOrPercent(m_v.gap, false));
    if (auto *opt = config.option<ConfigOptionFloats>("filament_scarf_length"); opt != nullptr)
        std::fill(opt->values.begin(), opt->values.end(), m_v.length);
    auto *use = config.option<ConfigOptionBools>("filament_scarf_use_settings", true);
    if (use->values.empty())
        use->values.push_back(1);
    std::fill(use->values.begin(), use->values.end(), (unsigned char) 1);
}

// A small window in the middle of the screen: the filaments of the project, each preset once (several filaments may use it).
void ScarfSeamDialog::choose_filaments()
{
    PresetBundle             &bundle  = *wxGetApp().preset_bundle;
    const DynamicPrintConfig  full    = bundle.full_config();
    const auto               *colours = full.option<ConfigOptionStrings>("filament_colour");
    std::vector<std::string>         names;
    std::vector<std::vector<size_t>> slots;
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
        auto it = std::find(names.begin(), names.end(), bundle.filament_presets[i]);
        if (it == names.end()) {
            names.push_back(bundle.filament_presets[i]);
            slots.push_back({i});
        } else
            slots[it - names.begin()].push_back(i);
    }

    wxDialog dlg(this, wxID_ANY, _L("Associate with a filament"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
    dlg.SetBackgroundColour(*wxWHITE);
    const int pad   = FromDIP(16);
    auto     *sizer = new wxBoxSizer(wxVERTICAL);
    auto     *hint  = new Label(&dlg, Label::Body_13, _L("Check the filaments of the project that take these scarf seams:"));
    sizer->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad);
    std::vector<::CheckBox *> checks;
    for (size_t k = 0; k < names.size(); ++k) {
        auto *row   = new wxBoxSizer(wxHORIZONTAL);
        auto *check = new ::CheckBox(&dlg);
        checks.push_back(check);
        // Its color, as in the list of the filaments.
        wxColour colour = colours != nullptr && slots[k].front() < colours->values.size() ? wxColour(colours->values[slots[k].front()]) : wxColour();
        auto    *swatch = new wxPanel(&dlg, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(14), FromDIP(14)));
        swatch->SetBackgroundColour(colour.IsOk() ? colour : wxColour(239, 159, 39));
        wxString numbers;
        for (size_t slot : slots[k])
            numbers += (numbers.empty() ? "" : ", ") + wxString::Format("%d", int(slot + 1));
        auto *label = new Label(&dlg, Label::Body_13, wxString::Format(_L("Filament %s: %s"), numbers, wxString::FromUTF8(names[k])));
        label->Bind(wxEVT_LEFT_DOWN, [check](wxMouseEvent &) { check->SetValue(!check->GetValue()); });
        row->Add(check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        row->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
    }
    auto   *buttons = new wxBoxSizer(wxHORIZONTAL);
    Button *ok      = make_button(_L("OK"), true, &dlg);
    Button *cancel  = make_button(_L("Cancel"), false, &dlg);
    ok->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_OK); });
    cancel->Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &) { dlg.EndModal(wxID_CANCEL); });
    buttons->AddStretchSpacer();
    buttons->Add(ok, 0, wxRIGHT, FromDIP(8));
    buttons->Add(cancel, 0);
    sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
    dlg.SetSizerAndFit(sizer);
    wxGetApp().UpdateDlgDarkUI(&dlg);
    dlg.CentreOnScreen();
    if (dlg.ShowModal() != wxID_OK)
        return;
    std::vector<std::string> chosen;
    for (size_t k = 0; k < names.size(); ++k)
        if (checks[k]->GetValue())
            chosen.push_back(names[k]);
    if (!chosen.empty())
        associate(chosen);
}

// Each preset saved with these settings, as Tab::save_preset() and CalibrationWizard::save_preset() do. A Bambu (system), default
// or project filament is not overwritten: saved as a copy "<name> - scarf", which takes its place in the project.
void ScarfSeamDialog::associate(const std::vector<std::string> &names)
{
    static const std::vector<std::string> scarf_keys = {"filament_scarf_seam_type", "filament_scarf_height", "filament_scarf_gap",
                                                        "filament_scarf_length", "filament_scarf_use_settings"};
    PresetBundle     &bundle    = *wxGetApp().preset_bundle;
    PresetCollection &filaments = bundle.filaments;
    // Saving a preset selects it: the filament of the Filament tab is selected again after, with its unsaved changes.
    std::string                    shown         = filaments.get_selected_preset_name();
    const DynamicPrintConfig       edited        = filaments.get_edited_preset().config;
    const std::vector<std::string> dirty         = filaments.current_dirty_options();
    bool                           shown_changed = false;
    std::vector<std::pair<std::string, std::string>> renamed;
    for (const std::string &name : names) {
        const Preset *preset = filaments.find_preset(name, false, true);
        if (preset == nullptr)
            continue;
        const bool        copy     = !preset->is_user() || preset->is_external;
        const std::string new_name = copy ? name + " - " + std::string(_L("scarf").utf8_str().data()) : name;
        const bool        existed  = filaments.find_preset(new_name, false, true) != nullptr;
        Preset            changed  = *preset;
        set_filament_scarf(changed.config);
        filaments.save_current_preset(new_name, false, false, &changed);
        Preset *saved = filaments.find_preset(new_name, false, true);
        if (saved == nullptr)
            continue;
        // For the sync of the presets.
        saved->sync_info = existed ? "update" : "create";
        if (!existed && wxGetApp().is_user_login() && wxGetApp().getAgent() != nullptr)
            saved->user_id = wxGetApp().getAgent()->get_user_id();
        saved->save_info();
        if (new_name != name)
            renamed.emplace_back(name, new_name);
        if (name == shown) {
            shown         = new_name;
            shown_changed = true;
        }
    }
    filaments.select_preset_by_name(shown, true);
    DynamicPrintConfig &now = filaments.get_edited_preset().config;
    for (const std::string &key : dirty) {
        const bool scarf = std::find(scarf_keys.begin(), scarf_keys.end(), key) != scarf_keys.end();
        if (edited.has(key) && key != "inherits" && key != "filament_settings_id" && !(shown_changed && scarf))
            now.set_key_value(key, edited.option(key)->clone());
    }
    bundle.update_compatible(PresetSelectCompatibleType::Never);
    // The filaments of the project use the copies.
    for (const auto &[from, to] : renamed)
        wxGetApp().plater()->sidebar().update_presets_from_to(Preset::TYPE_FILAMENT, from, to);
    if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT); tab != nullptr) {
        tab->load_current_preset();
        tab->update_dirty();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The window

Button *ScarfSeamDialog::make_button(const wxString &text, bool primary, wxWindow *parent)
{
    auto *btn = new Button(parent != nullptr ? parent : this, text);
    style_button(btn, primary);
    btn->SetFont(Label::Body_12);
    btn->SetMinSize(wxSize(FromDIP(84), FromDIP(26)));
    btn->SetCornerRadius(FromDIP(12));
    return btn;
}

void ScarfSeamDialog::style_button(Button *btn, bool primary)
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
        btn->SetTextColor(StateColor(std::pair<wxColour, int>(text_colour(), StateColor::Normal)));
    }
    btn->Refresh();
}

wxPanel *ScarfSeamDialog::make_canvas(double logical_width, double logical_height, std::function<void(wxGraphicsContext &, double)> paint, double zoom)
{
    auto *panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                              wxSize(FromDIP(int(logical_width * zoom)), FromDIP(int(logical_height * zoom))));
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

wxWindow *ScarfSeamDialog::make_reset(std::function<bool()> modified, std::function<void()> reset)
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

wxWindow *ScarfSeamDialog::make_help(const std::string &key)
{
    auto *help = new ScalableButton(this, wxID_ANY, "icon_qusetion", wxEmptyString, wxDefaultSize, wxDefaultPosition,
                                    wxBU_EXACTFIT | wxNO_BORDER, true);
    m_tips.emplace_back(help, key);
    return help;
}

Label *ScarfSeamDialog::make_label(const wxString &text, const std::string &key, bool title)
{
    auto *label = new Label(this, title ? Label::Head_13 : Label::Body_13, text);
    if (!key.empty())
        m_tips.emplace_back(label, key);
    return label;
}

// The title of a column, its help next to it.
wxSizer *ScarfSeamDialog::make_title(const wxString &text, const std::string &key)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(make_label(text, key, true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    row->Add(make_help(key), 0, wxALIGN_CENTER_VERTICAL);
    return row;
}

wxSizer *ScarfSeamDialog::make_check(const wxString &text, const std::string &key, std::function<void(bool)> set, ::CheckBox *&check)
{
    auto *row = new wxBoxSizer(wxHORIZONTAL);
    auto *box = new ::CheckBox(this);
    check     = box;
    box->Bind(wxEVT_TOGGLEBUTTON, [this, box, set](wxCommandEvent &evt) {
        set(box->GetValue());
        touched();
        refresh();
        evt.Skip();
    });
    Label *label = make_label(text, key);
    label->Bind(wxEVT_LEFT_DOWN, [this, box, set](wxMouseEvent &) {
        box->SetValue(!box->GetValue());
        set(box->GetValue());
        touched();
        refresh();
    });
    row->Add(box, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    row->Add(make_help(key), 0, wxALIGN_CENTER_VERTICAL);
    return row;
}

// A row of a column: the name on the left; on the right its help, a short field and its unit (all the fields of a column in
// line, their helps too).
wxSizer *ScarfSeamDialog::make_field(const wxString &text, const std::string &key, const wxString &unit, std::function<void(double)> set, wxTextCtrl *&field)
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
    auto *unit_label = new Label(this, Label::Body_13, unit);
    unit_label->SetMinSize(wxSize(FromDIP(22), -1));
    row->Add(unit_label, 0, wxALIGN_CENTER_VERTICAL);
    return row;
}

void ScarfSeamDialog::build()
{
    const int pad  = FromDIP(16);
    auto     *main = new wxBoxSizer(wxVERTICAL);

    // The three columns side by side, aligned at the top, thin lines between them. Each one is a box of rows of the same height
    // (the rows aligned from a column to the other): the title, the settings, then the drawing under them.
    const int  row_h     = FromDIP(30);
    const bool has_angle = editable(process_tab(), "scarf_angle_threshold");
    auto      *columns   = new wxBoxSizer(wxHORIZONTAL);
    auto       column    = [this, columns](bool first) {
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
    auto drawing = [this](wxBoxSizer *col, wxWindow *canvas) { col->Add(canvas, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(4)); };
    // The arrow at the end of a row (after a stretch for the titles and the boxes, so that the arrows of a column are in line).
    auto with_reset = [this](wxSizer *line, bool stretch, std::function<bool()> modified, std::function<void()> reset) {
        if (stretch)
            line->AddStretchSpacer();
        line->Add(make_reset(std::move(modified), std::move(reset)), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
        return line;
    };
    auto differs = [](double a, double b) { return std::abs(a - b) > 1e-4; };

    // Where: the three choices, the inner walls, then seen from above.
    wxBoxSizer *where = column(true);
    row(where, with_reset(make_title(_L("Where"), "where"), true, [this]() { return m_v.type != m_default.type; },
                          [this]() { m_v.type = m_default.type; }));
    const wxString types[3] = {_L("Nowhere"), _L("Contours"), _L("Contours and holes")};
    for (int i = 0; i < 3; ++i) {
        m_type_btn[i] = make_button(types[i], false);
        m_type_btn[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) {
            if (m_v.type == i)
                return;
            m_v.type = i;
            touched();
            refresh();
        });
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        line->Add(m_type_btn[i], 1, wxALIGN_CENTER_VERTICAL);
        row(where, line);
    }
    row(where, with_reset(make_check(_L("Inner walls too"), "inner", [this](bool v) { m_v.inner = v; }, m_inner), true,
                          [this]() { return m_v.inner != m_default.inner; }, [this]() { m_v.inner = m_default.inner; }));
    m_where = make_canvas(WHERE_W, WHERE_H, [this](wxGraphicsContext &gc, double scale) { paint_where(gc, scale); }, WHERE_ZOOM);
    m_tips.emplace_back(m_where, "where");
    drawing(where, m_where);

    // The slope: its drawing is the side view, under the columns.
    wxBoxSizer *slope = column(false);
    row(slope, make_title(_L("Slope"), "p1"));
    auto set_length = [this](double v) {
        m_v.length = std::clamp(v, 0.5, 200.);
        m_v.gap    = std::min(m_v.gap, m_v.length / 2.);
        m_domain   = domain_for(m_v.length);
    };
    row(slope, with_reset(make_field(_L("Slope length"), "length", "mm", set_length, m_length), false,
                          [this, differs]() { return differs(m_v.length, m_default.length); }, [this, set_length]() { set_length(m_default.length); }));
    row(slope, with_reset(make_field(_L("Nozzle start"), "start", "%", [this](double v) { m_v.start = std::clamp(v, 0., 95.) / 100.; }, m_start), false,
                          [this]() { return std::lround(m_v.start * 100.) != std::lround(m_default.start * 100.); },
                          [this]() { m_v.start = m_default.start; }));
    row(slope, with_reset(make_field(_L("Gap at the ends"), "gap_field", "mm", [this](double v) { m_v.gap = std::clamp(v, 0., ramp_length() / 2.); }, m_gap),
                          false, [this, differs]() { return differs(m_v.gap, m_default.gap); },
                          [this]() { m_v.gap = std::min(m_default.gap, ramp_length() / 2.); }));
    row(slope, with_reset(make_field(_L("Slope smoothness"), "steps", wxEmptyString, [this](double v) { m_v.steps = std::clamp(int(std::lround(v)), 1, 100); },
                                     m_steps),
                          false, [this]() { return m_v.steps != m_default.steps; }, [this]() { m_v.steps = m_default.steps; }));
    row(slope, with_reset(make_check(_L("Slope all around the wall"), "entire", [this](bool v) { m_v.entire = v; }, m_entire), true,
                          [this]() { return m_v.entire != m_default.entire; }, [this]() { m_v.entire = m_default.entire; }));

    // The angle: the field, "only on smooth walls", then the corner whose arm is turned with its examples.
    wxBoxSizer *angle = column(false);
    row(angle, make_title(_L("Angle"), "angle"));
    // A part or a modifier has no angle threshold of its own (an object option).
    if (has_angle)
        row(angle, with_reset(make_field(_L("Corners wider than"), "angle", L"°", [this](double v) { m_v.angle = std::clamp(int(std::lround(v)), 0, 180); },
                                         m_angle_input),
                              false, [this]() { return m_v.angle != m_default.angle; }, [this]() { m_v.angle = m_default.angle; }));
    row(angle, with_reset(make_check(_L("Scarf only on smooth walls"), "smart", [this](bool v) { m_v.smart = v; }, m_smart), true,
                          [this]() { return m_v.smart != m_default.smart; }, [this]() { m_v.smart = m_default.smart; }));
    if (has_angle) {
        m_angle = make_canvas(ANGLE_W, ANGLE_H, [this](wxGraphicsContext &gc, double scale) { paint_angle(gc, scale); });
        m_tips.emplace_back(m_angle, "angle");
        m_angle->Bind(wxEVT_LEFT_DOWN, &ScarfSeamDialog::on_angle_mouse, this);
        m_angle->Bind(wxEVT_LEFT_UP, &ScarfSeamDialog::on_angle_mouse, this);
        m_angle->Bind(wxEVT_MOTION, &ScarfSeamDialog::on_angle_mouse, this);
        m_angle->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
            m_drag = Drag::None;
            resume_tips_later();
        });
        drawing(angle, m_angle);
    }
    main->Add(columns, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP, pad);

    // Under them, the wall seen from the side.
    // Closer to the columns (more room between the hint and the buttons of the window).
    main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_side = make_canvas(SIDE_W, SIDE_H, [this](wxGraphicsContext &gc, double scale) { paint_side(gc, scale); });
    m_side->Bind(wxEVT_LEFT_DOWN, &ScarfSeamDialog::on_side_mouse, this);
    m_side->Bind(wxEVT_LEFT_UP, &ScarfSeamDialog::on_side_mouse, this);
    m_side->Bind(wxEVT_MOTION, &ScarfSeamDialog::on_side_mouse, this);
    m_side->Bind(wxEVT_LEAVE_WINDOW, &ScarfSeamDialog::on_side_mouse, this);
    m_side->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
        m_drag = Drag::None;
        resume_tips_later();
    });
    main->Add(m_side, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    // Under its legend, centered, the play button and at its right the hint or the step of the animation, painted in a box of
    // fixed size: wrapped to its width, left aligned, its lines centered on the height of the button (a native text stays at
    // the top of its box), it neither grows over the buttons of the window nor moves when the text changes.
    auto *play = new wxBoxSizer(wxHORIZONTAL);
    m_play     = new Button(this, wxEmptyString, "media_play", wxBORDER_NONE, 26);
    m_tips.emplace_back(m_play, "play");
    m_play->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { start_animation(); });
    m_caption = make_canvas(CAPTION_W, CAPTION_H, [this](wxGraphicsContext &gc, double scale) {
        gc.SetFont(Label::Body_12, text_colour());
        const double          width = CAPTION_W * scale;
        const wxArrayString   words = wxSplit(m_shown_caption, ' ', '\0');
        std::vector<wxString> lines;
        wxString              line;
        for (size_t i = 0; i < words.size(); ++i) {
            const wxString longer = line.empty() ? words[i] : line + " " + words[i];
            double         w = 0., h = 0.;
            gc.GetTextExtent(longer, &w, &h);
            if (w > width && !line.empty()) {
                lines.push_back(line);
                line = words[i];
            } else
                line = longer;
        }
        if (!line.empty())
            lines.push_back(line);
        double w = 0., line_h = 0.;
        gc.GetTextExtent("Ag", &w, &line_h);
        double y = (CAPTION_H * scale - line_h * lines.size()) / 2.;
        for (const wxString &text : lines) {
            gc.DrawText(text, 0., y);
            y += line_h;
        }
    });
    play->Add(m_play, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    play->Add(m_caption, 0, wxALIGN_CENTER_VERTICAL);
    main->Add(play, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP, FromDIP(2));

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    Button *ok        = make_button(_L("OK"), true);
    Button *link      = make_button(_L("Associate with a filament"), false);
    Button *cancel    = make_button(_L("Cancel"), false);
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_timer.Stop();
        apply();
        EndModal(wxID_OK);
    });
    link->SetToolTip(_L("Copies the type, the nozzle start, the gap and the length of these scarf seams into the filaments you check, "
                        "checks their \"Use these settings\" box and saves them at once. A Bambu filament is saved as a copy, which "
                        "takes its place in the project."));
    link->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_filaments(); });
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    buttons->Add(ok, 0, wxRIGHT, FromDIP(8));
    buttons->Add(link, 0, wxRIGHT, FromDIP(8));
    buttons->Add(cancel, 0);
    main->Add(buttons, 0, wxEXPAND | wxALL, pad);

    SetSizer(main);
    main->SetSizeHints(this);
    Layout();
}

void ScarfSeamDialog::touched()
{
    m_phase = 0;
    if (m_animating) {
        m_timer.Stop();
        m_animating = false;
    }
}

void ScarfSeamDialog::refresh()
{
    m_updating = true;
    // Not the field being typed in: it keeps the text as typed.
    auto set_text = [](wxTextCtrl *ctrl, const wxString &text) {
        if (ctrl != nullptr && wxWindow::FindFocus() != ctrl && ctrl->GetValue() != text)
            ctrl->ChangeValue(text);
    };
    auto enable = [](wxWindow *window, bool on) {
        if (window != nullptr && window->IsEnabled() != on)
            window->Enable(on);
    };
    auto check = [](::CheckBox *box, bool on) {
        if (box != nullptr && box->GetValue() != on)
            box->SetValue(on);
    };
    set_text(m_length, num(m_v.length));
    set_text(m_start, num(std::round(m_v.start * 100.)));
    set_text(m_gap, num(m_v.gap));
    set_text(m_steps, wxString::Format("%d", m_v.steps));
    set_text(m_angle_input, wxString::Format("%d", m_v.angle));
    enable(m_length, !m_v.entire);
    enable(m_angle_input, m_v.smart);
    check(m_inner, m_v.inner);
    check(m_entire, m_v.entire);
    check(m_smart, m_v.smart);
    if (m_styled_type != m_v.type) {
        m_styled_type = m_v.type;
        for (int i = 0; i < 3; ++i)
            style_button(m_type_btn[i], m_v.type == i);
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

    // The tooltips, but those of the drawings while a handle is dragged (they flickered).
    const bool paused = tips_paused();
    for (auto &[window, key] : m_tips) {
        if (paused && window == m_angle)
            continue;
        if (const wxString text = tip(key); window->GetToolTipText() != text)
            window->SetToolTip(text);
    }
    if (!paused && !m_side_tip.empty())
        if (const wxString text = tip(m_side_tip); m_side->GetToolTipText() != text)
            m_side->SetToolTip(text);

    // The drawings: the side view follows every value, the two others only some.
    m_side->Refresh();
    if (const int state = m_v.type * 2 + int(m_v.inner); state != m_where_state) {
        m_where_state = state;
        m_where->Refresh();
    }
    if (const int state = m_v.angle * 2 + int(m_v.smart); m_angle != nullptr && state != m_angle_state) {
        m_angle_state = state;
        m_angle->Refresh();
    }
    m_updating = false;
}

void ScarfSeamDialog::show_caption()
{
    if (const wxString text = caption(); text != m_shown_caption) {
        m_shown_caption = text;
        m_caption->Refresh();
    }
}

bool ScarfSeamDialog::tips_paused() const { return m_drag != Drag::None || std::chrono::steady_clock::now() < m_tips_resume; }

void ScarfSeamDialog::pause_tips()
{
    m_tip_timer.Stop();
    m_side_tip.clear();
    m_side->UnsetToolTip();
    if (m_angle != nullptr)
        m_angle->UnsetToolTip();
}

void ScarfSeamDialog::resume_tips_later()
{
    m_tips_resume = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    m_tip_timer.StartOnce(2000);
}

void ScarfSeamDialog::resume_tips()
{
    if (m_drag != Drag::None)
        return;
    m_tips_resume = std::chrono::steady_clock::time_point();
    if (m_angle != nullptr)
        m_angle->SetToolTip(tip("angle"));
    // The tooltip of what is under the mouse, if it is over the side view.
    const wxPoint p = m_side->ScreenToClient(wxGetMousePosition());
    if (!m_animating && m_side->GetClientRect().Contains(p)) {
        const double scale = m_side->GetClientSize().x / SIDE_W;
        m_side_tip         = side_region_at(p.x / scale, p.y / scale);
        if (!m_side_tip.empty())
            m_side->SetToolTip(tip(m_side_tip));
    }
}

wxString ScarfSeamDialog::caption() const
{
    if (m_v.type == 0)
        return _L("No scarf: the layer starts and ends flat, at the same place (classic seam).");
    switch (m_phase) {
    case 1: return wxString::Format(_L("1. The nozzle starts %s mm above the layer below and slowly rises: the bead gets thicker."), num(m_v.start * m_layer_height));
    case 2: return _L("2. It goes around the part at the normal height.");
    case 3: return _L("3. Back at the start, it goes again over the slope, laying less and less material.");
    case 4: return _L("4. Both passes fit into each other: no step, the seam blends into the wall.");
    default: return _L("Drag the nozzle, the mark above or the one below: the drawing shows the bead as it will be printed.");
    }
}

wxString ScarfSeamDialog::tip(const std::string &key) const
{
    const wxString start_mm = num(m_v.start * m_layer_height);
    const wxString percent  = wxString::Format("%d", int(std::lround(m_v.start * 100.)));
    // Opened from the filament: the settings of the process are told so.
    const bool     process_only = m_filament_mode && (key == "inner" || key == "entire" || key == "steps" || key == "steps_handle" ||
                                                  key == "smart" || key == "angle");
    const wxString process_note = process_only ? "\n" + _L("(A setting of the process, the same for all the filaments.)") : wxString();
    if (key == "play")
        return _L("Show the nozzle printing");
    if (key == "where")
        return _L("Contours: only the outer wall of the part. Contours and holes: the walls of the holes too.\n"
                  "Seen from above, the walls in the color of the filament get the scarf.");
    if (key == "inner")
        return _L("Also gives a scarf to the walls hidden behind the outer wall. They are not seen, but their seam may show through.") + process_note;
    if (key == "below")
        return wxString::Format(_L("Layers already printed, one bead each, their edges in a darker line. Heights are enlarged to be "
                                   "readable: a layer is %s mm thick."),
                                num(m_layer_height));
    if (key == "end")
        return _L("End of the loop: the nozzle comes back to the seam at the normal height, then goes on over the slope.");
    if (key == "p1")
        return wxString::Format(_L("Start of the layer. The nozzle starts %s mm above the layer below and rises to %s mm %s. "
                                   "The bead gets thicker as it goes: the layer starts with a gentle slope instead of a step."),
                                start_mm, num(m_layer_height),
                                m_v.entire ? _L("over the whole loop") : wxString::Format(_L("over %s mm"), num(m_v.length)));
    if (key == "p2")
        return _L("End of the layer. Back at the start, the nozzle stays up and goes again over the slope, laying less and less material. "
                  "Both passes fit into each other: the layer is complete, without bulge nor hole.");
    if (key == "gap")
        return wxString::Format(_L("Gap: %s mm left empty at the start of the bead and %s mm at its end, so that it does not crush itself."),
                                num(2. * m_v.gap), num(m_v.gap));
    if (key == "nozzle")
        return wxString::Format(_L("Drag the nozzle up or down: the height at which it starts the layer (%s %% of a layer, so %s mm)."), percent, start_mm);
    if (key == "length_handle")
        return _L("Drag left or right: the length over which the nozzle rises.");
    if (key == "gap_handle")
        return _L("Drag left or right: the small gap left at both ends of the bead.");
    if (key == "steps_handle")
        return wxString::Format(_L("Drag left or right: the smoothness of the slope, here %d steps."), m_v.steps) + process_note;
    if (key == "length")
        return _L("Distance over which the nozzle gently rises at the start of each layer. Longer, the seam is more spread out and less "
                  "visible. On a small part, the slope never goes beyond one loop.");
    if (key == "start")
        return wxString::Format(_L("Height of the nozzle when it starts the layer, in percent of the layer height. Here %s %% of %s mm = %s mm: "
                                   "the bead starts almost flat. 0 %%: the nozzle grazes the layer below. Always under 100 %%."),
                                percent, num(m_layer_height), start_mm);
    if (key == "gap_field")
        return _L("Length left empty where the start and the end of the bead meet: the start is shortened by twice this value, the end by once. "
                  "0 = no gap. Increase it if the seam makes a small bulge.");
    if (key == "steps")
        return _L("In how many steps the material increases along the slope. Few steps: they show on the bead. Many: a regular rise, "
                  "but a slightly longer G-code.") + process_note;
    if (key == "entire")
        return _L("The slope spreads over the whole length of the wall: the seam disappears completely. Ideal for vases and round parts.") +
               process_note;
    if (key == "smart")
        return _L("On a sharp corner, the classic seam already hides in the corner: the scarf brings nothing there. Checked, the scarf is only "
                  "used on rounded or almost straight walls, and never over an overhang.") + process_note;
    if (key == "angle")
        return wxString::Format(_L("Angle of the corner where the seam falls. 180%s = straight or rounded wall, 90%s = corner of a cube. "
                                   "Wider than %d%s: the wall is considered smooth and gets the scarf. Drag the arm of the corner to change it."),
                                L"°", L"°", m_v.angle, L"°") + process_note;
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------
// The side view

double ScarfSeamDialog::ramp_length() const { return m_v.entire ? m_domain : std::clamp(m_v.length, 0.5, m_domain); }
// The start of the slope is shortened by twice the gap, the end of the loop going over it by once (ExtrusionLoopSloped::clip_slope()).
double ScarfSeamDialog::slope_start() const { return std::min(2. * m_v.gap, ramp_length() * 0.9); }
double ScarfSeamDialog::slope_end() const { return std::max(0., ramp_length() - m_v.gap); }
// The nozzle rises steadily...
double ScarfSeamDialog::ratio_at(double mm) const { return std::min(1., m_v.start + (1. - m_v.start) * std::max(0., mm) / ramp_length()); }
// ...the flow by steps: each segment of the slope extrudes the ratio of its end (GCode::_extrude(), sloped paths).
double ScarfSeamDialog::step_ratio_at(double mm) const
{
    const double w = ramp_length() / m_v.steps;
    const int    i = std::clamp(int(std::ceil(mm / w - 1e-9)), 1, m_v.steps);
    return ratio_at(i * w);
}
double ScarfSeamDialog::px(double mm) const { return X0 + mm * (X1 - X0) / m_domain; }
double ScarfSeamDialog::steps_x() const
{
    return STEPS_X0 + (std::min(m_v.steps, STEPS_MAX) - 1) * (STEPS_X1 - STEPS_X0) / (STEPS_MAX - 1);
}

void ScarfSeamDialog::paint_side(wxGraphicsContext &gc, double scale)
{
    const wxColour bg      = GetBackgroundColour();
    const wxColour bead    = visible(m_color, bg);
    const wxColour bead2   = mix(bead, *wxBLACK, 0.3);
    const wxColour below   = mix(bead, bg, 0.45);
    const wxColour outline = mix(bead, *wxBLACK, 0.45);
    const double   L = ramp_length(), s = slope_start(), e = slope_end(), w = L / m_v.steps;
    auto           y_of = [](double ratio) { return Y_BASE - ratio * LAYER_H; };
    // During the animation, only what the nozzle printed so far.
    auto clip = [&gc](double x, double width) { gc.Clip(x, 0., std::max(0., width), SIDE_H); };

    gc.PushState();
    gc.Scale(scale, scale);

    // The layers already printed: one bead each, its edges the darker lines.
    gc.SetBrush(wxBrush(below));
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(mix(below, *wxBLACK, 0.25), 1.)));
    gc.DrawRectangle(XA, Y_BASE, X1 - XA, LAYER_H);
    gc.DrawRectangle(XA, Y_BASE + LAYER_H, X1 - XA, LAYER_H);

    // The end of the loop, coming to the seam at the normal height.
    clip(XA, m_animating ? m_clip0 : X0 - XA);
    fill_polygon(gc, {{XA, Y_TOP}, {X0, Y_TOP}, {X0, Y_BASE}, {XA, Y_BASE}}, bead, outline);
    gc.ResetClip();

    // The start of the layer: the slope, then the rest of the loop.
    clip(px(s), m_animating ? m_clip1 : X1 - px(s));
    Points first{{px(s), Y_BASE}};
    for (int i = 0; i < m_v.steps; ++i) {
        const double a = i * w, b = (i + 1) * w;
        if (b <= s)
            continue;
        const double y = y_of(ratio_at(b));
        first.emplace_back(px(std::max(a, s)), y);
        first.emplace_back(px(b), y);
    }
    first.emplace_back(X1, Y_TOP);
    first.emplace_back(X1, Y_BASE);
    fill_polygon(gc, first, bead, outline);
    gc.ResetClip();

    // The end of the layer, going over the slope with less and less material.
    clip(X0, m_animating ? m_clip2 : px(e) - X0);
    Points second{{X0, Y_TOP}, {px(e), Y_TOP}};
    for (int i = m_v.steps - 1; i >= 0; --i) {
        const double a = i * w, b = std::min((i + 1) * w, e);
        if (a >= e)
            continue;
        const double y = y_of(ratio_at((i + 1) * w));
        second.emplace_back(px(b), y);
        second.emplace_back(px(a), y);
    }
    fill_polygon(gc, second, bead2, outline);
    gc.ResetClip();

    // The seam.
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(text_colour(true), 1., wxPENSTYLE_SHORT_DASH)));
    gc.StrokeLine(X0, 36., X0, Y_BASE + 2. * LAYER_H);

    // The handles: the length (the mark above), the gap (the mark below), the start height (the nozzle), the smoothness (the
    // slider at the top right).
    const bool   handles  = !m_animating && m_v.type != 0;
    const double nozzle_x = m_animating ? m_nozzle_x : px(s);
    const double nozzle_y = m_animating ? m_nozzle_y : y_of(ratio_at(s));
    if (handles) {
        if (!m_v.entire) {
            stroke_line(gc, px(L), Y_TOP, px(L), Y_TOP - 24., accent(), 1.5);
            draw_handle(gc, px(L), Y_TOP - 34., 10., bg, true);
        }
        draw_handle(gc, px(s), Y_BASE + 16., 8., bg, true);
        stroke_line(gc, STEPS_X0, STEPS_Y, STEPS_X1, STEPS_Y, mix(text_colour(true), bg, 0.4), 3.);
        stroke_line(gc, STEPS_X0, STEPS_Y, steps_x(), STEPS_Y, accent(), 3.);
        draw_handle(gc, steps_x(), STEPS_Y, 8., bg, true);
    }
    if (m_v.type != 0)
        draw_nozzle(gc, nozzle_x, nozzle_y, handles);
    gc.PopState();

    // The texts, in pixels.
    draw_text(gc, _L("seam"), X0 * scale, 26. * scale, text_colour(true), Label::Body_11, 0);
    if (handles) {
        draw_text(gc, wxString::Format(_L("starts at %s mm"), num(m_v.start * m_layer_height)), (nozzle_x + 36.) * scale,
                  (nozzle_y - 51.) * scale, text_colour(true), Label::Body_11, -1);
        draw_text(gc, _L("smoothness"), (STEPS_X0 - 14.) * scale, STEPS_Y * scale, text_colour(true), Label::Body_11, 1);
    }
    if (m_v.entire)
        draw_text(gc, _L("slope all around"), (X0 + X1) / 2. * scale, (Y_TOP - 14.) * scale, accent(), Label::Body_11, 0);
    double x = XA * scale;
    for (const auto &[colour, text] : {std::make_pair(bead, _L("start of the layer")), std::make_pair(bead2, _L("end of the layer, going over it")),
                                       std::make_pair(below, _L("layers already printed"))}) {
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(colour));
        gc.DrawRoundedRectangle(x, (Y_LEGEND - 5.) * scale, 12. * scale, 10. * scale, 2. * scale);
        x += 18. * scale;
        x += draw_text(gc, text, x, Y_LEGEND * scale, text_colour(true), Label::Body_11, -1) + 22. * scale;
    }

    // No scarf: the drawing faded.
    if (m_v.type == 0) {
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(wxColour(bg.Red(), bg.Green(), bg.Blue(), 170)));
        gc.DrawRectangle(0., 0., SIDE_W * scale, SIDE_H * scale);
    }
}

ScarfSeamDialog::Drag ScarfSeamDialog::side_handle_at(double x, double y) const
{
    if (m_animating || m_v.type == 0)
        return Drag::None;
    auto         close_to = [x, y](double cx, double cy, double r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
    const double L = ramp_length(), s = slope_start();
    if (!m_v.entire && close_to(px(L), Y_TOP - 34., 14.))
        return Drag::Length;
    if (close_to(px(s), Y_BASE + 16., 12.))
        return Drag::Gap;
    if (close_to(steps_x(), STEPS_Y, 12.))
        return Drag::Steps;
    const double nx = px(s), ny = Y_BASE - ratio_at(s) * LAYER_H;
    if (x >= nx - 20. && x <= nx + 34. && y >= ny - 64. && y <= ny + 2.)
        return Drag::Start;
    return Drag::None;
}

std::string ScarfSeamDialog::side_region_at(double x, double y) const
{
    if (m_v.type == 0)
        return {};
    if (const Drag drag = side_handle_at(x, y); drag != Drag::None)
        return drag == Drag::Length ? "length_handle" : drag == Drag::Gap ? "gap_handle" : drag == Drag::Steps ? "steps_handle" : "nozzle";
    if (x < XA || x > X1 || y < Y_TOP || y > Y_BASE + 2. * LAYER_H)
        return {};
    if (y >= Y_BASE)
        return "below";
    if (x < X0)
        return "end";
    const double mm = (x - X0) * m_domain / (X1 - X0);
    if (mm >= ramp_length())
        return "p1";
    if (y >= Y_BASE - step_ratio_at(mm) * LAYER_H)
        return mm < slope_start() ? "gap" : "p1";
    return mm > slope_end() ? "gap" : "p2";
}

void ScarfSeamDialog::on_side_mouse(wxMouseEvent &evt)
{
    const double scale = m_side->GetClientSize().x / SIDE_W;
    const double x = evt.GetX() / scale, y = evt.GetY() / scale;
    if (evt.LeftDown()) {
        m_drag = side_handle_at(x, y);
        // The offset of the mouse from the handle: the nozzle is grabbed by its body, far above its tip.
        const double s = slope_start();
        m_grab_dx = m_drag == Drag::Length ? x - px(ramp_length()) : m_drag == Drag::Gap ? x - px(s) : m_drag == Drag::Steps ? x - steps_x() : 0.;
        m_grab_dy = m_drag == Drag::Start ? y - (Y_BASE - ratio_at(s) * LAYER_H) : 0.;
        if (m_drag != Drag::None) {
            touched();
            pause_tips();
            m_side->CaptureMouse();
        }
    } else if (evt.LeftUp()) {
        if (m_side->HasCapture())
            m_side->ReleaseMouse();
        if (m_drag != Drag::None) {
            m_drag = Drag::None;
            resume_tips_later();
        }
    } else if (evt.Dragging() && m_drag != Drag::None) {
        const double hx = x - m_grab_dx, hy = y - m_grab_dy;
        const double mm  = (hx - X0) * m_domain / (X1 - X0);
        const Values old = m_v;
        if (m_drag == Drag::Length) {
            m_v.length = std::round(std::clamp(mm, 0.5, m_domain) * 2.) / 2.;
            m_v.gap    = std::min(m_v.gap, m_v.length / 2.);
        } else if (m_drag == Drag::Gap)
            m_v.gap = std::round(std::clamp(mm / 2., 0., ramp_length() / 2.) * 20.) / 20.;
        else if (m_drag == Drag::Start) {
            // The tip is drawn at the start of the slope (after the gap), already a bit higher than the start height: ratio_at(s).
            const double r = (Y_BASE - hy) / LAYER_H, q = slope_start() / ramp_length();
            m_v.start      = std::round(std::clamp((r - q) / (1. - q), 0., 0.95) * 100.) / 100.;
        } else if (m_drag == Drag::Steps)
            m_v.steps = std::clamp(int(std::lround(1. + (hx - STEPS_X0) * (STEPS_MAX - 1) / (STEPS_X1 - STEPS_X0))), 1, STEPS_MAX);
        // Only when the rounded value moved: the mouse moves far more often.
        if (m_v.length != old.length || m_v.gap != old.gap || m_v.start != old.start || m_v.steps != old.steps)
            refresh();
    }
    // The cursor, and the tooltip of what is under the mouse (none while dragging, and 2 s after).
    const Drag hover = m_drag != Drag::None ? m_drag : side_handle_at(x, y);
    m_side->SetCursor(hover == Drag::Start ? wxCursor(wxCURSOR_SIZENS) : hover != Drag::None ? wxCursor(wxCURSOR_SIZEWE) : wxNullCursor);
    if (!tips_paused()) {
        const std::string region = evt.Leaving() || m_animating ? std::string() : side_region_at(x, y);
        if (region != m_side_tip) {
            m_side_tip = region;
            if (region.empty())
                m_side->UnsetToolTip();
            else
                m_side->SetToolTip(tip(region));
        }
    }
    evt.Skip();
}

// ---------------------------------------------------------------------------------------------------------------------
// Where, and the angle

// Seen from above, in thin lines: the part with its outer wall and inner wall, a hole with its two walls, the infill hatched.
// The bands of the walls getting the scarf filled with the color of the filament.
void ScarfSeamDialog::paint_where(wxGraphicsContext &gc, double scale)
{
    const wxColour bg    = GetBackgroundColour();
    const wxColour bead  = visible(m_color, bg);
    const bool     dark  = wxGetApp().dark_mode();
    const wxColour body  = dark ? wxColour(58, 63, 68) : wxColour(238, 241, 243);
    const wxColour off   = dark ? wxColour(120, 120, 118) : wxColour(170, 168, 160);
    const wxColour hatch = mix(body, bead, 0.45);
    // A rounded rectangle inset from the outline of the part.
    auto rect = [](wxGraphicsPath &p, double inset) {
        p.AddRoundedRectangle(8. + inset, 8. + inset, 114. - 2. * inset, 80. - 2. * inset, std::max(2., 14. - inset));
    };
    // A wall: the band between two outlines, filled when it gets the scarf.
    auto wall = [&](bool on, const std::function<void(wxGraphicsPath &, double)> &shape, double from, double to) {
        if (on) {
            wxGraphicsPath band = gc.CreatePath();
            shape(band, from);
            shape(band, to);
            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.SetBrush(wxBrush(wxColour(bead.Red(), bead.Green(), bead.Blue(), 120)));
            gc.FillPath(band, wxODDEVEN_RULE);
        }
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(on ? bead : off, 1.2)));
        for (double d : {from, to}) {
            wxGraphicsPath line = gc.CreatePath();
            shape(line, d);
            gc.StrokePath(line);
        }
    };
    auto contour = [&rect](wxGraphicsPath &p, double inset) { rect(p, inset); };
    auto hole    = [](wxGraphicsPath &p, double radius) { p.AddCircle(HOLE_X, HOLE_Y, radius); };

    gc.PushState();
    gc.Scale(scale, scale);
    // The part, its infill hatched between the inner walls, the hole empty.
    wxGraphicsPath part = gc.CreatePath();
    rect(part, 0.);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(body));
    gc.FillPath(part);
    gc.Clip(18., 18., 94., 60.);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(hatch, 1.)));
    for (double c = -70.; c <= 100.; c += 9.)
        gc.StrokeLine(18. + c, 18., 78. + c, 78.);
    gc.ResetClip();
    wxGraphicsPath around = gc.CreatePath();
    around.AddCircle(HOLE_X, HOLE_Y, HOLE_R + 10.);
    gc.SetBrush(wxBrush(body));
    gc.FillPath(around);
    wxGraphicsPath empty = gc.CreatePath();
    empty.AddCircle(HOLE_X, HOLE_Y, HOLE_R);
    gc.SetBrush(wxBrush(bg));
    gc.FillPath(empty);
    // The walls, those without scarf first so that a line shared with a wall getting it takes its color.
    const bool outer_on = m_v.type >= 1, inner_on = m_v.type >= 1 && m_v.inner;
    const bool hole_on = m_v.type == 2, hole_inner_on = m_v.type == 2 && m_v.inner;
    for (bool first_pass : {true, false}) {
        if ((!outer_on) == first_pass)
            wall(outer_on, contour, 0., 5.);
        if ((!inner_on) == first_pass)
            wall(inner_on, contour, 5., 10.);
        if ((!hole_on) == first_pass)
            wall(hole_on, hole, HOLE_R, HOLE_R + 5.);
        if ((!hole_inner_on) == first_pass)
            wall(hole_inner_on, hole, HOLE_R + 5., HOLE_R + 10.);
    }
    gc.PopState();
}

void ScarfSeamDialog::paint_angle(wxGraphicsContext &gc, double scale)
{
    const wxColour bg   = GetBackgroundColour();
    const wxColour bead = visible(m_color, bg);
    const wxColour off  = wxGetApp().dark_mode() ? wxColour(95, 94, 90) : wxColour(211, 209, 199);
    const double   t    = (180. - m_v.angle) * PI / 180.;
    const double   hx = CORNER_X + CORNER_ARM * std::cos(t), hy = CORNER_Y - CORNER_ARM * std::sin(t);
    struct Shape { int sides; int angle; wxString name; };
    const Shape shapes[4] = {{4, 90, _L("Cube")}, {6, 120, _L("Hexagon")}, {8, 135, _L("Octagon")}, {0, 178, _L("Cylinder")}};
    auto shape_x = [](int i) { return 35. + i * 60.; };

    gc.PushState();
    gc.Scale(scale, scale);
    // The corner where the seam falls, seen from above, as a bead: the limit, wider walls get the scarf.
    wxGraphicsPath corner = gc.CreatePath();
    corner.MoveToPoint(CORNER_X - CORNER_ARM + 6., CORNER_Y);
    corner.AddLineToPoint(CORNER_X, CORNER_Y);
    corner.AddLineToPoint(hx, hy);
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(m_v.smart ? bead : off, 11.).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
    gc.StrokePath(corner);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(mix(bead, *wxBLACK, 0.5)));
    gc.DrawEllipse(CORNER_X - 4., CORNER_Y - 4., 8., 8.);
    if (m_v.smart)
        draw_handle(gc, hx, hy, 9., bg, false);
    // Examples under it: which shapes get the scarf with this angle.
    for (int i = 0; i < 4; ++i) {
        const double   cx = shape_x(i), cy = 106.;
        const bool     scarf = !m_v.smart || shapes[i].angle > m_v.angle;
        wxGraphicsPath path  = gc.CreatePath();
        if (shapes[i].sides == 0)
            path.AddCircle(cx, cy, 11.);
        else {
            for (int k = 0; k < shapes[i].sides; ++k) {
                const double u = 2. * PI * k / shapes[i].sides + PI / shapes[i].sides;
                if (k == 0)
                    path.MoveToPoint(cx + 11. * std::cos(u), cy + 11. * std::sin(u));
                else
                    path.AddLineToPoint(cx + 11. * std::cos(u), cy + 11. * std::sin(u));
            }
            path.CloseSubpath();
        }
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(scarf ? bead : off, 4.).Join(wxJOIN_ROUND)));
        gc.StrokePath(path);
    }
    gc.PopState();
    for (int i = 0; i < 4; ++i) {
        const bool scarf = !m_v.smart || shapes[i].angle > m_v.angle;
        draw_text(gc, shapes[i].name, shape_x(i) * scale, 128. * scale, text_colour(), Label::Body_11, 0);
        draw_text(gc, scarf ? _L("scarf") : _L("in the corner"), shape_x(i) * scale, 142. * scale, scarf ? accent() : text_colour(true), Label::Body_11, 0);
    }
}

void ScarfSeamDialog::on_angle_mouse(wxMouseEvent &evt)
{
    const double scale = m_angle->GetClientSize().x / ANGLE_W;
    const double x = evt.GetX() / scale, y = evt.GetY() / scale;
    const double t = (180. - m_v.angle) * PI / 180.;
    const double hx = CORNER_X + CORNER_ARM * std::cos(t), hy = CORNER_Y - CORNER_ARM * std::sin(t);
    const bool   on_handle = m_v.smart && (x - hx) * (x - hx) + (y - hy) * (y - hy) <= 14. * 14.;
    if (evt.LeftDown() && on_handle) {
        touched();
        m_drag = Drag::Angle;
        pause_tips();
        m_angle->CaptureMouse();
    } else if (evt.LeftUp()) {
        if (m_angle->HasCapture())
            m_angle->ReleaseMouse();
        if (m_drag == Drag::Angle) {
            m_drag = Drag::None;
            resume_tips_later();
        }
    } else if (evt.Dragging() && m_drag == Drag::Angle) {
        const double a     = std::atan2(CORNER_Y - y, x - CORNER_X) * 180. / PI;
        const int    angle = int(std::lround(std::clamp(180. - a, 60., 180.)));
        if (angle != m_v.angle) {
            m_v.angle = angle;
            refresh();
        }
    }
    m_angle->SetCursor(on_handle || m_drag == Drag::Angle ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    evt.Skip();
}

// ---------------------------------------------------------------------------------------------------------------------
// The animation: the slope and the rest of the loop, the end of the loop coming to the seam, the end going over the slope.

void ScarfSeamDialog::start_animation()
{
    if (m_v.type == 0)
        return;
    m_animating  = true;
    m_anim_start = std::chrono::steady_clock::now();
    m_clip0 = m_clip1 = m_clip2 = 0.;
    m_side_tip.clear();
    m_side->UnsetToolTip();
    m_timer.Start(16);
    animation_step();
}

void ScarfSeamDialog::animation_step()
{
    if (!m_animating)
        return;
    const double s = slope_start(), L = ramp_length();
    const double a = X1 - px(s), b = X0 - XA, c = std::max(0., px(slope_end()) - X0), total = a + b + c;
    const double d = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_anim_start).count() / ANIMATION_SECONDS * total;
    int          phase;
    if (d < a) {
        m_clip1         = d;
        m_nozzle_x      = px(s) + d;
        const double mm = (m_nozzle_x - X0) * m_domain / (X1 - X0);
        m_nozzle_y      = Y_BASE - (mm < L ? ratio_at(mm) : 1.) * LAYER_H;
        phase           = mm < L ? 1 : 2;
    } else if (d < a + b) {
        m_clip1    = a;
        m_clip0    = d - a;
        m_nozzle_x = XA + m_clip0;
        m_nozzle_y = Y_TOP;
        phase      = 2;
    } else if (d < total) {
        m_clip0    = b;
        m_clip2    = d - a - b;
        m_nozzle_x = X0 + m_clip2;
        m_nozzle_y = Y_TOP;
        phase      = 3;
    } else {
        m_timer.Stop();
        m_animating = false;
        phase       = 4;
    }
    if (phase != m_phase) {
        m_phase = phase;
        show_caption();
    }
    m_side->Refresh();
}

void ScarfSeamDialog::on_dpi_changed(const wxRect &)
{
    for (wxWindow *child : GetChildren())
        if (auto *btn = dynamic_cast<Button *>(child)) {
            if (btn != m_play) {
                btn->SetMinSize(wxSize(FromDIP(84), FromDIP(26)));
                btn->SetCornerRadius(FromDIP(12));
            }
            btn->Rescale();
        }
    m_where->SetMinSize(wxSize(FromDIP(int(WHERE_W * WHERE_ZOOM)), FromDIP(int(WHERE_H * WHERE_ZOOM))));
    m_side->SetMinSize(wxSize(FromDIP(int(SIDE_W)), FromDIP(int(SIDE_H))));
    m_caption->SetMinSize(wxSize(FromDIP(int(CAPTION_W)), FromDIP(int(CAPTION_H))));
    for (Reset &reset : m_resets)
        reset.panel->SetMinSize(wxSize(FromDIP(int(RESET_SIZE)), FromDIP(int(RESET_SIZE))));
    if (m_angle != nullptr)
        m_angle->SetMinSize(wxSize(FromDIP(int(ANGLE_W)), FromDIP(int(ANGLE_H))));
    Layout();
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
