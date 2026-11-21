#ifndef slic3r_GUI_PartSkipPanel_hpp_
#define slic3r_GUI_PartSkipPanel_hpp_

// Orio: "Objects being printed" box of the Device tab, under the filaments. It shows a top view of the
// objects of the running print and excludes the ones clicked by the user ("skip_objects" command of the printer,
// the same one as the PartSkipDialog of Bambu Studio), for instance when a part came loose from the plate.

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/longlong.h>
#include <wx/window.h>

#include <boost/shared_ptr.hpp>

#include "Widgets/StaticBox.hpp"

class Button;
class Label;
class PrinterFileSystem;

namespace Slic3r {

class MachineObject;

namespace GUI {

// Keeps the plate files of a print sent from this computer (picking image and slice_info.config of the sent 3MF,
// number of layers of its G-code) in the data folder: the box shows the plate from them, without asking the
// printer, which does not share its files in LAN mode. Called by PrintJob before sending; may run on a worker thread.
void remember_sent_plate(const std::string &dev_id, const std::string &subtask_name, int plate_idx, const std::string &path_3mf);

// Top view of the plate drawn from the picking image stored in the printed 3MF (Metadata/pick_<plate>.png):
// every object is painted with its own colour, which encodes its identify_id (R | G << 8 | B << 16), the id
// expected by the "skip_objects" command.
class PartSkipMap : public wxWindow
{
public:
    enum class ObjectState { Printing, Selected, Pending, Skipped };

    struct Object
    {
        wxString    name;
        ObjectState state{ObjectState::Printing};

        bool operator==(const Object &other) const { return name == other.name && state == other.state; }
        bool operator!=(const Object &other) const { return !(*this == other); }
    };

    explicit PartSkipMap(wxWindow *parent);

    // Plate silhouette drawn under the objects (depends on the printer model).
    void set_background(const wxString &path);
    // Picking image of the running print. Returns false if it cannot be read.
    bool load_picking_image(const wxString &path);
    void clear();

    void set_objects(const std::map<uint32_t, Object> &objects);
    // Objects can be selected by a click only when the print supports object skipping.
    void set_clickable(bool clickable);
    // Called with the identify_id of a clicked object that is still printed (selected or not).
    void set_on_click(std::function<void(uint32_t)> on_click) { m_on_click = std::move(on_click); }

    void msw_rescale();

private:
    void on_paint(wxPaintEvent &event);
    void on_size(wxSizeEvent &event);
    void on_motion(wxMouseEvent &event);
    void on_leave(wxMouseEvent &event);
    void on_left_down(wxMouseEvent &event);

    wxRect   view_rect() const;
    uint32_t object_at(const wxPoint &pos) const;
    bool     is_clickable(uint32_t id) const;
    void     set_hover(uint32_t id);
    void     update_tooltip();
    void     invalidate();
    void     compose();

    // Size of the picking image; the plate silhouette is stretched over it, as the printer does.
    int                          m_width{512};
    int                          m_height{512};
    std::vector<uint32_t>        m_ids;   // one identify_id per pixel of the picking image, 0 = no object
    std::vector<uint8_t>         m_edges; // 1 on the outline of an object
    wxImage                      m_background_src;
    wxImage                      m_background; // m_background_src stretched to m_width x m_height
    std::map<uint32_t, Object>   m_objects;
    uint32_t                     m_hover{0};
    bool                         m_clickable{false};
    std::function<void(uint32_t)> m_on_click;

    wxImage  m_composed;       // plate + objects, at the size of the picking image
    bool     m_composed_dirty{true};
    wxBitmap m_bitmap;         // m_composed scaled to the window
    bool     m_bitmap_dirty{true};
};

class PartSkipPanel : public StaticBox
{
public:
    explicit PartSkipPanel(wxWindow *parent);
    ~PartSkipPanel() override;

    // Called by StatusPanel::update() about once a second while the Device tab is shown.
    // Returns whether the box has to be shown: only while the printer runs a print whose objects can be listed.
    bool update(MachineObject *obj);
    // Printer changed or disconnected.
    void reset();
    void msw_rescale();

private:
    enum class LoadState { Idle, Loading, Ready, Failed };
    enum class UrlAttempt { Local, Remote };

    void create_ui();
    void start_loading(MachineObject *obj);
    bool find_sent_plate(MachineObject *obj);
    void finish_loading(bool downloaded);
    bool read_plate_files();
    void clear_job();
    void release_file_system(bool deferred);
    void fetch_url();
    void on_fs_status(wxCommandEvent &event);
    void on_fs_download(wxCommandEvent &event);
    void sync_with_printer(MachineObject *obj);
    void on_object_clicked(uint32_t id);
    void on_exclude(wxCommandEvent &event);
    void on_retry(wxCommandEvent &event);
    void refresh_view();
    void set_message(const wxString &message);
    int  info_width() const;

    // UI
    Label       *m_title{nullptr};
    Label       *m_counter{nullptr};
    PartSkipMap *m_map{nullptr};
    Label       *m_message{nullptr};
    wxString     m_message_text;
    wxSizer     *m_legend_sizer{nullptr};
    std::vector<wxWindow *> m_legend_swatches;
    std::vector<wxWindow *> m_legend_windows; // swatches and their labels
    Button      *m_exclude_btn{nullptr};
    Button      *m_retry_btn{nullptr};

    // Print shown in the box
    std::string                  m_job_key;
    std::string                  m_dev_id;
    int                          m_plate_idx{1};
    bool                         m_can_download{false}; // the printer gives access to its files (not in LAN mode on X1/P1)
    int                          m_sent_layers{0};      // layers of the print sent from this computer shown, 0 otherwise
    LoadState                    m_load_state{LoadState::Idle};
    wxLongLong                   m_load_start{0};
    wxLongLong                   m_failed_at{0};
    bool                         m_auto_retried{false};
    bool                         m_can_skip{false};
    wxString                     m_cannot_skip_reason;
    std::map<uint32_t, wxString> m_names;    // identify_id -> object name
    std::set<uint32_t>           m_selected; // chosen by the user, not sent yet
    std::map<uint32_t, wxLongLong> m_pending; // sent to the printer, not reported as skipped yet
    wxLongLong                   m_pending_since{0}; // last exclusion sent
    size_t                       m_push_requests{0}; // full states asked for since then
    std::set<uint32_t>           m_skipped;  // reported as skipped by the printer
    wxString                     m_notice;
    wxLongLong                   m_notice_time{0};
    std::string                  m_background_name;

    // Download of the plate files from the printer
    boost::shared_ptr<PrinterFileSystem> m_file_sys;
    UrlAttempt                   m_url_attempt{UrlAttempt::Local};
    bool                         m_can_try_remote{false};
    std::string                  m_tmp_dir;
    std::vector<std::string>     m_local_paths;
    std::vector<std::string>     m_target_paths;
    int                          m_load_counter{0};
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PartSkipPanel_hpp_
