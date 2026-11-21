#include "GCodeConsoleDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "SelectMachine.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevAxis.h"
#include "DeviceCore/DevBed.h"
#include "DeviceCore/DevChamber.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevManager.h"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <wx/clipbrd.h>
#include <wx/datetime.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace Slic3r { namespace GUI {

namespace {

constexpr int    TIMER_PERIOD_MS        = 1000;
constexpr int    ACK_TIMEOUT_S          = 10;   // au-delà, la commande est signalée sans accusé de réception
constexpr size_t MAX_LINES_PER_MESSAGE  = 50;   // lignes par commande « gcode_line »
constexpr size_t MAX_LINES              = 1000; // au-delà, mieux vaut imprimer un fichier .gcode
constexpr size_t HISTORY_MAX            = 100;
constexpr size_t HISTORY_ENTRY_MAX      = 4000; // caractères au plus par entrée de l'historique
const char      *HISTORY_KEY            = "gcode_console_history";

// Mot de la forme lettre + nombre (G28, m104, S200, X-10.5…). Pour la commande (premier mot),
// au moins un chiffre après la lettre.
bool is_code_word(const std::string &word, bool command)
{
    if (word.size() < (command ? 2u : 1u) || !std::isalpha(static_cast<unsigned char>(word.front())))
        return false;
    for (size_t i = 1; i < word.size(); ++i) {
        const char c = word[i];
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+'))
            return false;
    }
    return !command || std::isdigit(static_cast<unsigned char>(word[1]));
}

// « g28 » → « G28 », « m104 s200 » → « M104 S200 ». Seules les lignes qui commencent par une
// commande G/M/T sont touchées, et seuls leurs mots lettre + nombre : « M1002 gcode_claim_action : 2 »
// reste tel quel (les commandes Bambu à mots-clés sont sensibles à la casse).
std::string normalize_line(const std::string &line)
{
    std::vector<std::string> words;
    boost::split(words, line, boost::is_any_of(" \t"), boost::token_compress_on);
    if (words.empty() || !is_code_word(words.front(), true))
        return line;
    const char cmd = static_cast<char>(std::toupper(static_cast<unsigned char>(words.front().front())));
    if (cmd != 'G' && cmd != 'M' && cmd != 'T')
        return line;

    std::string out;
    for (size_t i = 0; i < words.size(); ++i) {
        std::string word = words[i];
        if (is_code_word(word, i == 0))
            word.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(word.front())));
        if (!out.empty())
            out += ' ';
        out += word;
    }
    return out;
}

// Texte saisi → lignes à envoyer : commentaires (« ; … ») et lignes vides retirés.
std::vector<std::string> prepare_lines(const std::string &text)
{
    std::vector<std::string> raw;
    boost::split(raw, text, boost::is_any_of("\n"));
    std::vector<std::string> lines;
    for (std::string line : raw) {
        if (const size_t comment = line.find(';'); comment != std::string::npos)
            line.erase(comment);
        boost::trim(line);
        if (!line.empty())
            lines.emplace_back(normalize_line(line));
    }
    return lines;
}

// Position « Park » : au-dessus de la goulotte de purge, là où le firmware purge le filament lors
// d'un changement de filament (X54 Y265 sur les X1 et P1 : change_filament_gcode et
// machine_start_gcode des profils). Trajet repris de resources/printers/ams_unload.gcode, que
// Bambu Studio envoie lui-même : courant XY réduit (M17), petit recul en Y et plateau descendu de
// 3 mm, origine X, puis entrée lente dans la zone de la goulotte par Y245 → Y265, enfin X54.
// Vide si la position n'est pas connue pour ce modèle (A1, H2D, P2S… : autre géométrie).
std::vector<std::string> park_gcode(const MachineObject *obj)
{
    static const std::set<std::string> x1_p1 = {"BL-P001", "BL-P002", "C13", "C11", "C12"}; // X1 Carbon, X1, X1E, P1P, P1S
    if (obj == nullptr || x1_p1.count(obj->printer_type) == 0)
        return {};
    return {"M17 S",         "M17 X0.5 Y0.5", "G91",           "G1 Y-5 F3000",  "G1 Z3 F1200",   "G90",
            "G28 X",         "M17 R",         "G1 X70 F21000", "G1 Y245",       "G1 Y265 F3000", "G1 X54 F3000"};
}

// Imprimante LAN dont un envoi « Imprimer » en arrière-plan tient encore la seule connexion MQTT locale
// (BackgroundPrintJobs::lan_connection_holder) ; false si aucune, ou si c'est dev_id lui-même.
// Choisir une autre machine la couperait (DeviceManager::set_selected_machine → disconnect_printer).
bool lan_connection_busy(const std::string &dev_id, wxString &holder_name)
{
    Plater *plater = wxGetApp().plater();
    auto   *print_dlg = plater != nullptr ? static_cast<SelectMachineDialog *>(plater->get_select_machine_dialog()) : nullptr;
    std::string holder_id;
    return print_dlg != nullptr && print_dlg->lan_connection_holder(holder_id, holder_name) && holder_id != dev_id;
}

wxString printer_name(const MachineObject *obj)
{
    return obj ? wxString::FromUTF8(obj->get_dev_name().c_str()) : wxString();
}

const wxString &deg_c()
{
    static const wxString s(L" °C");
    return s;
}

const wxString &dot_sep()
{
    static const wxString s(L"  ·  ");
    return s;
}

} // namespace

GCodeConsoleDialog *GCodeConsoleDialog::s_instance = nullptr;

void GCodeConsoleDialog::show_console(wxWindow *parent)
{
    if (s_instance == nullptr)
        s_instance = new GCodeConsoleDialog(parent != nullptr ? parent : static_cast<wxWindow *>(wxGetApp().mainframe));
    s_instance->refresh_printers();
    s_instance->refresh_status();
    if (!s_instance->m_timer.IsRunning())
        s_instance->m_timer.Start(TIMER_PERIOD_MS);
    s_instance->Show();
    s_instance->Raise();
    s_instance->m_input->SetFocus();
}

void GCodeConsoleDialog::show_console_with_text(wxWindow *parent, const wxString &text)
{
    show_console(parent);
    if (s_instance != nullptr) {
        s_instance->m_input->ChangeValue(text);
        s_instance->m_input->SetInsertionPoint(0);
    }
}

void GCodeConsoleDialog::on_printer_message(const std::string &dev_id, const std::string &msg)
{
    // Filtre bon marché d'abord : la plupart des messages sont des push_status.
    if (s_instance == nullptr || s_instance->m_pending.empty() || msg.find("gcode_line") == std::string::npos)
        return;
    try {
        const nlohmann::json j = nlohmann::json::parse(msg);
        if (!j.contains("print") || !j["print"].is_object())
            return;
        const nlohmann::json &p = j["print"];
        if (!p.contains("command") || !p["command"].is_string() || p["command"].get<std::string>() != "gcode_line")
            return;

        int sequence_id = -1;
        if (p.contains("sequence_id")) {
            if (p["sequence_id"].is_string())
                sequence_id = std::stoi(p["sequence_id"].get<std::string>());
            else if (p["sequence_id"].is_number_integer())
                sequence_id = p["sequence_id"].get<int>();
        }
        const std::string result = p.contains("result") && p["result"].is_string() ? p["result"].get<std::string>() : std::string();
        const std::string reason = p.contains("reason") && p["reason"].is_string() ? p["reason"].get<std::string>() : std::string();
        const int err_code = p.contains("err_code") && p["err_code"].is_number_integer() ? p["err_code"].get<int>() : 0;
        s_instance->on_ack(dev_id, sequence_id, result, reason, err_code);
    } catch (...) {
        // Message illisible : ce n'est pas un accusé de la console.
    }
}

GCodeConsoleDialog::GCodeConsoleDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("G-code console"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    std::string icon_path = (boost::format("%1%/images/BambuStudioTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));
    SetBackgroundColour(*wxWHITE);

    const StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(5, 96, 194), StateColor::Pressed),
                                  std::pair<wxColour, int>(wxColour(43, 140, 242), StateColor::Hovered),
                                  std::pair<wxColour, int>(wxColour(8, 116, 224), StateColor::Normal));
    const StateColor btn_bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                                  std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                                  std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    auto make_button = [this, &btn_bg_green, &btn_bg_white](const wxString &label, bool primary) {
        Button *btn = new Button(this, label);
        if (primary) {
            btn->SetBackgroundColor(btn_bg_green);
            btn->SetBorderColor(wxColour(8, 116, 224));
            btn->SetTextColor(wxColour("#FFFFFE"));
        } else {
            btn->SetBackgroundColor(btn_bg_white);
            btn->SetBorderColor(wxColour(38, 46, 48));
        }
        btn->SetFont(Label::Body_12);
        btn->SetMinSize(wxSize(FromDIP(84), FromDIP(26)));
        btn->SetCornerRadius(FromDIP(12));
        return btn;
    };

    wxFont mono(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    if (!mono.IsOk())
        mono = wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);

    // Imprimante + état (températures, impression en cours…)
    auto *top_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *printer_label = new Label(this, Label::Body_13, _L("Printer"));
    m_printer_combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(240), -1), 0, nullptr, wxCB_READONLY);
    m_printer_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) { on_printer_selected(e.GetInt()); });
    m_status = new Label(this, Label::Body_12, wxEmptyString, wxST_ELLIPSIZE_END);
    m_status->SetForegroundColour(wxColour(107, 107, 107));
    top_sizer->Add(printer_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    top_sizer->Add(m_printer_combo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    top_sizer->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
    m_park_btn = make_button(_L("Park"), false);
    m_park_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { send_park(); });
    top_sizer->Add(m_park_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    main_sizer->Add(top_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // Journal
    m_log = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(620), FromDIP(300)),
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    m_log->SetFont(mono);
    main_sizer->Add(m_log, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    // Saisie
    auto *input_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_input = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(52)), wxTE_MULTILINE);
    m_input->SetFont(mono);
    m_input->Bind(wxEVT_KEY_DOWN, &GCodeConsoleDialog::on_input_key, this);
    m_send_btn = make_button(_L("Send"), true);
    m_send_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { send_input(); });
    input_sizer->Add(m_input, 1, wxEXPAND | wxRIGHT, FromDIP(8));
    input_sizer->Add(m_send_btn, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(input_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    // Aide clavier + boutons du journal
    auto *bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_hint = new Label(this, Label::Body_12, _L("Enter: send    Shift+Enter: new line    Up/Down: history    ; comment"));
    m_hint->SetForegroundColour(wxColour(144, 144, 144));
    m_copy_btn = make_button(_L("Copy log"), false);
    m_copy_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(m_log->GetValue()));
            wxTheClipboard->Close();
        }
    });
    m_clear_btn = make_button(_L("Clear log"), false);
    m_clear_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_log->Clear(); });
    bottom_sizer->Add(m_hint, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    bottom_sizer->Add(m_copy_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    bottom_sizer->Add(m_clear_btn, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(bottom_sizer, 0, wxEXPAND | wxALL, FromDIP(12));

    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);
    Layout();
    CentreOnParent();

    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &GCodeConsoleDialog::on_timer, this);
    // Fermer = cacher : le journal et l'historique restent pour la prochaine ouverture.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &e) {
        if (e.CanVeto()) {
            e.Veto();
            Hide();
            return;
        }
        e.Skip();
    });

    load_history();
    wxGetApp().UpdateDlgDarkUI(this);

    append_log(_L("Commands go to the selected printer, the same way as the jog buttons of the Device tab. "
                  "Bambu printers do not send back the text output of commands (M105, M114...): "
                  "the console shows whether each command was accepted, and the temperatures above."),
               LogKind::Info);
}

GCodeConsoleDialog::~GCodeConsoleDialog()
{
    m_timer.Stop();
    if (s_instance == this)
        s_instance = nullptr;
}

void GCodeConsoleDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    for (Button *btn : {m_park_btn, m_send_btn, m_copy_btn, m_clear_btn}) {
        btn->SetMinSize(wxSize(FromDIP(84), FromDIP(26)));
        btn->SetCornerRadius(FromDIP(12));
        btn->Rescale();
    }
    m_printer_combo->Rescale();
    Layout();
    Refresh();
}

void GCodeConsoleDialog::on_timer(wxTimerEvent &)
{
    expire_pending();
    apply_printer_switch();
    if (!IsShown()) {
        if (m_pending.empty() && m_switch_pending.empty())
            m_timer.Stop();
        return;
    }
    refresh_printers();
    refresh_status();
}

void GCodeConsoleDialog::on_input_key(wxKeyEvent &event)
{
    const int key = event.GetKeyCode();
    if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && !event.ShiftDown()) {
        send_input();
        return;
    }
    if (key == WXK_UP || key == WXK_DOWN) {
        // Historique depuis la première (↑) ou la dernière (↓) ligne de la saisie ; ailleurs, le curseur se déplace.
        long col = 0, row = 0;
        m_input->PositionToXY(m_input->GetInsertionPoint(), &col, &row);
        if ((key == WXK_UP && row <= 0) || (key == WXK_DOWN && row >= m_input->GetNumberOfLines() - 1)) {
            history_step(key == WXK_UP ? -1 : 1);
            return;
        }
    }
    event.Skip();
}

void GCodeConsoleDialog::on_printer_selected(int selection)
{
    if (selection < 0 || selection >= int(m_printer_ids.size()))
        return;
    DeviceManager *dev = wxGetApp().getDeviceManager();
    if (dev == nullptr)
        return;
    m_switch_pending = m_printer_ids[selection];
    wxString holder_name;
    if (!apply_printer_switch() && lan_connection_busy(m_switch_pending, holder_name))
        append_log(wxString::Format(_L("Waiting for the end of the sending to %s (only one printer in LAN mode can be connected at a time)."),
                                    holder_name),
                   LogKind::Info);
}

// Sélectionne la machine choisie (m_switch_pending), sauf si un envoi LAN en arrière-plan tient encore la
// connexion locale d'une autre imprimante : false, on réessaie au tic suivant (comme
// SelectMachineDialog::apply_machine_switch). Bug [81] du journal.
bool GCodeConsoleDialog::apply_printer_switch()
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    if (m_switch_pending.empty() || dev == nullptr) {
        m_switch_pending.clear();
        return true;
    }
    const std::string dev_id  = m_switch_pending;
    MachineObject    *current = dev->get_selected_machine();
    if (current != nullptr && current->get_dev_id() == dev_id) {
        m_switch_pending.clear();
        return true;
    }
    wxString holder_name;
    if (lan_connection_busy(dev_id, holder_name))
        return false;

    // Même chemin que le choix d'imprimante de l'onglet Imprimante, qui suit donc la console.
    // En LAN, une seule imprimante est connectée à la fois : l'ancienne est déconnectée.
    if (MainFrame *mainframe = wxGetApp().mainframe; mainframe != nullptr && mainframe->m_monitor != nullptr)
        mainframe->m_monitor->select_machine(dev_id);
    else
        dev->set_selected_machine(dev_id);
    m_switch_pending.clear();
    m_last_selected = dev_id;
    append_log(wxString::Format(_L("Connecting to %s..."), printer_name(dev->get_my_machine(dev_id))), LogKind::Info);
    return true;
}

void GCodeConsoleDialog::refresh_printers()
{
    DeviceManager *dev = wxGetApp().getDeviceManager();

    std::vector<std::pair<std::string, MachineObject *>> machines;
    if (dev != nullptr)
        for (const auto &[dev_id, obj] : dev->get_my_machine_list())
            if (obj != nullptr)
                machines.emplace_back(dev_id, obj);
    std::sort(machines.begin(), machines.end(), [](const auto &a, const auto &b) { return a.second->get_dev_name() < b.second->get_dev_name(); });

    std::string signature;
    for (const auto &[dev_id, obj] : machines)
        signature += dev_id + '\x1f' + obj->get_dev_name() + (obj->is_lan_mode_printer() ? "\x1fL" : "\x1f" "C") + '\x1e';

    if (signature != m_printers_signature) {
        m_printers_signature = signature;
        m_printer_combo->Clear();
        m_printer_ids.clear();
        for (const auto &[dev_id, obj] : machines) {
            wxString label = printer_name(obj);
            if (obj->is_lan_mode_printer())
                label += " (LAN)";
            m_printer_combo->Append(label);
            m_printer_ids.push_back(dev_id);
        }
        m_last_selected = "\x1f"; // force la resélection ci-dessous
    }

    MachineObject    *selected    = dev != nullptr ? dev->get_selected_machine() : nullptr;
    const std::string selected_id = selected != nullptr ? selected->get_dev_id() : std::string();
    if (selected_id != m_last_selected) {
        m_last_selected = selected_id;
        const auto it   = std::find(m_printer_ids.begin(), m_printer_ids.end(), selected_id);
        m_printer_combo->SetSelection(it == m_printer_ids.end() ? -1 : int(it - m_printer_ids.begin()));
    }
}

void GCodeConsoleDialog::refresh_status()
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev != nullptr ? dev->get_selected_machine() : nullptr;

    wxString status;
    if (m_printer_ids.empty())
        status = _L("No printer: sign in, or connect a LAN printer from the Device tab.");
    else if (obj == nullptr)
        status = _L("Choose a printer.");
    else if (!obj->is_connected())
        status = _L("Not connected (connecting...)");
    else {
        DevExtderSystem *extruders = obj->GetExtderSystem();
        DevBed          *bed       = obj->GetBed();
        if (extruders != nullptr)
            status += wxString::Format("%s %d/%d", _L("Nozzle"), extruders->GetNozzleTempCurrent(MAIN_EXTRUDER_ID),
                                       extruders->GetNozzleTempTarget(MAIN_EXTRUDER_ID)) + deg_c();
        if (bed != nullptr)
            status += dot_sep() + wxString::Format("%s %d/%d", _L("Bed"), int(bed->GetBedTemp() + 0.5f), int(bed->GetBedTempTarget() + 0.5f)) + deg_c();
        if (const auto chamber = obj->GetChamber(); chamber && chamber->SupportChamberTempDisplay())
            status += dot_sep() + wxString::Format("%s %d", _L("Chamber"), int(chamber->GetChamberTemp() + 0.5f)) + deg_c();
        status += dot_sep();
        if (obj->is_in_printing())
            status += obj->print_status == "PAUSE" ? _L("print paused") : _L("printing");
        else
            status += _L("ready");
    }
    if (m_status->GetLabel() != status) {
        m_status->SetLabel(status);
        m_status->SetToolTip(status);
    }

    const bool     park_known = !park_gcode(obj).empty();
    const wxString park_tip   = park_known || obj == nullptr ?
                                    _L("Move the toolhead above the purge chute (X54 Y265), where it goes during a filament change. X1 and P1 series.") :
                                    _L("Park position known only for the X1 and P1 series.");
    m_park_btn->Enable(park_known && obj->is_connected());
    if (m_park_btn->GetToolTipText() != park_tip)
        m_park_btn->SetToolTip(park_tip);
}

void GCodeConsoleDialog::expire_pending()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (now - it->second.sent > std::chrono::seconds(ACK_TIMEOUT_S)) {
            append_log(wxString::Format(_L("%s: no acknowledgement after %d s (the command may still have been executed)."),
                                        it->second.printer, ACK_TIMEOUT_S),
                       LogKind::Error);
            it = m_pending.erase(it);
        } else
            ++it;
    }
}

void GCodeConsoleDialog::send_input()
{
    const wxString                 text  = m_input->GetValue();
    const std::vector<std::string> lines = prepare_lines(text.ToUTF8().data());
    if (lines.empty()) {
        m_input->Clear();
        return;
    }
    if (lines.size() > MAX_LINES) {
        append_log(wxString::Format(_L("Too many lines (%d, at most %d): print a G-code file instead."), int(lines.size()), int(MAX_LINES)),
                   LogKind::Error);
        return;
    }
    if (send_lines(lines)) {
        history_add(text);
        m_input->Clear();
    }
}

void GCodeConsoleDialog::send_park()
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev != nullptr ? dev->get_selected_machine() : nullptr;
    if (obj == nullptr) {
        append_log(_L("No printer selected: nothing was sent."), LogKind::Error);
        return;
    }
    const std::vector<std::string> lines = park_gcode(obj);
    if (lines.empty()) {
        append_log(wxString::Format(_L("Park: position not known for this printer model (%s)."), obj->get_printer_type_display_str()),
                   LogKind::Error);
        return;
    }
    // En pleine impression, la tête quitterait la pièce : refusé (en pause, simple confirmation, voir send_lines).
    if (obj->is_in_printing() && obj->print_status != "PAUSE") {
        append_log(_L("Park is not available while printing: pause or stop the print first."), LogKind::Error);
        return;
    }
    // Les déplacements Y sont absolus : il faut que l'imprimante ait fait son origine (comme les flèches de l'onglet Imprimante).
    if (const auto axis = obj->GetAxis(); axis && (!axis->IsAxisAtHomeX() || !axis->IsAxisAtHomeY())) {
        append_log(_L("Park: the X/Y axes are not homed yet. Home the printer first (home button of the axis controls in the Device tab, or G28)."),
                   LogKind::Error);
        return;
    }
    append_log(_L("Park: above the purge chute (X54 Y265), as during a filament change."), LogKind::Info);
    send_lines(lines);
}

bool GCodeConsoleDialog::send_lines(const std::vector<std::string> &lines)
{
    // Sinon les commandes partiraient vers l'ancienne imprimante, encore sélectionnée.
    if (!m_switch_pending.empty()) {
        append_log(_L("Printer change in progress: nothing was sent."), LogKind::Error);
        return false;
    }
    DeviceManager *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev != nullptr ? dev->get_selected_machine() : nullptr;
    if (obj == nullptr) {
        append_log(_L("No printer selected: nothing was sent."), LogKind::Error);
        return false;
    }
    const wxString name = printer_name(obj);
    if (!obj->is_connected()) {
        append_log(wxString::Format(_L("%s is not connected: nothing was sent."), name), LogKind::Error);
        return false;
    }
    if (obj->is_in_printing()) {
        MessageDialog dlg(this,
                          wxString::Format(_L("A print is in progress on %s.\nG-code sent now is executed during the print "
                                              "(moves, temperatures, fans...) and can ruin it.\n\nSend anyway?"),
                                           name),
                          _L("G-code console"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
        if (dlg.ShowModal() != wxID_YES)
            return false;
    }

    const wxString time = wxDateTime::Now().Format("%H:%M:%S");
    for (const std::string &line : lines)
        append_log(time + "  " + name + " > " + wxString::FromUTF8(line.c_str()), LogKind::Sent);

    for (size_t begin = 0; begin < lines.size(); begin += MAX_LINES_PER_MESSAGE) {
        const size_t end = std::min(lines.size(), begin + MAX_LINES_PER_MESSAGE);
        if (!send_block(obj, std::vector<std::string>(lines.begin() + begin, lines.begin() + end)))
            break;
    }
    if (!m_timer.IsRunning())
        m_timer.Start(TIMER_PERIOD_MS);
    return true;
}

bool GCodeConsoleDialog::send_block(MachineObject *obj, const std::vector<std::string> &lines)
{
    std::string param;
    for (const std::string &line : lines)
        param += line + "\n";

    // Comme MachineObject::publish_gcode(), sans la télémétrie (qui contiendrait le G-code saisi),
    // et en gardant le numéro de séquence pour reconnaître l'accusé de réception.
    const int      sequence_id = MachineObject::m_sequence_id++;
    nlohmann::json j;
    j["print"]["command"]     = "gcode_line";
    j["print"]["param"]       = param;
    j["print"]["sequence_id"] = std::to_string(sequence_id);
    if (NetworkAgent *agent = wxGetApp().getAgent())
        j["print"]["user_id"] = agent->get_user_id();

    const int ret = obj->publish_json(j);
    if (ret != 0) {
        append_log(wxString::Format(_L("%s: sending failed (code %d)."), printer_name(obj), ret), LogKind::Error);
        return false;
    }
    m_pending[sequence_id] = Pending{obj->get_dev_id(), printer_name(obj), lines.size(), std::chrono::steady_clock::now()};
    return true;
}

void GCodeConsoleDialog::on_ack(const std::string &dev_id, int sequence_id, const std::string &result, const std::string &reason, int err_code)
{
    const auto it = m_pending.find(sequence_id);
    if (it == m_pending.end() || it->second.dev_id != dev_id)
        return; // commande d'une autre fenêtre de l'appli, ou d'un autre logiciel

    const std::string res = boost::algorithm::to_lower_copy(result);
    if (err_code == 0 && (res.empty() || res == "success" || res == "ok")) {
        append_log(wxString::Format(_L("%s: accepted"), it->second.printer), LogKind::Ok);
    } else {
        wxString text = wxString::Format(_L("%s: refused"), it->second.printer);
        if (!reason.empty() && boost::algorithm::to_lower_copy(reason) != res)
            text += " (" + wxString::FromUTF8(reason.c_str()) + ")";
        else if (!result.empty() && res != "fail" && res != "failed")
            text += " (" + wxString::FromUTF8(result.c_str()) + ")";
        if (err_code != 0)
            text += wxString::Format(" [0x%08X]", unsigned(err_code));
        append_log(text, LogKind::Error);
    }
    m_pending.erase(it);
}

void GCodeConsoleDialog::history_step(int direction)
{
    if (m_history.empty())
        return;
    if (m_history_pos < 0) {
        if (direction > 0)
            return;
        m_history_draft = m_input->GetValue();
        m_history_pos   = int(m_history.size()) - 1;
    } else {
        const int pos = m_history_pos + direction;
        if (pos < 0)
            return;
        if (pos >= int(m_history.size())) {
            m_history_pos = -1;
            m_input->ChangeValue(m_history_draft);
            m_input->SetInsertionPointEnd();
            return;
        }
        m_history_pos = pos;
    }
    m_input->ChangeValue(m_history[m_history_pos]);
    m_input->SetInsertionPointEnd();
}

void GCodeConsoleDialog::history_add(const wxString &entry)
{
    wxString text = entry;
    text.Trim(true).Trim(false);
    m_history_pos = -1;
    m_history_draft.clear();
    if (text.empty() || (!m_history.empty() && m_history.back() == text))
        return;
    // Orio : un programme entier (onglet « Dessin G-code ») n'est pas gardé dans l'historique,
    // enregistré dans la configuration.
    if (text.length() > HISTORY_ENTRY_MAX)
        return;
    m_history.push_back(text);
    if (m_history.size() > HISTORY_MAX)
        m_history.erase(m_history.begin(), m_history.begin() + (m_history.size() - HISTORY_MAX));
    save_history();
}

void GCodeConsoleDialog::load_history()
{
    AppConfig *config = wxGetApp().app_config;
    if (config == nullptr)
        return;
    const std::string stored = config->get(HISTORY_KEY);
    if (stored.empty())
        return;
    try {
        const nlohmann::json j = nlohmann::json::parse(stored);
        if (j.is_array())
            for (const auto &entry : j)
                if (entry.is_string())
                    m_history.push_back(wxString::FromUTF8(entry.get<std::string>().c_str()));
    } catch (...) {
        m_history.clear();
    }
}

void GCodeConsoleDialog::save_history() const
{
    AppConfig *config = wxGetApp().app_config;
    if (config == nullptr)
        return;
    nlohmann::json j = nlohmann::json::array();
    for (const wxString &entry : m_history)
        j.push_back(std::string(entry.ToUTF8().data()));
    config->set(HISTORY_KEY, j.dump());
}

void GCodeConsoleDialog::append_log(const wxString &text, LogKind kind)
{
    const bool dark = wxGetApp().dark_mode();
    wxColour   colour;
    switch (kind) {
    case LogKind::Info:  colour = wxColour(144, 144, 144); break;
    case LogKind::Sent:  colour = dark ? wxColour(239, 239, 240) : wxColour(38, 46, 48); break;
    case LogKind::Ok:    colour = wxColour(8, 119, 232); break;
    case LogKind::Error: colour = wxColour(225, 71, 71); break;
    }
    m_log->SetDefaultStyle(wxTextAttr(colour));
    if (!m_log->IsEmpty())
        m_log->AppendText("\n");
    m_log->AppendText(text);
    m_log->ShowPosition(m_log->GetLastPosition());
}

}} // namespace Slic3r::GUI
