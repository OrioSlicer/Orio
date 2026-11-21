#include "SendPlatesDialog.hpp"

#include "AMSMaterialsSetting.hpp"
#include "AmsMappingPopup.hpp"
#include "BitmapCache.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "SelectMachine.hpp"
#include "DeviceManager.hpp"
#include "wxExtensions.hpp"
#include "DeviceCore/DevDefs.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevFilaSwitch.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevMapping.h"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"
#include "../Utils/ASCIIFolding.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/wrapsizer.h>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iterator>
#include <set>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

// Vignette du plateau (lignes rangées de bas en haut), carrée ; transparente si le plateau n'en a pas.
static wxBitmap plate_bitmap(const ThumbnailData &data, int size)
{
    if (!data.is_valid()) {
        wxImage image(size, size);
        image.InitAlpha();
        std::memset(image.GetAlpha(), 0, size_t(size) * size_t(size));
        return wxBitmap(image);
    }
    wxImage image(int(data.width), int(data.height));
    image.InitAlpha();
    for (unsigned int r = 0; r < data.height; ++r)
        for (unsigned int c = 0; c < data.width; ++c) {
            const unsigned char *px = data.pixels.data() + 4 * ((data.height - 1 - r) * data.width + c);
            image.SetRGB(int(c), int(r), px[0], px[1], px[2]);
            image.SetAlpha(int(c), int(r), px[3]);
        }
    image.Rescale(size, size, wxIMAGE_QUALITY_HIGH);
    return wxBitmap(image);
}

// Une tuile se choisit d'un clic n'importe où dessus, vignette et textes compris.
static void bind_click(wxWindow *win, const std::function<void()> &on_click)
{
    win->SetCursor(wxCursor(wxCURSOR_HAND));
    win->Bind(wxEVT_LEFT_DOWN, [on_click](wxMouseEvent &) { on_click(); });
    for (wxWindowList::compatibility_iterator node = win->GetChildren().GetFirst(); node; node = node->GetNext())
        bind_click(node->GetData(), on_click);
}

static void set_tree_tooltip(wxWindow *win, const wxString &tip)
{
    win->SetToolTip(tip);
    for (wxWindowList::compatibility_iterator node = win->GetChildren().GetFirst(); node; node = node->GetNext())
        set_tree_tooltip(node->GetData(), tip);
}

// Imprimantes du réseau local d'un modèle (adresse IP et code d'accès connus), par nom.
static std::vector<MachineObject *> lan_printers(const std::string &model_id)
{
    std::vector<MachineObject *> found;
    if (DeviceManager *dev = wxGetApp().getDeviceManager())
        for (const auto &[dev_id, obj] : dev->get_my_machine_list())
            if (obj != nullptr && obj->printer_type == model_id && !obj->get_dev_ip().empty() && obj->has_access_right())
                found.push_back(obj);
    std::sort(found.begin(), found.end(), [](const MachineObject *a, const MachineObject *b) { return a->get_dev_name() < b->get_dev_name(); });
    return found;
}

// Modèle et buse(s) sous le nom de l'imprimante, comme la carte de la fenêtre d'impression (_get_tips de SelectMachine.cpp).
static wxString printer_tips(MachineObject *obj)
{
    wxString               tips = obj->get_printer_type_display_str();
    const auto             ext  = obj->GetExtderSystem();
    if (ext->GetTotalExtderCount() == 1)
        tips += "  " + wxString::FromDouble(ext->GetNozzleDiameter(0)) + "mm";
    else if (ext->GetTotalExtderCount() == 2)
        tips += "  " + wxString::FromDouble(ext->GetNozzleDiameter(1)) + "/" + wxString::FromDouble(ext->GetNozzleDiameter(0)) + "mm";
    return tips;
}

// Comme SelectMachineDialog::get_filament_mapping_show_type : les bobines proposées pour un filament.
static ShowType mapping_show_type(MachineObject *obj, const std::vector<int> &fmap, int id)
{
    try {
        if (obj->GetExtderSystem()->GetTotalExtderCount() < 2)
            return ShowType::RIGHT;
        if (obj->GetFilaSwitch() && obj->GetFilaSwitch()->IsInstalled())
            return ShowType::LEFT_AND_RIGHT_DYNAMIC;
        if (fmap.at(id) == 1)
            return ShowType::LEFT;
        if (fmap.at(id) == 2)
            return ShowType::RIGHT;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": exception: " << e.what();
    }
    return ShowType::LEFT_AND_RIGHT;
}

void SendPlatesDialog::show(Plater *plater, bool print, int plate_index)
{
    SendPlatesDialog dlg(plater, print, plate_index);
    if (dlg.m_groups.empty()) {
        MessageDialog(plater, _L("No sliced plate to send: slice the plates first."), print ? _L("Print") : _L("Send"),
                      wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    dlg.ShowModal();
}

std::string SendPlatesDialog::ftp_upload(const std::string &ip, const std::string &access_code, bool ssl, std::string &remote_path,
                                         const fs::path &file, const std::function<bool(int)> &on_percent)
{
    boost::nowide::ifstream in(into_u8(from_path(file)), std::ios::binary);
    if (!in)
        return "cannot read " + into_u8(from_path(file));
    in.seekg(0, std::ios::end);
    const curl_off_t size = curl_off_t(in.tellg());
    in.seekg(0);

    CURL *curl = curl_easy_init();
    if (curl == nullptr)
        return "curl_easy_init failed";

    std::string url = std::string(ssl ? "ftps://" : "ftp://") + ip + (ssl ? ":990/" : "/");
    std::vector<std::string> parts;
    boost::split(parts, remote_path, boost::is_any_of("/"));
    for (size_t i = 0; i < parts.size(); ++i) {
        char *escaped = curl_easy_escape(curl, parts[i].c_str(), int(parts[i].size()));
        url += escaped;
        curl_free(escaped);
        if (i + 1 < parts.size())
            url += '/';
    }

    using ReadFn     = size_t (*)(char *, size_t, size_t, void *);
    using ProgressFn = int (*)(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t);
    ReadFn read_fn = [](char *buffer, size_t size, size_t count, void *stream) -> size_t {
        auto *s = static_cast<std::istream *>(stream);
        s->read(buffer, std::streamsize(size * count));
        return size_t(s->gcount());
    };
    ProgressFn progress_fn = [](void *cb, curl_off_t, curl_off_t, curl_off_t total, curl_off_t now) -> int {
        return (*static_cast<const std::function<bool(int)> *>(cb))(total > 0 ? int(now * 100 / total) : 0) ? 0 : 1;
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, "bblp");
    curl_easy_setopt(curl, CURLOPT_PASSWORD, access_code.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_fn);
    curl_easy_setopt(curl, CURLOPT_READDATA, static_cast<std::istream *>(&in));
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, size);
    curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, long(CURLFTP_CREATE_DIR));
    curl_easy_setopt(curl, CURLOPT_FTP_USE_EPSV, 0L);
    // Certificat auto-signé de l'imprimante, sur le réseau local seulement.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_fn);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::function<bool(int)> *>(&on_percent));

    const CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    BOOST_LOG_TRIVIAL(info) << "SendPlatesDialog: upload of " << remote_path << " to " << ip << ": " << curl_easy_strerror(res);
    // Dossier refusé (H2S : « Access denied ») : à la racine de la carte, où l'envoi d'une impression dépose aussi.
    if (res == CURLE_REMOTE_ACCESS_DENIED && remote_path.find('/') != std::string::npos) {
        remote_path = remote_path.substr(remote_path.rfind('/') + 1);
        return ftp_upload(ip, access_code, ssl, remote_path, file, on_percent);
    }
    return res == CURLE_OK ? std::string() : std::string(curl_easy_strerror(res));
}

SendPlatesDialog::SendPlatesDialog(Plater *plater, bool print, int plate_index)
    : DPIDialog(plater, wxID_ANY,
                plate_index == PLATE_ALL_IDX ? (print ? _L("Print all") : _L("Send all")) : (print ? _L("Print plate") : _L("Send")),
                wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_plater(plater)
    , m_print(print)
    // Noms sans accents sur l'imprimante (« Pièce » → « Piece ») : son FTP et ses commandes d'impression ne les prennent pas à coup sûr.
    , m_project(fold_utf8_to_ascii(plater->plate_files_project_name()))
{
    // Plateaux tranchés, regroupés par modèle de machine : celle de leur feuillet, sinon celle du projet.
    PresetBundle &bundle          = *wxGetApp().preset_bundle;
    Preset       &project_printer = bundle.printers.get_edited_preset();
    const std::string project_model_id = project_printer.get_printer_type(&bundle);
    auto model_id_of = [&bundle](const std::string &printer_model) {
        for (const auto &vendor : bundle.vendors)
            for (const auto &model : vendor.second.models)
                if (model.name == printer_model)
                    return model.model_id;
        return std::string();
    };
    PartPlateList &plates = plater->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        if (plate_index != PLATE_ALL_IDX && i != plate_index)
            continue;
        PartPlate *plate = plates.get_plate(i);
        if (plate->empty() || plate->is_all_instances_unprintable() || !plate->is_slice_result_ready_for_export())
            continue;
        const Preset     *machine       = plate->sheet_machine();
        const std::string printer_model = (machine != nullptr ? machine->config : project_printer.config).opt_string("printer_model");
        const std::string model_id      = machine != nullptr ? model_id_of(printer_model) : project_model_id;
        auto it = std::find_if(m_groups.begin(), m_groups.end(), [&model_id](const Group &g) { return g.model_id == model_id; });
        if (it == m_groups.end()) {
            Group group;
            group.model_id   = model_id;
            group.model_name = from_u8(boost::replace_first_copy(printer_model, "Bambu Lab ", ""));
            m_groups.push_back(std::move(group));
            it = std::prev(m_groups.end());
        }
        Row row;
        row.plate = i;
        row.file   = fold_utf8_to_ascii(plater->plate_file_name(i));
        row.remote = m_project + "/" + row.file;
        it->rows.push_back(row);
    }
    if (m_groups.empty())
        return;
    // Impression : une section par imprimante du modèle, autant que de plateaux, chacune lance le sien (fill_printers).
    // ponytail: sections comptées à l'ouverture ; « Actualiser » n'en ajoute pas pour une imprimante apparue depuis.
    if (m_print)
        for (size_t gi = 0; gi < m_groups.size(); ++gi) {
            const size_t n = std::min(lan_printers(m_groups[gi].model_id).size(), m_groups[gi].rows.size());
            for (size_t k = 1; k < n; ++k) {
                Group copy = m_groups[gi];
                copy.slot  = k;
                m_groups.insert(m_groups.begin() + gi + k, std::move(copy));
            }
            gi += n > 1 ? n - 1 : 0;
        }

    if (DeviceManager *dev = wxGetApp().getDeviceManager())
        if (MachineObject *selected = dev->get_selected_machine())
            m_orig_dev_id = selected->get_dev_id();

    std::string icon_path = (boost::format("%1%/images/BambuStudioTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));
    // Vignettes des plateaux, comme la fenêtre d'impression.
    m_plater->update_all_plate_thumbnails(false);
    const bool inches = wxGetApp().app_config->get("use_inches") == "1";

    SetBackgroundColour(*wxWHITE);
    const bool     dark = wxGetApp().dark_mode();
    const wxColour text = dark ? wxColour(239, 239, 240) : wxColour(38, 46, 48);
    const wxColour grey = dark ? wxColour(176, 176, 176) : wxColour(74, 81, 84);

    // Lecture des AMS et choix d'une bobine, comme la fenêtre d'impression.
    m_ams_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { on_ams_timer(); }, m_ams_timer.GetId());
    m_launch_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { launch_next(); }, m_launch_timer.GetId());
    m_tray_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { on_tray_timer(); }, m_tray_timer.GetId());
    m_mapping_popup = new AmsMapingPopup(this);
    m_mapping_popup->EnableExtMappingFilaTypeCheck(false);
    Bind(EVT_SET_FINISH_MAPPING, &SendPlatesDialog::on_finish_mapping, this);

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);

    // Ce que deviennent les plateaux.
    auto *hint = new Label(this, Label::Body_13,
                           wxString::Format(m_print ? _L("Everything is deposited in a folder \"%s\" of each printer, then one plate starts on each printer.") :
                                                      _L("On each printer, a folder \"%s\" receives one file per plate."),
                                            from_u8(m_project)));
    hint->SetForegroundColour(grey);
    hint->Wrap(FromDIP(720));
    main_sizer->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    // Une section par modèle de machine : la carte de son imprimante, puis ses plateaux.
    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scroll->SetBackgroundColour(*wxWHITE);
    m_scroll->SetScrollRate(0, FromDIP(10));
    auto *list = new wxBoxSizer(wxVERTICAL);

    auto make_tile = [this]() {
        auto *tile = new StaticBox(m_scroll);
        tile->SetCornerRadius(FromDIP(4));
        tile->SetMinSize(wxSize(FromDIP(146), FromDIP(68)));
        tile->SetMaxSize(wxSize(FromDIP(146), -1));
        tile->SetBackgroundColor(StateColor(*wxWHITE));
        tile->SetBackgroundColour(*wxWHITE); // fond des textes et des images
        tile->SetBorderColor(StateColor(wxColour("#EEEEEE")));
        tile->SetBorderWidth(FromDIP(1));
        return tile;
    };

    for (size_t gi = 0; gi < m_groups.size(); ++gi) {
        Group         &g          = m_groups[gi];
        const wxString model_name = g.model_name.empty() ? from_u8(g.model_id) : g.model_name;

        // En-tête de section, comme « Imprimante » dans la fenêtre d'impression.
        auto *head  = new wxBoxSizer(wxHORIZONTAL);
        auto *title = new Label(m_scroll, Label::Head_13, model_name);
        title->SetForegroundColour(wxColour(0x90, 0x90, 0x90));
        auto *line = new wxPanel(m_scroll, wxID_ANY);
        line->SetMinSize(wxSize(-1, FromDIP(1)));
        line->SetBackgroundColour(wxColour("#EEEEEE"));
        head->Add(title, 0, wxALIGN_CENTER_VERTICAL);
        head->Add(line, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        list->Add(head, 0, wxEXPAND);
        list->AddSpacer(FromDIP(8));

        // Carte de l'imprimante (PrinterInfoBox) : photo, nom, modèle et buse.
        auto *card = new StaticBox(m_scroll);
        card->SetMinSize(wxSize(FromDIP(338), FromDIP(68)));
        card->SetMaxSize(wxSize(FromDIP(338), FromDIP(68)));
        card->SetBorderColor(wxColour("#CECECE"));
        g.combo = new ComboBox(card, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        g.combo->SetBorderWidth(0);
        g.combo->SetMinSize(wxSize(FromDIP(300), FromDIP(60)));
        g.combo->SetMaxSize(wxSize(FromDIP(300), FromDIP(60)));
        g.combo->SetBackgroundColor(*wxWHITE);
        g.combo->Bind(wxEVT_COMBOBOX, [this, gi](wxCommandEvent &) {
            if (m_print) {
                m_groups[gi].ams = Ams::Idle;
                read_ams(gi);
            } else {
                // Autre imprimante : les états d'envoi de la précédente s'effacent.
                for (Row &row : m_groups[gi].rows) {
                    row.sent = false;
                    row.error.clear();
                    row.status->SetLabel(wxEmptyString);
                }
                relayout();
            }
            update_tiles(gi);
            update_summary();
            update_filaments(gi);
        });
        g.missing = new Label(card, Label::Body_12, wxString::Format(_L("No %s found on the local network: these plates will not be sent."), model_name));
        g.missing->SetForegroundColour(text);
        g.missing->Wrap(FromDIP(290));
        g.refresh = new ScalableButton(card, wxID_ANY, "refresh_printer", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
        g.refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { fill_printers(); });
        auto *cs = new wxBoxSizer(wxHORIZONTAL);
        cs->Add(0, 0, 0, wxLEFT, FromDIP(7));
        cs->Add(g.combo, 0, wxALIGN_CENTER);
        cs->Add(g.missing, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
        cs->Add(g.refresh, 0, wxALIGN_CENTER);
        card->SetSizer(cs);

        // Une tuile par plateau : vignette, numéro, durée et poids (remplacés par l'état de l'envoi).
        const int tiles = int(g.rows.size()) + (m_print ? 1 : 0);
        // ponytail: 3 tuiles par ligne au plus ; selon la largeur de la fenêtre si des projets en ont beaucoup.
        auto *grid = new wxGridSizer(std::min(tiles, 3), FromDIP(4), FromDIP(4));
        for (size_t ri = 0; ri < g.rows.size(); ++ri) {
            Row       &row   = g.rows[ri];
            PartPlate *plate = plates.get_plate(row.plate);
            row.tile         = make_tile();
            auto *thumb = new wxStaticBitmap(row.tile, wxID_ANY, plate_bitmap(plate->thumbnail_data, FromDIP(48)));
            thumb->SetBackgroundColour(*wxWHITE);
            auto *name = new Label(row.tile, Label::Head_13, wxString::Format(_L("Plate %d"), row.plate + 1));
            name->SetForegroundColour(text);

            wxString time, weight;
            if (GCodeProcessorResult *result = plate->get_slice_result()) {
                time = from_u8(short_time(get_time_dhms(result->print_statistics.modes[0].time)));
                PlateData data;
                data.parse_filament_info(result);
                float grams = 0.f;
                for (const FilamentInfo &f : data.slice_filaments_info)
                    grams += f.used_g;
                weight = inches ? wxString::Format("%.2f oz", grams * 0.035274) : wxString::Format("%.2f g", grams);
            }
            auto stat = [&](const std::string &icon, const wxString &value) {
                auto *s   = new wxBoxSizer(wxHORIZONTAL);
                auto *bmp = new wxStaticBitmap(row.tile, wxID_ANY, create_scaled_bitmap(icon, this, 12));
                bmp->SetBackgroundColour(*wxWHITE);
                auto *label = new Label(row.tile, Label::Body_12, value);
                label->SetForegroundColour(grey);
                s->Add(bmp, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
                s->Add(label, 0, wxALIGN_CENTER_VERTICAL);
                return s;
            };
            row.info = new wxBoxSizer(wxVERTICAL);
            row.info->Add(stat("print-time", time), 0, wxTOP, FromDIP(2));
            row.info->Add(stat("print-weight", weight), 0, wxTOP, FromDIP(2));
            row.status = new Label(row.tile, Label::Body_12, wxEmptyString);
            row.status->SetForegroundColour(grey);
            row.status->Hide();

            auto *col = new wxBoxSizer(wxVERTICAL);
            col->Add(name);
            col->Add(row.info);
            col->Add(row.status, 0, wxTOP, FromDIP(2));
            auto *ts = new wxBoxSizer(wxHORIZONTAL);
            ts->Add(thumb, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
            ts->Add(col, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
            row.tile->SetSizer(ts);
            bind_click(row.tile, [this, gi, ri] { pick_tile(gi, ri); });
            grid->Add(row.tile, 0, wxEXPAND);
            // Envoi : tous les plateaux ; impression : le premier est lancé, le deuxième sur la deuxième imprimante…
            row.on = !m_print || ri == g.slot;
        }
        if (m_print) {
            g.none = make_tile();
            auto *label = new Label(g.none, Label::Head_13, _L("None, only deposit"));
            label->SetForegroundColour(text);
            label->Wrap(FromDIP(126));
            auto *ns = new wxBoxSizer(wxHORIZONTAL);
            ns->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(10));
            g.none->SetSizer(ns);
            bind_click(g.none, [this, gi, n = g.rows.size()] { pick_tile(gi, n); });
            grid->Add(g.none, 0, wxEXPAND);
        }

        auto *body = new wxBoxSizer(wxHORIZONTAL);
        body->Add(card, 0, wxALIGN_TOP);
        body->AddSpacer(FromDIP(12));
        body->Add(grid, 0, wxALIGN_TOP);
        list->Add(body, 0);
        // Impression : les filaments du plateau lancé et leurs bobines (update_filaments).
        g.filaments = new wxBoxSizer(wxVERTICAL);
        list->Add(g.filaments, 0, wxEXPAND);
        if (gi + 1 < m_groups.size())
            list->AddSpacer(FromDIP(16));
    }
    m_scroll->SetSizer(list);
    main_sizer->Add(m_scroll, 1, wxEXPAND | wxALL, FromDIP(20));

    // Pied : un trait, le résumé et les boutons de la fenêtre d'impression.
    auto *foot_line = new wxPanel(this, wxID_ANY);
    foot_line->SetMinSize(wxSize(-1, FromDIP(1)));
    foot_line->SetBackgroundColour(wxColour("#EEEEEE"));
    main_sizer->Add(foot_line, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    main_sizer->AddSpacer(FromDIP(14));

    const StateColor btn_bg_blue(std::pair<wxColour, int>(wxColour(5, 96, 194), StateColor::Pressed),
                                 std::pair<wxColour, int>(wxColour(43, 140, 242), StateColor::Hovered),
                                 std::pair<wxColour, int>(wxColour(8, 116, 224), StateColor::Normal));
    const StateColor btn_bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                                  std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                                  std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    auto make_button = [this, &btn_bg_blue, &btn_bg_white](const wxString &label, bool primary) {
        Button *btn = new Button(this, label);
        btn->SetBackgroundColor(primary ? btn_bg_blue : btn_bg_white);
        btn->SetBorderColor(primary ? wxColour(8, 116, 224) : wxColour(38, 46, 48));
        if (primary)
            btn->SetTextColor(wxColour("#FFFFFE"));
        btn->SetFont(Label::Body_12);
        btn->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
        btn->SetCornerRadius(FromDIP(4));
        return btn;
    };
    m_summary = new Label(this, Label::Body_13, wxEmptyString);
    m_summary->SetForegroundColour(grey);
    m_btn_cancel = make_button(_L("Cancel"), false);
    m_btn_ok     = make_button(m_print ? _L("Deposit and start") : _L("Send"), true);
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { start(); });
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (m_running)
            m_cancel = true;
        else
            EndModal(wxID_CANCEL);
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &e) {
        // Les fichiers en cours d'envoi sont lus dans le dossier temporaire de ce dialogue.
        if (m_running && e.CanVeto()) {
            m_cancel = true;
            e.Veto();
            return;
        }
        e.Skip();
    });
    auto *foot = new wxBoxSizer(wxHORIZONTAL);
    foot->Add(m_summary, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    foot->Add(m_btn_cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    foot->Add(m_btn_ok, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(foot, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));

    SetSizer(main_sizer);
    fill_printers();
    for (size_t gi = 0; gi < m_groups.size(); ++gi)
        update_filaments(gi);
    main_sizer->SetSizeHints(this);
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

SendPlatesDialog::~SendPlatesDialog()
{
    m_ams_timer.Stop();
    m_tray_timer.Stop();
    // Rien de lancé : l'imprimante sélectionnée à l'ouverture revient (la lecture des AMS en a sélectionné d'autres).
    if (!m_launched && m_switched && !m_orig_dev_id.empty() && !BackgroundPrintJobs::lan_connection_busy())
        if (DeviceManager *dev = wxGetApp().getDeviceManager()) {
            MachineObject *selected = dev->get_selected_machine();
            if (selected == nullptr || selected->get_dev_id() != m_orig_dev_id)
                dev->set_selected_machine(m_orig_dev_id);
        }
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
    // Un plateau lancé est encore lu par son envoi (PrintJob, en arrière-plan) : son dossier reste, effacé plus tard.
    if (!m_temp_dir.empty() && !m_launched) {
        boost::system::error_code ec;
        fs::remove_all(m_temp_dir, ec);
    }
}

void SendPlatesDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    for (Button *btn : {m_btn_cancel, m_btn_ok}) {
        btn->SetMinSize(wxSize(FromDIP(80), FromDIP(32)));
        btn->SetCornerRadius(FromDIP(4));
        btn->Rescale();
    }
    for (Group &g : m_groups) {
        g.combo->Rescale();
        g.refresh->msw_rescale();
    }
    m_mapping_popup->msw_rescale();
    Layout();
    Refresh();
}

// Les tuiles montrent l'état de leur envoi, les cartes leur menu ou « aucune imprimante », les lignes de filaments
// changent avec la lecture des AMS : la fenêtre grandit avec elles, puis la liste défile.
void SendPlatesDialog::relayout()
{
    for (Group &g : m_groups) {
        for (Row &row : g.rows) {
            const wxString status = row.status->GetLabel();
            const bool     shown  = !status.empty();
            row.status->Show(shown);
            if (shown)
                row.status->Wrap(FromDIP(80));
            row.tile->GetSizer()->Show(row.info, !shown, true);
            wxString tip = from_u8(row.remote);
            if (shown)
                tip += "\n" + status;
            if (!row.error.empty())
                tip += ": " + from_u8(row.error);
            set_tree_tooltip(row.tile, tip);
            row.tile->Layout();
        }
        g.combo->GetParent()->Layout();
    }
    const wxSize best = m_scroll->GetSizer()->CalcMin();
    m_scroll->SetMinSize(wxSize(std::max(best.x, FromDIP(380)), std::min(best.y, FromDIP(460))));
    m_scroll->FitInside();
    const wxSize fit = GetSizer()->ComputeFittingWindowSize(this);
    SetMinSize(fit);
    if (GetSize().x < fit.x || GetSize().y < fit.y)
        SetSize(wxSize(std::max(GetSize().x, fit.x), std::max(GetSize().y, fit.y)));
    Layout();
}

// Imprimantes du réseau local de chaque modèle (adresse IP et code d'accès connus).
void SendPlatesDialog::fill_printers()
{
    DeviceManager *dev      = wxGetApp().getDeviceManager();
    MachineObject *selected = dev != nullptr ? dev->get_selected_machine() : nullptr;

    for (size_t gi = 0; gi < m_groups.size(); ++gi) {
        Group                             &g        = m_groups[gi];
        const int                          previous = g.combo->GetSelection();
        const std::string                  keep     = previous != wxNOT_FOUND ? g.dev_ids[previous] : std::string();
        const std::vector<MachineObject *> found    = lan_printers(g.model_id);

        // Entrées de PrinterInfoBox::SetPrinters : photo de la machine, nom, modèle et buse.
        std::vector<DropDown::Item> items;
        g.dev_ids.clear();
        // À l'ouverture : l'imprimante sélectionnée, puis les suivantes dans les sections suivantes du modèle.
        int pick = wxNOT_FOUND;
        if (!found.empty()) {
            const auto it = std::find(found.begin(), found.end(), selected);
            pick          = int((size_t(it != found.end() ? it - found.begin() : 0) + g.slot) % found.size());
        }
        for (MachineObject *obj : found) {
            DropDown::Item item;
            item.text = wxString::FromUTF8(obj->get_dev_name());
            if (obj->is_lan_mode_printer())
                item.text += "(LAN)";
            item.text_static_tips = printer_tips(obj);
            try {
                item.icon          = create_scaled_bitmap("printer_preview_" + obj->printer_type, this, 32);
                item.icon_textctrl = create_scaled_bitmap("printer_preview_" + obj->printer_type, this, 52);
            } catch (const std::exception &) {
                item.icon          = create_scaled_bitmap("printer_preview_BL-P001", this, 32);
                item.icon_textctrl = create_scaled_bitmap("printer_preview_BL-P001", this, 52);
            }
            item.tip = obj->get_printer_type_display_str();
            items.push_back(item);
            g.dev_ids.push_back(obj->get_dev_id());
            if (obj->get_dev_id() == keep)
                pick = int(g.dev_ids.size()) - 1;
        }
        g.combo->Clear();
        g.combo->SetItems(items);
        g.combo->SetSelection(pick);
        g.combo->Show(!found.empty());
        g.missing->Show(found.empty());
        // À l'ouverture et à chaque actualisation : son AMS et son état sont à lire.
        if (m_print) {
            g.ams = Ams::Idle;
            read_ams(gi);
        }
        update_tiles(gi);
    }
    update_summary();
    relayout();
}

// Bordure à l'accent Orio : les plateaux envoyés ; en impression, celui qui est lancé (ou « Aucun »).
void SendPlatesDialog::update_tiles(size_t gi)
{
    Group         &g           = m_groups[gi];
    const bool     has_printer = g.combo->GetSelection() != wxNOT_FOUND;
    const wxColour accent      = wxGetApp().dark_mode() ? wxColour("#00C2FF") : wxColour("#0877E8");
    auto paint = [this, &accent](StaticBox *tile, bool on) {
        tile->SetBorderColor(StateColor(on ? accent : wxColour("#EEEEEE")));
        tile->SetBorderWidth(on ? FromDIP(2) : FromDIP(1));
        tile->Refresh();
    };
    bool any = false;
    for (Row &row : g.rows) {
        paint(row.tile, has_printer && row.on);
        any |= row.on;
    }
    if (g.none != nullptr)
        paint(g.none, has_printer && !any);
}

// ri == g.rows.size() : « Aucun, seulement déposer ».
void SendPlatesDialog::pick_tile(size_t gi, size_t ri)
{
    Group &g = m_groups[gi];
    if (m_running || m_done || g.combo->GetSelection() == wxNOT_FOUND)
        return;
    if (m_print) {
        for (size_t i = 0; i < g.rows.size(); ++i)
            g.rows[i].on = i == ri;
        if (g.ams == Ams::Idle && ri < g.rows.size())
            read_ams(gi);
        update_filaments(gi);
    } else if (ri < g.rows.size())
        g.rows[ri].on = !g.rows[ri].on;
    update_tiles(gi);
    update_summary();
}

void SendPlatesDialog::update_summary()
{
    const bool editable = !m_running && !m_done;
    int files = 0, printers = 0, starts = 0;
    for (Group &g : m_groups) {
        g.combo->Enable(editable);
        g.refresh->Enable(editable);
        if (g.combo->GetSelection() == wxNOT_FOUND)
            continue;
        int  n     = 0;
        bool start = false;
        for (const Row &row : g.rows) {
            n += m_print || row.on;
            start |= row.on;
        }
        files += n;
        printers += n > 0;
        starts += m_print && start;
    }
    m_summary->SetLabel(m_print ? wxString::Format(_L("Files: %d · printers: %d · prints started: %d"), files, printers, starts) :
                                  wxString::Format(_L("Files: %d · printers: %d · local network"), files, printers));
    m_btn_ok->Enable(files > 0 && editable);
}

MachineObject *SendPlatesDialog::group_machine(const Group &g) const
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    const int      sel = g.combo->GetSelection();
    return dev != nullptr && sel != wxNOT_FOUND && size_t(sel) < g.dev_ids.size() ? dev->get_my_machine(g.dev_ids[sel]) : nullptr;
}

void SendPlatesDialog::read_ams(size_t gi)
{
    if (std::find(m_ams_queue.begin(), m_ams_queue.end(), gi) == m_ams_queue.end())
        m_ams_queue.push_back(gi);
    if (m_ams_group < 0)
        read_next_ams();
}

// Une imprimante après l'autre : son AMS n'arrive que sur l'imprimante sélectionnée (une seule connexion LAN).
void SendPlatesDialog::read_next_ams()
{
    m_ams_timer.Stop();
    m_ams_group = -1;
    DeviceManager *dev = wxGetApp().getDeviceManager();
    while (!m_ams_queue.empty()) {
        const size_t gi = m_ams_queue.front();
        m_ams_queue.pop_front();
        Group &g = m_groups[gi];
        if (std::none_of(g.rows.begin(), g.rows.end(), [](const Row &r) { return r.on; }))
            continue;
        MachineObject *obj = group_machine(g);
        if (obj == nullptr || dev == nullptr) {
            set_ams(gi, Ams::Unavailable);
            continue;
        }
        MachineObject *selected = dev->get_selected_machine();
        if (selected == nullptr || selected->get_dev_id() != obj->get_dev_id()) {
            // Un envoi en arrière-plan tient la connexion : pas d'autre imprimante pendant ce temps.
            if (BackgroundPrintJobs::lan_connection_busy()) {
                set_ams(gi, Ams::Unavailable);
                continue;
            }
            dev->set_selected_machine(obj->get_dev_id());
            m_switched = true;
        }
        m_ams_group = int(gi);
        m_ams_ticks = 0;
        set_ams(gi, Ams::Reading);
        m_ams_timer.Start(500);
        return;
    }
}

void SendPlatesDialog::on_ams_timer()
{
    if (m_ams_group < 0) {
        m_ams_timer.Stop();
        return;
    }
    const size_t   gi       = size_t(m_ams_group);
    MachineObject *obj      = group_machine(m_groups[gi]);
    DeviceManager *dev      = wxGetApp().getDeviceManager();
    MachineObject *selected = dev != nullptr ? dev->get_selected_machine() : nullptr;
    if (obj == nullptr || selected == nullptr || selected->get_dev_id() != obj->get_dev_id())
        set_ams(gi, Ams::Unavailable);
    else if (selected->is_connected() && selected->is_info_ready())
        set_ams(gi, Ams::Ready);
    // ponytail: 20 s au plus par imprimante, en attendant son premier état complet ; plus long si des AMS tardent.
    else if (++m_ams_ticks >= 40)
        set_ams(gi, Ams::Unavailable);
    else
        return;
    read_next_ams();
}

void SendPlatesDialog::set_ams(size_t gi, Ams state)
{
    m_groups[gi].ams = state;
    update_filaments(gi);
    // Bobine cliquee sur une imprimante non connectee : sa fenetre s'ouvre une fois l'imprimante relue (apres le minuteur).
    if (int(gi) == m_pending_group && state != Ams::Reading) {
        m_pending_group = -1;
        if (state == Ams::Ready)
            CallAfter([this, gi, a = m_pending_ams, s = m_pending_slot] { edit_tray(gi, a, s); });
    }
}

// Comme SelectMachineDialog::do_ams_mapping : couleur et type, buse par buse sur les machines à deux buses.
void SendPlatesDialog::map_filaments(Group &g, MachineObject *obj)
{
    g.result.clear();
    const bool use_ams         = obj->HasAms();
    int        filament_result = 0;
    if (obj->GetExtderSystem()->GetTotalExtderCount() > 1) {
        std::vector<FilamentInfo> left, right, result_left, result_right;
        for (const FilamentInfo &f : g.slots) {
            if (f.id < 0 || f.id > int(g.fmap.size()))
                return;
            if (f.id < int(g.fmap.size())) {
                if (g.fmap[f.id] == 1)
                    left.push_back(f);
                else if (g.fmap[f.id] == 2)
                    right.push_back(f);
            }
        }
        bool has_left_ams = false, has_right_ams = false;
        for (const auto &ams : obj->GetFilaSystem()->GetAmsList()) {
            has_right_ams |= ams.second->GetBindedExtruderSet().count(MAIN_EXTRUDER_ID) != 0;
            has_left_ams |= ams.second->GetBindedExtruderSet().count(DEPUTY_EXTRUDER_ID) != 0;
        }
        // Quatre choix : AMS gauche, AMS droit, bobine externe gauche, bobine externe droite.
        const int r1 = DevMappingUtil::ams_filament_mapping(obj, left, result_left, {use_ams, false, !use_ams || !has_left_ams, false});
        const int r2 = DevMappingUtil::ams_filament_mapping(obj, right, result_right, {false, use_ams, false, !use_ams || !has_right_ams});
        g.result.resize(result_left.size() + result_right.size());
        std::merge(result_left.begin(), result_left.end(), result_right.begin(), result_right.end(), g.result.begin(),
                   [](const FilamentInfo &a, const FilamentInfo &b) { return a.id < b.id; });
        filament_result = r1 && r2;
    } else
        filament_result = DevMappingUtil::ams_filament_mapping(obj, g.slots, g.result, {false, use_ams, false, !use_ams});
    // Appariement impossible : bobines à choisir à la main, comme dans la fenêtre d'impression.
    const bool valid = DevMappingUtil::is_valid_mapping_result(obj, g.result);
    if (filament_result != 0 && filament_result != 1 && !valid)
        for (FilamentInfo &r : g.result) {
            r.tray_id  = -1;
            r.distance = 99999;
        }
}

// Impression : les filaments du plateau lancé, une case par filament (en haut sa couleur, en bas la bobine de l'AMS) ;
// un clic choisit une autre bobine.
void SendPlatesDialog::update_filaments(size_t gi)
{
    Group &g = m_groups[gi];
    g.filaments->Clear(true);
    g.items.clear();
    g.slots.clear();
    g.fmap.clear();
    g.result.clear();

    MachineObject *obj = m_print ? group_machine(g) : nullptr;
    // Son état, sous son nom : connu à la lecture de son AMS (Orio ne suit que l'imprimante sélectionnée).
    if (obj != nullptr) {
        wxString state;
        if (g.ams == Ams::Reading)
            state = _L("Connecting...");
        else if (g.ams == Ams::Ready)
            state = obj->is_in_printing() || obj->is_in_upgrading() || obj->is_system_printing() ? _L("In use") : _L("Available");
        else if (g.ams == Ams::Unavailable && !BackgroundPrintJobs::lan_connection_busy())
            state = _L("Offline");
        g.combo->SetStaticTips(printer_tips(obj) + (state.empty() ? wxString() : wxString::FromUTF8("  \xC2\xB7  ") + state), wxNullBitmap);
    }
    const auto     row = std::find_if(g.rows.begin(), g.rows.end(), [](const Row &r) { return r.on; });
    PartPlate     *plate = row != g.rows.end() ? m_plater->get_partplate_list().get_plate(row->plate) : nullptr;
    if (obj == nullptr || plate == nullptr || plate->get_slice_result() == nullptr) {
        relayout();
        return;
    }

    // Type, couleur et référence de chaque filament, comme la fenêtre d'impression (le G-code ne donne que les quantités).
    PlateData data;
    data.parse_filament_info(plate->get_slice_result());
    DynamicPrintConfig cfg     = wxGetApp().preset_bundle->full_config_secure();
    const auto        *ids     = cfg.option<ConfigOptionStrings>("filament_ids");
    const auto        *colours = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    for (FilamentInfo f : data.slice_filaments_info) {
        std::string display;
        f.type = cfg.get_filament_type(display, f.id);
        if (ids != nullptr && size_t(f.id) < ids->values.size())
            f.filament_id = ids->values[f.id];
        unsigned char rgba[4] = {255, 255, 255, 255};
        if (colours != nullptr && size_t(f.id) < colours->values.size())
            BitmapCache::parse_color4(colours->values[f.id], rgba);
        f.color = wxString::Format("#%02X%02X%02X%02X", rgba[0], rgba[1], rgba[2], rgba[3]).ToStdString();
        g.slots.push_back(f);
    }
    // ponytail: filaments et buses du projet, même pour un feuillet d'une autre machine ; le plateau ne garde pas les siens.
    g.fmap = plate->get_real_filament_maps(wxGetApp().preset_bundle->project_config);

    const bool ready = g.ams == Ams::Ready;
    if (ready)
        map_filaments(g, obj);
    const int total = obj->GetExtderSystem()->GetTotalExtderCount();
    // ponytail: buse gauche / droite connues une fois l'imprimante lue ; une seule ligne avant.
    const bool     split = ready && total > 1 && !(obj->GetFilaSwitch() && obj->GetFilaSwitch()->IsInstalled());
    const wxColour grey  = wxGetApp().dark_mode() ? wxColour(176, 176, 176) : wxColour(74, 81, 84);
    auto nozzle_of = [&g](const FilamentInfo &f) { return size_t(f.id) < g.fmap.size() ? g.fmap[f.id] : 0; };

    auto add_line = [&](const wxString &title, int nozzle) {
        if (std::none_of(g.slots.begin(), g.slots.end(), [&](const FilamentInfo &f) { return nozzle == 0 || nozzle_of(f) == nozzle; }))
            return;
        auto *line  = new wxBoxSizer(wxHORIZONTAL);
        auto *label = new Label(m_scroll, Label::Body_12, title);
        label->SetForegroundColour(grey);
        label->SetMinSize(wxSize(FromDIP(90), -1));
        auto *wrap = new wxWrapSizer(wxHORIZONTAL);
        for (const FilamentInfo &f : g.slots) {
            if (nozzle != 0 && nozzle_of(f) != nozzle)
                continue;
            auto *item = new MaterialItem(m_scroll, DevAmsTray::decode_color(f.color), from_u8(f.get_display_filament_type()), f.filament_id);
            if (ready) {
                // Comme SelectMachineDialog::sync_ams_mapping_result.
                wxColour              col(0xCE, 0xCE, 0xCE);
                wxString              tray  = "Ext";
                int                   ctype = 0;
                std::vector<wxColour> cols;
                for (const FilamentInfo &r : g.result)
                    if (r.id == f.id) {
                        tray = r.tray_id >= 0 ? wxGetApp().transition_tridid(r.tray_id, total) : wxString("-");
                        if (split && r.tray_id >= 0 && devPrinterUtil::IsVirtualSlot(r.tray_id))
                            tray = "Ext";
                        if (!r.color.empty())
                            col = DevAmsTray::decode_color(r.color);
                        for (const std::string &c : r.colors)
                            cols.push_back(DevAmsTray::decode_color(c));
                        ctype = r.ctype;
                        break;
                    }
                item->set_ams_info(col, tray, ctype, cols);
                item->SetToolTip(_L("Upper half area:  Original\nLower half area:  Filament in AMS\nAnd you can click it to modify"));
                item->Bind(wxEVT_LEFT_UP, [](wxMouseEvent &) {});
                item->Bind(wxEVT_LEFT_DOWN, [this, gi, item, id = f.id, type = f.type](wxMouseEvent &) {
                    Group &grp = m_groups[gi];
                    if (!item->m_enable || m_running || m_done)
                        return;
                    for (auto &it : grp.items)
                        it.second->on_normal();
                    item->on_selected();
                    // ponytail: l'AMS lu en dernier sur cette imprimante, même si une autre a été sélectionnée depuis.
                    MachineObject *mo = group_machine(grp);
                    if (m_mapping_popup->IsShown() || mo == nullptr || grp.ams != Ams::Ready)
                        return;
                    m_popup_group = int(gi);
                    m_mapping_popup->set_show_type(mapping_show_type(mo, grp.fmap, id));
                    m_mapping_popup->set_parent_item(item);
                    m_mapping_popup->set_current_filament_id(id);
                    m_mapping_popup->set_tag_texture(type);
                    m_mapping_popup->set_send_win(this);
                    m_mapping_popup->update(mo, grp.result);
                    m_mapping_popup->Popup();
                });
            } else {
                item->set_ams_info(wxColour(0xCE, 0xCE, 0xCE), "-");
                item->disable();
            }
            wrap->Add(item, 0, wxALL, FromDIP(5));
            g.items.emplace_back(f.id, item);
        }
        line->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        line->Add(wrap, 1, wxEXPAND);
        g.filaments->Add(line, 0, wxEXPAND | wxTOP, FromDIP(8));
    };
    if (split) {
        add_line(_L("Left nozzle"), 1);
        add_line(_L("Right nozzle"), 2);
    } else
        add_line(_L("Filament"), 0);

    // L'AMS de l'imprimante, comme la carte de l'ecran Imprimante en plus compact : un clic sur une bobine en change le
    // filament sans quitter la fenetre (edit_tray) ; "Auto-refill" montre ses bobines de secours.
    if (ready) {
        auto *line  = new wxBoxSizer(wxHORIZONTAL);
        auto *label = new Label(m_scroll, Label::Body_12, _L("AMS"));
        label->SetForegroundColour(grey);
        label->SetMinSize(wxSize(FromDIP(90), -1));
        auto *wrap     = new wxWrapSizer(wxHORIZONTAL);
        auto  add_tray = [&](const DevAmsTray &tray, int ams_id, int slot_id, int tray_id) {
            // Bobine externe toujours modifiable ; emplacement vide de l'AMS : rien a regler, comme sur l'ecran Imprimante.
            const bool            filled = tray.is_exists || devPrinterUtil::IsVirtualSlot(ams_id);
            const wxColour        col    = filled && !tray.color.empty() ? DevAmsTray::decode_color(tray.color) : wxColour(0xEE, 0xEE, 0xEE);
            std::vector<wxColour> cols;
            for (const std::string &c : tray.cols)
                cols.push_back(DevAmsTray::decode_color(c));
            auto *item = new MaterialItem(m_scroll, col, filled ? from_u8(tray.get_display_filament_type()) : wxString(), "");
            item->set_ams_info(col, wxGetApp().transition_tridid(tray_id, total), tray.ctype, cols);
            if (filled && !m_running && !m_done) {
                item->SetToolTip(_L("Click to change the filament of this slot"));
                item->Bind(wxEVT_LEFT_UP, [](wxMouseEvent &) {});
                item->Bind(wxEVT_LEFT_DOWN, [this, gi, ams_id, slot_id](wxMouseEvent &) {
                    CallAfter([this, gi, ams_id, slot_id] { edit_tray(gi, ams_id, slot_id); });
                });
            } else
                item->disable();
            wrap->Add(item, 0, wxALL, FromDIP(5));
        };
        for (const auto &[ams_id, ams] : obj->GetFilaSystem()->GetAmsList())
            if (ams != nullptr)
                for (const auto &[slot_id, tray] : ams->GetTrays())
                    if (tray != nullptr) {
                        const int a = std::atoi(ams_id.c_str()), s = std::atoi(slot_id.c_str());
                        add_tray(*tray, a, s, obj->GetFilaSystem()->GetTrayIdByAmsSlotId(a, s));
                    }
        for (size_t k = 0; k < obj->vt_slot.size() && k < 2; ++k) {
            const int a = k == 0 ? VIRTUAL_TRAY_MAIN_ID : VIRTUAL_TRAY_DEPUTY_ID;
            add_tray(obj->vt_slot[k], a, 0, a);
        }
        line->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        line->Add(wrap, 1, wxEXPAND);
        if (obj->is_support_filament_backup) {
            auto *refill = new Button(m_scroll, _L("Auto-refill"));
            refill->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                                                  std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                                                  std::pair<wxColour, int>(*wxWHITE, StateColor::Normal)));
            refill->SetBorderColor(wxColour(38, 46, 48));
            refill->SetFont(Label::Body_12);
            refill->SetMinSize(wxSize(FromDIP(80), FromDIP(26)));
            refill->SetCornerRadius(FromDIP(4));
            refill->Enable(!m_running && !m_done);
            // Comme StatusPanel::on_ams_filament_backup.
            refill->Bind(wxEVT_BUTTON, [this, gi](wxCommandEvent &) {
                if (MachineObject *mo = group_machine(m_groups[gi])) {
                    AmsReplaceMaterialDialog dlg(this);
                    dlg.update_machine_obj(mo);
                    dlg.ShowModal();
                }
            });
            line->Add(refill, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        }
        g.filaments->Add(line, 0, wxEXPAND | wxTOP, FromDIP(4));
    }

    if (!ready) {
        auto *note = new Label(m_scroll, Label::Body_12,
                               g.ams == Ams::Unavailable ? _L("AMS not read: the filaments will be chosen in the print window.") : _L("Reading the AMS..."));
        note->SetForegroundColour(grey);
        g.filaments->Add(note, 0, wxLEFT, FromDIP(90));
    }
    relayout();
}

// Bobine choisie dans la fenêtre des bobines (comme SelectMachineDialog::on_set_finish_mapping).
void SendPlatesDialog::on_finish_mapping(wxCommandEvent &evt)
{
    const wxArrayString arr = wxSplit(evt.GetString(), '|');
    if (arr.size() != 8 || m_popup_group < 0 || m_popup_group >= int(m_groups.size()))
        return;
    Group         &g = m_groups[m_popup_group];
    const wxColour colour(wxAtoi(arr[0]), wxAtoi(arr[1]), wxAtoi(arr[2]), wxAtoi(arr[3]));
    int                      ctype = 0;
    std::vector<wxColour>    cols;
    std::vector<std::string> tray_cols;
    for (MappingItem *mapping_item : m_mapping_popup->m_mapping_item_list)
        if (mapping_item->m_tray_data.id == evt.GetInt()) {
            ctype = mapping_item->m_tray_data.ctype;
            cols  = mapping_item->m_tray_data.material_cols;
            for (const wxColour &c : cols)
                tray_cols.push_back(wxString::Format("#%02X%02X%02X%02X", c.Red(), c.Green(), c.Blue(), c.Alpha()).ToStdString());
            break;
        }
    const int id = wxAtoi(arr[5]);
    for (FilamentInfo &r : g.result)
        if (r.id == id) {
            r.tray_id = evt.GetInt();
            r.color   = wxString::Format("#%02X%02X%02X%02X", colour.Red(), colour.Green(), colour.Blue(), colour.Alpha()).ToStdString();
            r.ctype   = ctype;
            r.colors  = tray_cols;
            r.ams_id  = arr[6].ToStdString();
            r.slot_id = arr[7].ToStdString();
        }
    for (auto &[fid, item] : g.items)
        if (fid == id)
            item->set_ams_info(colour, arr[4], ctype, cols);
}

// Bobine de l'AMS changee depuis cette fenetre, comme sur l'ecran Imprimante (StatusPanel::on_filament_edit et
// on_ext_spool_edit). La commande ne part que vers l'imprimante connectee (une seule en LAN) : une autre est d'abord
// relue, la fenetre de la bobine s'ouvre a la fin de sa lecture (set_ams).
void SendPlatesDialog::edit_tray(size_t gi, int ams_id, int slot_id)
{
    if (m_running || m_done || gi >= m_groups.size())
        return;
    MachineObject *obj      = group_machine(m_groups[gi]);
    DeviceManager *dev      = wxGetApp().getDeviceManager();
    MachineObject *selected = dev != nullptr ? dev->get_selected_machine() : nullptr;
    if (obj == nullptr)
        return;
    if (selected == nullptr || selected->get_dev_id() != obj->get_dev_id() || !selected->is_connected()) {
        m_pending_group = int(gi);
        m_pending_ams   = ams_id;
        m_pending_slot  = slot_id;
        read_ams(gi);
        return;
    }
    DevAmsTray *tray = nullptr;
    if (devPrinterUtil::IsVirtualSlot(ams_id)) {
        const size_t k = ams_id == VIRTUAL_TRAY_MAIN_ID ? 0 : 1;
        if (k < obj->vt_slot.size())
            tray = &obj->vt_slot[k];
    } else
        tray = obj->GetFilaSystem()->GetAmsTray(std::to_string(ams_id), std::to_string(slot_id));
    if (tray == nullptr)
        return;
    if (m_tray_dlg == nullptr)
        m_tray_dlg = new AMSMaterialsSetting(this, wxID_ANY);
    m_tray_dlg->obj             = obj;
    m_tray_dlg->ams_id          = ams_id;
    m_tray_dlg->slot_id         = slot_id;
    m_tray_dlg->ams_filament_id = tray->setting_id;
    m_tray_dlg->set_ctype(tray->ctype);
    const wxColour color = DevAmsTray::decode_color(tray->color);
    if (tray->setting_id.empty())
        m_tray_dlg->set_empty_color(color);
    else {
        std::vector<wxColour> cols;
        for (const std::string &c : tray->cols)
            cols.push_back(DevAmsTray::decode_color(c));
        m_tray_dlg->set_color(color);
        m_tray_dlg->set_colors(cols);
    }
    m_tray_dlg->m_is_third = !DevFilaSystem::IsBBL_Filament(tray->tag_uid);
    wxString filament, sn, temp_min, temp_max;
    if (!m_tray_dlg->m_is_third) {
        sn       = from_u8(tray->uuid);
        filament = from_u8(tray->sub_brands);
        temp_min = from_u8(tray->nozzle_temp_min);
        temp_max = from_u8(tray->nozzle_temp_max);
    }
    const wxString k = wxString::Format("%.3f", tray->k), n = wxString::Format("%.3f", tray->n);
    m_tray_group = int(gi);
    m_tray_ticks = 0;
    m_tray_timer.Start(1000);
    m_tray_dlg->CentreOnParent();
    m_tray_dlg->Popup(filament, sn, temp_min, temp_max, k, n); // modale
}

// Fenetre de la bobine ouverte : ses profils de calibrage arrivent de l'imprimante (comme StatusPanel::update_ams).
// Fermee : la bobine modifiee revient de l'imprimante en une ou deux secondes, puis la ligne AMS et l'appariement des
// filaments sont refaits.
void SendPlatesDialog::on_tray_timer()
{
    if (m_tray_dlg != nullptr && m_tray_dlg->IsShown()) {
        m_tray_dlg->TryRefreshPAProfiles();
        return;
    }
    if (++m_tray_ticks < 3)
        return;
    m_tray_timer.Stop();
    if (m_tray_group >= 0 && size_t(m_tray_group) < m_groups.size() && m_groups[m_tray_group].ams == Ams::Ready && !m_running && !m_done)
        update_filaments(size_t(m_tray_group));
    m_tray_group = -1;
}

void SendPlatesDialog::start()
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    if (dev == nullptr)
        return;

    // Un fichier par plateau, comme l'export de tous les plateaux, dans un dossier temporaire au nom du projet.
    std::vector<Upload> uploads;
    size_t              exported = 0;
    {
        wxBusyCursor busy;
        boost::system::error_code ec;
        if (m_temp_dir.empty()) {
            const fs::path temp = fs::temp_directory_path(ec);
            // Dossiers laissés par un « Tout imprimer » (lus par l'envoi après la fermeture de la fenêtre) : effacés au
            // bout d'un jour.
            for (fs::directory_iterator it(temp, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().filename().string().rfind("orio-envoi-", 0) == 0 && std::time(nullptr) - fs::last_write_time(it->path(), ec) > 24 * 3600)
                    fs::remove_all(it->path(), ec);
            ec.clear();
            m_temp_dir = temp / fs::unique_path("orio-envoi-%%%%-%%%%");
        }
        const fs::path folder = m_temp_dir / into_path(from_u8(m_project));
        fs::create_directories(folder, ec);
        std::set<int> written; // un plateau de plusieurs sections n'est exporté qu'une fois
        for (size_t gi = 0; gi < m_groups.size(); ++gi) {
            Group         &g   = m_groups[gi];
            const int      sel = g.combo->GetSelection();
            MachineObject *obj = sel != wxNOT_FOUND ? dev->get_my_machine(g.dev_ids[sel]) : nullptr;
            for (size_t ri = 0; ri < g.rows.size(); ++ri) {
                Row &row = g.rows[ri];
                if (obj == nullptr) {
                    row.status->SetLabel(_L("Not sent"));
                    continue;
                }
                if (!m_print && !row.on)
                    continue;
                const fs::path file = folder / into_path(from_u8(row.file));
                if (written.insert(row.plate).second && (ec || m_plater->export_3mf(file, SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel, row.plate) < 0)) {
                    MessageDialog(this, _L("Failed to export the sliced file.\nPlease check whether the file is occupied by another program or if there is enough disk space."),
                                  GetTitle(), wxOK | wxICON_WARNING).ShowModal();
                    return;
                }
                ++exported;
                // Impression : le plateau lance attend son tour, les autres leur depot (bug de lecture : le % du depot
                // d'un autre plateau passait pour celui du plateau lance).
                row.status->SetLabel(m_print && row.on ? _L("To start") : _L("Waiting"));
                row.status->SetForegroundColour(wxGetApp().dark_mode() ? wxColour(176, 176, 176) : wxColour(74, 81, 84));
                // Le plateau lancé part par sa fenêtre d'impression, qui le téléverse elle-même (bug [166]) : pas de dépôt.
                if (m_print && row.on)
                    continue;
                // La racine du FTP est la carte, sur tous les modèles (get_ftp_folder() n'est pas un chemin FTP : il
                // envoyait les fichiers des P1 / A1 dans un sous-dossier « sdcard »).
                uploads.push_back({gi, ri, obj->get_dev_ip(), obj->get_access_code(), m_project + "/" + row.file, obj->local_use_ssl_for_ftp, file});
            }
        }
    }
    if (exported == 0)
        return;

    // Plus de lecture d'AMS pendant l'envoi : la fenêtre d'impression apparie elle-même les imprimantes non lues.
    m_ams_timer.Stop();
    m_ams_queue.clear();
    m_ams_group = -1;
    for (size_t gi = 0; gi < m_groups.size(); ++gi)
        if (m_groups[gi].ams == Ams::Idle || m_groups[gi].ams == Ams::Reading)
            set_ams(gi, Ams::Unavailable);

    m_cancel  = false;
    m_running = true;
    m_btn_cancel->SetLabel(_L("Stop"));
    update_summary();
    relayout();
    if (uploads.empty()) {
        on_uploads_done();
        return;
    }

    // ponytail: un fichier après l'autre, toutes imprimantes confondues ; en parallèle par imprimante si c'est trop lent.
    m_thread = std::thread([this, uploads]() {
        for (const Upload &u : uploads) {
            if (m_cancel)
                break;
            int               last   = -1;
            std::string       remote = u.remote_path;
            const std::string error  = ftp_upload(u.ip, u.access_code, u.ssl, remote, u.file, [this, &u, &last](int percent) {
                if (percent != last) {
                    last = percent;
                    CallAfter([this, g = u.group, r = u.row, percent] {
                        m_groups[g].rows[r].status->SetLabel(m_print ? wxString::Format(_L("Deposit %d %%"), percent) : wxString::Format("%d %%", percent));
                        relayout();
                    });
                }
                return !m_cancel;
            });
            CallAfter([this, g = u.group, r = u.row, error, remote] {
                Row &row   = m_groups[g].rows[r];
                row.remote = remote;
                row.sent   = error.empty();
                row.error = error;
                row.status->SetLabel(row.sent ? (m_print ? _L("Deposited") : _L("Sent")) : m_cancel ? _L("Stopped") : _L("Failed"));
                row.status->SetForegroundColour(row.sent ? wxColour(8, 119, 232) : wxColour(225, 71, 71));
                relayout();
            });
        }
        CallAfter([this] { on_uploads_done(); });
    });
}

void SendPlatesDialog::on_uploads_done()
{
    if (m_thread.joinable())
        m_thread.join();
    m_running = false;
    // Tout envoyer : la fenêtre reste ouverte pour envoyer aussi à une autre imprimante du modèle.
    m_done = m_print;
    m_btn_ok->Show(!m_print);
    m_btn_cancel->SetLabel(_L("Close"));
    update_summary();
    relayout();
    if (!m_print || m_cancel)
        return;

    // Le plateau choisi sur chaque machine, l'une après l'autre (le module réseau ne tient qu'une imprimante LAN à la fois).
    for (size_t gi = 0; gi < m_groups.size(); ++gi)
        if (m_groups[gi].combo->GetSelection() != wxNOT_FOUND)
            for (size_t ri = 0; ri < m_groups[gi].rows.size(); ++ri)
                if (m_groups[gi].rows[ri].on)
                    m_launches.push_back({gi, ri});
    launch_next();
}

// Comme « Imprimer le plateau » : chaque fenêtre d'impression se ferme sur « Envoyer », son envoi se poursuit dans la
// notification du bas, puis vient la fenêtre de la machine suivante.
void SendPlatesDialog::launch_next()
{
    while (!m_launches.empty()) {
        // Un envoi LAN tient encore la connexion locale, unique : la machine suivante attend sa commande d'impression.
        if (BackgroundPrintJobs::lan_connection_busy()) {
            m_launch_timer.StartOnce(500);
            return;
        }
        const auto [gi, ri] = m_launches.front();
        m_launches.pop_front();
        Row &row = m_groups[gi].rows[ri];
        row.sent = launch(m_groups[gi], row);
        row.status->SetLabel(row.sent ? _L("Sending...") : _L("Not sent"));
        row.status->SetForegroundColour(row.sent ? wxColour(8, 119, 232) : wxColour(225, 71, 71));
        relayout();
    }
    // Tout est déposé et lancé : la fenêtre se ferme ; sinon elle reste pour ses états.
    for (const Group &g : m_groups)
        if (g.combo->GetSelection() != wxNOT_FOUND)
            for (const Row &row : g.rows)
                if (!row.sent)
                    return;
    if (m_launched)
        EndModal(wxID_OK);
}

bool SendPlatesDialog::launch(Group &group, Row &row)
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    if (dev == nullptr)
        return false;
    const std::string &dev_id   = group.dev_ids[group.combo->GetSelection()];
    MachineObject     *selected = dev->get_selected_machine();
    if (selected == nullptr || selected->get_dev_id() != dev_id) {
        if (BackgroundPrintJobs::refuse_machine_switch(dev_id, this))
            return false;
        dev->set_selected_machine(dev_id);
    }

    // Fenêtre d'impression d'un fichier de la carte (onglet Imprimante) : les données du plateau sont lues dans son
    // fichier, elle fait la correspondance AMS. Elle reçoit le fichier de l'ordinateur (chemin absolu), que PrintJob
    // envoie comme une impression normale : les imprimantes refusent de lancer un fichier déposé par Orio sur leur
    // carte (0500-4002 sur X1, 0500-C010 sur P1). PrintJob le passe au module réseau en « from_normal », qui le
    // téléverse puis le lance (en « from_sdcard_view », il ne téléverse rien).
    const fs::path     local = m_temp_dir / into_path(from_u8(m_project)) / into_path(from_u8(row.file));
    DynamicPrintConfig config;
    Model              model;
    PlateDataPtrs      plate_data;
    Semver             version;
    boost::nowide::ifstream in(into_u8(from_path(local)), std::ios::binary);
    if (!in || !load_gcode_3mf_from_stream(in, &config, &model, &plate_data, &version) || plate_data.empty())
        return false;
    if (m_plater->update_print_required_data(config, model, plate_data, row.file, local.string()) <= 0)
        return false;
    // Bobines choisies ici, reprises par la fenêtre d'impression à sa première correspondance AMS.
    if (group.ams == Ams::Ready && !group.result.empty())
        SelectMachineDialog::s_preset_ams_mapping[dev_id] = group.result;
    // Traité tout de suite (pas wxPostEvent) : la fenêtre d'impression est modale, la machine suivante attend sa fin.
    const int   started = BackgroundPrintJobs::started_count();
    SimpleEvent evt(EVT_PRINT_FROM_SDCARD_VIEW);
    m_plater->GetEventHandler()->ProcessEvent(evt);
    SelectMachineDialog::s_preset_ams_mapping.erase(dev_id);
    // « Envoyer » referme la fenêtre, l'envoi part en arrière-plan ; sinon elle a été fermée sans rien envoyer. Un envoi
    // déjà en cours vers cette machine (un « Imprimer le plateau » encore en téléversement) ne compte pas.
    const bool sending = BackgroundPrintJobs::started_count() != started && BackgroundPrintJobs::is_sending_to(dev_id);
    m_launched |= sending;
    return sending;
}

} // namespace GUI
} // namespace Slic3r
