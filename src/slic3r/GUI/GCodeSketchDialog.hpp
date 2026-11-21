#ifndef slic3r_GCodeSketchDialog_hpp_
#define slic3r_GCodeSketchDialog_hpp_

// Orio: G-code of a drawing of the "G-code drawing" tab (GCodeSketchPanel), shown to be copied, saved as a
// .gcode file or sent to the printer through the G-code console, either as a block to insert into G-code or as a
// program run alone by the printer.

#include "GUI_Utils.hpp"
#include "libslic3r/GCodeSketch.hpp"

#include <string>

class wxCheckBox;
class wxRadioButton;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

class GCodeSketchDialog : public DPIDialog
{
public:
    // The options are kept for the next time. directory and file_name are proposed to save the file.
    GCodeSketchDialog(wxWindow *parent, const GCodeSketch::Drawing &drawing, const GCodeSketch::Settings &settings,
                      const GCodeSketch::Machine &machine, GCodeSketch::ExportOptions &options, const wxString &problems,
                      const wxString &directory, const wxString &file_name);

    // Folder and name of the last file saved, otherwise the ones proposed.
    const wxString &directory() const { return m_directory; }
    const wxString &file_name() const { return m_file_name; }
    // "Send to the printer" was clicked: gcode() goes to the G-code console once the dialog is closed.
    bool               send_requested() const { return m_send; }
    const std::string &gcode() const { return m_gcode; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void read_options();
    void update_gcode();
    void copy();
    void save();
    void send();

    const GCodeSketch::Drawing  &m_drawing;
    const GCodeSketch::Settings &m_settings;
    const GCodeSketch::Machine  &m_machine;
    GCodeSketch::ExportOptions  &m_options;
    std::string                  m_gcode;
    GCodeSketch::Stats           m_stats;

    wxRadioButton *m_block       { nullptr };
    wxRadioButton *m_program     { nullptr };
    wxSpinCtrl    *m_nozzle_temp { nullptr };
    wxSpinCtrl    *m_bed_temp    { nullptr };
    wxSpinCtrl    *m_fan         { nullptr };
    wxCheckBox    *m_approach    { nullptr };
    wxCheckBox    *m_arcs        { nullptr };
    wxCheckBox    *m_comments    { nullptr };
    wxTextCtrl    *m_text        { nullptr };
    wxStaticText  *m_info        { nullptr };
    wxWindow      *m_copy_button { nullptr };
    wxWindow      *m_save_button { nullptr };
    wxWindow      *m_send_button { nullptr };
    wxString       m_directory;
    wxString       m_file_name;
    bool           m_send        { false };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GCodeSketchDialog_hpp_
