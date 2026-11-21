#ifndef slic3r_GUI_GCodeConsoleDialog_hpp_
#define slic3r_GUI_GCodeConsoleDialog_hpp_

#include "GUI_Utils.hpp"

#include <wx/timer.h>

#include <chrono>
#include <map>
#include <string>
#include <vector>

class wxTextCtrl;
class Button;
class ComboBox;
class Label;

namespace Slic3r {

class MachineObject;

namespace GUI {

// Orio : console G-code. Envoie des lignes G-code à l'imprimante choisie par la
// commande MQTT « gcode_line » (celle des flèches de déplacement de l'onglet Imprimante),
// en cloud comme en LAN, et affiche l'accusé de réception renvoyé par l'imprimante.
// Fenêtre non modale, une seule instance, gardée cachée à la fermeture (journal conservé).
class GCodeConsoleDialog : public DPIDialog
{
public:
    // Ouvre la console, ou la ramène au premier plan.
    static void show_console(wxWindow *parent);
    // Orio (onglet « Dessin G-code ») : ouvre la console avec ce texte dans la zone de saisie,
    // sans l'envoyer ; l'utilisateur le vérifie puis l'envoie (mêmes vérifications que la saisie).
    static void show_console_with_text(wxWindow *parent, const wxString &text);
    // Appelée par GUI_App pour chaque message MQTT reçu d'une imprimante (cloud ou LAN),
    // sur le fil principal : repère les accusés de réception des commandes de la console.
    static void on_printer_message(const std::string &dev_id, const std::string &msg);

    ~GCodeConsoleDialog() override;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    explicit GCodeConsoleDialog(wxWindow *parent);

    // Commande envoyée, en attente de son accusé de réception.
    struct Pending
    {
        std::string                           dev_id;
        wxString                              printer;
        size_t                                lines{0};
        std::chrono::steady_clock::time_point sent;
    };

    void on_timer(wxTimerEvent &event);
    void on_input_key(wxKeyEvent &event);
    void on_printer_selected(int selection);
    bool apply_printer_switch();

    void refresh_printers();
    void refresh_status();
    void expire_pending();

    void send_input();
    void send_park();
    // Envoie des lignes déjà préparées à l'imprimante sélectionnée (vérifications, confirmation
    // pendant une impression, journal, accusés). Faux si rien n'est parti.
    bool send_lines(const std::vector<std::string> &lines);
    bool send_block(MachineObject *obj, const std::vector<std::string> &lines);
    void on_ack(const std::string &dev_id, int sequence_id, const std::string &result, const std::string &reason, int err_code);

    void history_step(int direction);
    void history_add(const wxString &entry);
    void load_history();
    void save_history() const;

    enum class LogKind { Info, Sent, Ok, Error };
    void append_log(const wxString &text, LogKind kind);

    static GCodeConsoleDialog *s_instance;

    ComboBox   *m_printer_combo{nullptr};
    Label      *m_status{nullptr};
    Button     *m_park_btn{nullptr};
    wxTextCtrl *m_log{nullptr};
    wxTextCtrl *m_input{nullptr};
    Button     *m_send_btn{nullptr};
    Button     *m_copy_btn{nullptr};
    Button     *m_clear_btn{nullptr};
    Label      *m_hint{nullptr};
    wxTimer     m_timer;

    std::vector<std::string> m_printer_ids;       // dev_id de chaque entrée de m_printer_combo
    std::string              m_printers_signature; // pour ne reconstruire la liste que si elle change
    std::string              m_last_selected;      // machine sélectionnée au dernier rafraîchissement
    std::string              m_switch_pending;     // machine choisie, pas encore sélectionnée (connexion LAN occupée)

    std::map<int, Pending> m_pending; // sequence_id -> commande en attente d'accusé

    std::vector<wxString> m_history;  // du plus ancien au plus récent
    int                   m_history_pos{-1};
    wxString              m_history_draft;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_GCodeConsoleDialog_hpp_
