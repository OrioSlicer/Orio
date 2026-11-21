#ifndef slic3r_GUI_SendPlatesDialog_hpp_
#define slic3r_GUI_SendPlatesDialog_hpp_

#include "GUI_Utils.hpp"
#include "libslic3r/ProjectTask.hpp"

#include <wx/timer.h>

#include <boost/filesystem/path.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class wxScrolledWindow;
class Button;
class ComboBox;
class Label;
class ScalableButton;
class StaticBox;

namespace Slic3r {

class MachineObject;

namespace GUI {

class Plater;
class MaterialItem;
class AmsMapingPopup;
class AMSMaterialsSetting;

// Orio : « Tout envoyer » / « Tout imprimer » (et un plateau tranché pour la machine de son feuillet).
// Une section par modèle de machine (en impression, une par imprimante du modèle, autant que de plateaux), comme la
// fenêtre « Envoi impression » : la carte de l'imprimante du réseau local
// qui reçoit ses plateaux, une tuile par plateau et, en impression, les filaments du plateau lancé appariés aux bobines
// de son AMS. Chaque plateau devient un « Projet_plate_N.gcode.3mf » (comme l'export de tous les plateaux), déposé par
// FTPS dans un dossier « Projet » de la carte SD. En mode impression, le plateau choisi sur chaque machine n'y est pas
// déposé : il part par la fenêtre d'impression d'un fichier de la carte, préremplie avec les bobines choisies ici, qui
// l'envoie comme une impression normale (dépôt à la racine par le module réseau, puis lancement), en arrière-plan.
class SendPlatesDialog : public DPIDialog
{
public:
    // plate_index : PLATE_ALL_IDX ou un seul plateau. Prévient et ne s'ouvre pas si rien n'est tranché.
    static void show(Plater *plater, bool print, int plate_index);

    // FTPS implicite de l'imprimante (port 990, bblp / code d'accès ; FTP simple si !ssl), dossiers manquants
    // créés ; dossier refusé : à la racine, et remote_path le dit. on_percent renvoie false pour arrêter. Renvoie "" si
    // réussi, sinon l'erreur.
    static std::string ftp_upload(const std::string &ip, const std::string &access_code, bool ssl, std::string &remote_path,
                                  const boost::filesystem::path &file, const std::function<bool(int)> &on_percent);

    ~SendPlatesDialog() override;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    SendPlatesDialog(Plater *plater, bool print, int plate_index);

    struct Row
    {
        int         plate{0};
        std::string file;              // Projet_plate_N.gcode.3mf
        std::string remote;            // son chemin sur l'imprimante
        StaticBox  *tile{nullptr};
        wxSizer    *info{nullptr};     // durée et poids, remplacés par l'état de l'envoi
        Label      *status{nullptr};
        std::string error;             // erreur de l'envoi, pour l'infobulle
        bool        on{false};         // envoi : plateau envoyé ; impression : plateau lancé
        bool        sent{false};
    };
    // Lecture de l'AMS de l'imprimante choisie, une à la fois (Orio ne tient qu'une connexion LAN).
    enum class Ams { Idle, Reading, Ready, Unavailable };
    struct Group
    {
        std::string                                  model_id;
        wxString                                     model_name;
        std::vector<Row>                             rows;
        ComboBox                                    *combo{nullptr};
        ScalableButton                              *refresh{nullptr};
        StaticBox                                   *none{nullptr};    // impression : « Aucun, seulement déposer »
        Label                                       *missing{nullptr};
        wxSizer                                     *filaments{nullptr};
        std::vector<std::pair<int, MaterialItem *>>  items;            // id du filament, sa case
        std::vector<FilamentInfo>                    slots;            // filaments du plateau lancé
        std::vector<FilamentInfo>                    result;           // bobines choisies
        std::vector<int>                             fmap;             // buse de chaque filament (1 gauche, 2 droite)
        Ams                                          ams{Ams::Idle};
        std::vector<std::string>                     dev_ids;
        size_t                                       slot{0};          // impression : rang parmi les sections du modèle
    };
    struct Upload
    {
        size_t                  group, row;
        std::string             ip, access_code, remote_path;
        bool                    ssl;
        boost::filesystem::path file;
    };

    void fill_printers();
    void relayout();
    void update_summary();
    void update_tiles(size_t gi);
    void pick_tile(size_t gi, size_t ri);
    void start();
    void on_uploads_done();
    void launch_next();
    bool launch(Group &group, Row &row);

    void           read_ams(size_t gi);
    void           read_next_ams();
    void           on_ams_timer();
    void           set_ams(size_t gi, Ams state);
    MachineObject *group_machine(const Group &g) const;
    void           map_filaments(Group &g, MachineObject *obj);
    void           update_filaments(size_t gi);
    void           on_finish_mapping(wxCommandEvent &evt);
    void           edit_tray(size_t gi, int ams_id, int slot_id);
    void           on_tray_timer();

    Plater                  *m_plater;
    bool                     m_print;
    std::string              m_project;
    std::vector<Group>       m_groups;
    wxScrolledWindow        *m_scroll{nullptr};
    Label                   *m_summary{nullptr};
    Button                  *m_btn_ok{nullptr};
    Button                  *m_btn_cancel{nullptr};
    AmsMapingPopup          *m_mapping_popup{nullptr};
    int                      m_popup_group{-1};
    wxTimer                  m_ams_timer;
    std::deque<size_t>       m_ams_queue;
    int                      m_ams_group{-1};
    int                      m_ams_ticks{0};
    std::string              m_orig_dev_id;   // imprimante sélectionnée à l'ouverture, rendue si rien n'est lancé
    bool                     m_switched{false};
    boost::filesystem::path  m_temp_dir;
    std::thread              m_thread;
    wxTimer                  m_launch_timer;
    std::deque<std::pair<size_t, size_t>> m_launches; // (groupe, plateau) encore à lancer
    std::atomic<bool>        m_cancel{false};
    bool                     m_running{false};
    bool                     m_done{false};
    bool                     m_launched{false};
    // Bobine de l'AMS changee depuis cette fenetre (edit_tray).
    AMSMaterialsSetting     *m_tray_dlg{nullptr};
    wxTimer                  m_tray_timer;
    int                      m_tray_group{-1};
    int                      m_tray_ticks{0};
    int                      m_pending_group{-1}; // imprimante relue avant d'ouvrir la fenetre de sa bobine
    int                      m_pending_ams{0};
    int                      m_pending_slot{0};
};

} // namespace GUI
} // namespace Slic3r

#endif
