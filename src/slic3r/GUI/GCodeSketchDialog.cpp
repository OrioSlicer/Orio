#include "GCodeSketchDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include "libslic3r/Utils.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <limits>

namespace Slic3r {
namespace GUI {

namespace {

// Longer G-code is shown in part, but copied and saved whole.
constexpr size_t MAX_SHOWN = 2 * 1024 * 1024;
// Lines the G-code console sends at most (GCodeConsoleDialog: MAX_LINES).
constexpr size_t MAX_CONSOLE_LINES = 1000;
// Beyond this size, the G-code isn't written: the size of one layer times the layers.
constexpr double MAX_GCODE_MB = 64.;

wxColour warning_colour() { return wxColour(226, 75, 74); }

} // namespace

GCodeSketchDialog::GCodeSketchDialog(wxWindow *parent, const GCodeSketch::Drawing &drawing, const GCodeSketch::Settings &settings,
                                     const GCodeSketch::Machine &machine, GCodeSketch::ExportOptions &options, const wxString &problems,
                                     const wxString &directory, const wxString &file_name)
    : DPIDialog(parent, wxID_ANY, _L("G-code of the drawing"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_drawing(drawing)
    , m_settings(settings)
    , m_machine(machine)
    , m_options(options)
    , m_directory(directory)
    , m_file_name(file_name)
{
    SetBackgroundColour(*wxWHITE);
    const int em    = em_unit();
    auto     *sizer = new wxBoxSizer(wxVERTICAL);

    m_block   = new wxRadioButton(this, wxID_ANY, _L("Block to insert into G-code (custom, start or end G-code)"), wxDefaultPosition,
                                  wxDefaultSize, wxRB_GROUP);
    m_program = new wxRadioButton(this, wxID_ANY, _L("Program run alone by the printer (heating, homing and end)"));
    m_block->SetValue(!options.standalone);
    m_program->SetValue(options.standalone);
    sizer->Add(m_block, 0, wxLEFT | wxRIGHT | wxTOP, em);
    sizer->Add(m_program, 0, wxLEFT | wxRIGHT | wxTOP, em / 2);

    auto *values = new wxBoxSizer(wxHORIZONTAL);
    auto  spin   = [&](const wxString &label, int min, int max, int value) {
        values->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
        auto *control = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(8 * em, -1), wxSP_ARROW_KEYS, min, max, value);
        values->Add(control, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 * em);
        return control;
    };
    m_nozzle_temp = spin(_L("Nozzle temperature"), 0, 500, options.nozzle_temp);
    m_bed_temp    = spin(_L("Bed temperature"), 0, 200, options.bed_temp);
    m_fan         = spin(_L("Part cooling fan (%)"), 0, 100, options.fan_percent);
    sizer->Add(values, 0, wxLEFT | wxRIGHT | wxTOP, em);

    m_approach = new wxCheckBox(this, wxID_ANY, _L("Safe approach: lift the nozzle, move above the start, then lower it"));
    m_arcs     = new wxCheckBox(this, wxID_ANY, _L("Horizontal arcs as G2 / G3 (otherwise straight moves)"));
    m_comments = new wxCheckBox(this, wxID_ANY, _L("Comments"));
    m_approach->SetValue(options.safe_approach);
    m_arcs->SetValue(options.arcs);
    m_comments->SetValue(options.comments);
    for (wxCheckBox *check : { m_approach, m_arcs, m_comments })
        sizer->Add(check, 0, wxLEFT | wxRIGHT | wxTOP, em / 2);

    m_text = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(72 * em, 40 * em),
                            wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
    m_text->SetFont(wxGetApp().code_font());
    sizer->Add(m_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    m_info = new wxStaticText(this, wxID_ANY, wxEmptyString);
    sizer->Add(m_info, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
    if (!problems.empty()) {
        auto *warning = new wxStaticText(this, wxID_ANY, problems);
        warning->SetForegroundColour(warning_colour());
        sizer->Add(warning, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
    }

    auto *buttons      = new wxBoxSizer(wxHORIZONTAL);
    auto *copy_button  = new wxButton(this, wxID_ANY, _L("Copy"));
    auto *save_button  = new wxButton(this, wxID_ANY, _L("Save..."));
    auto *send_button  = new wxButton(this, wxID_ANY, _L("Send to the printer..."));
    auto *close_button = new wxButton(this, wxID_CANCEL, _L("Close"));
    copy_button->SetToolTip(_L("Copy the G-code to the clipboard"));
    save_button->SetToolTip(_L("Save the G-code as a .gcode file, which opens again in the G-code Drawing tab"));
    send_button->SetToolTip(_L("Open the G-code console with this G-code, to check it and send it to the printer chosen there"));
    buttons->Add(copy_button, 0, wxRIGHT, em);
    buttons->Add(save_button, 0, wxRIGHT, em);
    buttons->Add(send_button, 0, wxRIGHT, em);
    m_copy_button = copy_button;
    m_save_button = save_button;
    m_send_button = send_button;
    buttons->AddStretchSpacer();
    buttons->Add(close_button, 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, em);
    SetSizer(sizer);

    for (wxRadioButton *radio : { m_block, m_program })
        radio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent &) { update_gcode(); });
    for (wxSpinCtrl *control : { m_nozzle_temp, m_bed_temp, m_fan })
        control->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { update_gcode(); });
    for (wxCheckBox *check : { m_approach, m_arcs, m_comments })
        check->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { update_gcode(); });
    copy_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { copy(); });
    save_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { save(); });
    send_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { send(); });
    close_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

    update_gcode();
    Fit();
    SetMinSize(wxSize(50 * em, 30 * em));
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void GCodeSketchDialog::on_dpi_changed(const wxRect &)
{
    Fit();
    Refresh();
}

void GCodeSketchDialog::read_options()
{
    m_options.standalone    = m_program->GetValue();
    m_options.nozzle_temp   = m_nozzle_temp->GetValue();
    m_options.bed_temp      = m_bed_temp->GetValue();
    m_options.fan_percent   = m_fan->GetValue();
    m_options.safe_approach = m_approach->GetValue();
    m_options.arcs          = m_arcs->GetValue();
    m_options.comments      = m_comments->GetValue();
    // The temperatures are only set by a program run alone.
    m_nozzle_temp->Enable(m_options.standalone);
    m_bed_temp->Enable(m_options.standalone);
}

void GCodeSketchDialog::update_gcode()
{
    read_options();
    // Too many layers for the drawing: the G-code would be too large. Its size is the one of a layer (arcs written as
    // straight moves included) times the layers.
    GCodeSketch::Settings one_layer = m_settings;
    one_layer.layers                = 1;
    const double megabytes = double(GCodeSketch::generate(m_drawing, one_layer, m_machine, m_options).size()) *
                             double(std::max(m_settings.layers, 1)) / (1024. * 1024.);
    const bool   too_big   = megabytes > MAX_GCODE_MB;
    for (wxWindow *button : { m_copy_button, m_save_button, m_send_button })
        if (button != nullptr)
            button->Enable(!too_big);
    if (too_big) {
        m_gcode.clear();
        m_text->ChangeValue(wxString::Format(_L("The G-code of the drawing would be about %d MB, at most %d MB: reduce the number of layers."),
                                             int(std::min(megabytes, 1e9)), int(MAX_GCODE_MB)));
        m_info->SetLabel(wxEmptyString);
        return;
    }
    m_gcode = GCodeSketch::generate(m_drawing, m_settings, m_machine, m_options, &m_stats);

    wxString text;
    if (m_gcode.size() > MAX_SHOWN) {
        // Cut at the end of a line, not to split a character.
        const size_t end = m_gcode.rfind('\n', MAX_SHOWN);
        text = from_u8(m_gcode.substr(0, end == std::string::npos ? MAX_SHOWN : end + 1)) + "\n" +
               _L("[...] The G-code is too long to be shown whole: it is copied and saved whole.");
    } else
        text = from_u8(m_gcode);
    m_text->ChangeValue(text);
    m_text->ShowPosition(0);

    const int lines = int(std::count(m_gcode.begin(), m_gcode.end(), '\n'));
    wxString  info  = wxString::Format(_L("%d lines, %.1f mm printed, %.2f m of filament (%.1f g), about %s"), lines, m_stats.extrude_length,
                                       m_stats.filament_length / 1000., m_stats.filament_weight,
                                       from_u8(short_time(get_time_dhms(float(m_stats.time)))));
    // A block in absolute extrusion resets E: it must go where the G-code resets E as well.
    if (!m_machine.relative_e && !m_options.standalone)
        info += "\n" + _L("Absolute extrusion: the block resets E (G92 E0). Insert it where the G-code resets E too, e.g. at a layer change.");
    m_info->SetLabel(info);
    Layout();
}

void GCodeSketchDialog::copy()
{
    if (!wxTheClipboard->Open()) {
        show_error(this, _L("The clipboard cannot be opened."));
        return;
    }
    wxTheClipboard->SetData(new wxTextDataObject(from_u8(m_gcode)));
    wxTheClipboard->Close();
    m_info->SetLabel(_L("The G-code is copied to the clipboard."));
}

void GCodeSketchDialog::send()
{
    // The console sends the lines without their comments, at most MAX_CONSOLE_LINES of them.
    size_t lines = 0;
    size_t begin = 0;
    while (begin < m_gcode.size()) {
        size_t end = m_gcode.find('\n', begin);
        if (end == std::string::npos)
            end = m_gcode.size();
        const size_t first = m_gcode.find_first_not_of(" \t\r", begin);
        if (first < end && m_gcode[first] != ';')
            ++ lines;
        begin = end + 1;
    }
    if (lines > MAX_CONSOLE_LINES) {
        show_error(this, wxString::Format(_L("This G-code has %d lines: the G-code console sends at most %d. Save it as a .gcode file "
                                             "and print it instead."),
                                          int(lines), int(MAX_CONSOLE_LINES)));
        return;
    }
    m_send = true;
    EndModal(wxID_OK);
}

void GCodeSketchDialog::save()
{
    wxFileDialog dialog(this, _L("Save the G-code"), m_directory, m_file_name, _L("G-code files") + " (*.gcode)|*.gcode",
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK)
        return;
    const wxString path = dialog.GetPath();
    bool           written = false;
    {
        boost::nowide::ofstream file(into_u8(path), std::ios::binary | std::ios::trunc);
        if (file) {
            file << m_gcode;
            // The drawing, so that the file opens again for editing in the tab.
            file << GCodeSketch::serialize(m_drawing, m_settings);
            file.close();
            written = !file.fail();
        }
    }
    if (!written) {
        show_error(this, _L("The file cannot be written."));
        return;
    }
    m_directory = wxPathOnly(path);
    m_file_name = wxFileNameFromPath(path);
    m_info->SetLabel(wxString::Format(_L("Saved to %s"), path));
}

} // namespace GUI
} // namespace Slic3r
