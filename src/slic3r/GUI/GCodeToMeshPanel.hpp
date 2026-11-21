#ifndef slic3r_GCodeToMeshPanel_hpp_
#define slic3r_GCodeToMeshPanel_hpp_

// Orio: "Converter" tab (formerly "G-code to Mesh"), on the left of "G-code Drawing". The page has a row per
// conversion, one under the other, each its header, its rail of three steps and its three illustrated cards.
// Row 1, a G-code into a mesh; the imported G-codes open here (menu, drag and drop, command line):
// 1 open a G-code (drop zone, recent G-codes; then the picture of the print and what the G-code says),
// 2 read and adjust (the size compensations of the slicer to undo, the smoothing between the layers),
// 3 transform (the objects it prints, rebuilt from their outer walls by GCodeToMesh, go to the Prepare tab; or "View"
// shows the G-code alone in the Preview). The meshes are built in the background as soon as the G-code is open, with the
// settings of card 2: card 3 shows them, to be turned with the mouse, and "Transform" takes them as they are.
// Row 2, a mesh into a STEP solid by MeshToStep, still in beta (its cards in a yellow frame with the "Beta" badge): a
// mesh file (STL, OBJ, 3MF) opened or dropped here, an object of the Prepare tab (right click), or the mesh of row 1:
// 1 the mesh, 2 the deviation allowed to the surfaces recognized, 3 the surfaces found, one color per kind, and "Save
// as STEP".

#include "libslic3r/GCodeToMesh.hpp"
#include "libslic3r/MeshToStep.hpp"

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/stopwatch.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Button;
class wxBoxSizer;
class wxFont;
class wxGraphicsContext;
class wxRect2DDouble;
class wxScrolledWindow;
class wxSizer;
class wxStaticText;

namespace Slic3r {

class ModelObject;

namespace GUI {

class GCodeToMeshPanel : public wxPanel
{
public:
    explicit GCodeToMeshPanel(wxWindow *parent);
    ~GCodeToMeshPanel() override;

    // Reads the G-code and shows what it says; false if it cannot be read.
    bool open(const wxString &path);
    // A mesh to convert into STEP: a file (STL, OBJ, 3MF), false if it cannot be read; an object of the Prepare tab,
    // one of its instances or all of them (-1).
    bool open_mesh(const wxString &path);
    void open_object(const ModelObject &object, int instance);
    // The light or dark theme was switched.
    void sys_color_changed();

    // What an illustration shows.
    enum class Scene { Drop, Scan, Mesh, Thumbnail, Top, Profile, Result, View, Transform, Smooth, Compensation, DropMesh, Surfaces, StepFile };

private:
    struct Palette;
    class Canvas;
    class Card;
    class BetaFrame;
    class Rail;
    class Chips;
    class Option;

    // The width of a layer seen from the side, in mm.
    struct Bar
    {
        double z0;
        double z1;
        double x0;
        double x1;
    };
    // A text in a color of the theme (0 text, 1 secondary text, 2 accent), on a card or on the page; wrapped to the
    // width of its card or of the page. A caption keeps at least `lines` lines.
    struct Text
    {
        wxStaticText *text;
        int           role;
        bool          on_card;
        bool          wrapped;
        wxString      label;
        int           lines{ 0 };
    };
    // A job of a thread of the page, given its number.
    using Job = std::function<void(unsigned generation)>;
    // A thread working for one row of the page, on the latest job given: post() gives it a job (the one waiting is
    // dropped, the one running stopped: its stale() turns true), stop() stops it, quit() stops it and waits for the end.
    class Worker
    {
    public:
        void post(Job job);
        void stop();
        void quit();
        bool stale(unsigned generation) const { return m_quit || generation != m_generation; }

    private:
        void work();

        std::thread             m_thread;
        std::mutex              m_mutex;
        std::condition_variable m_wake;
        Job                     m_job;
        unsigned                m_job_generation{ 0 };
        std::atomic<bool>       m_quit{ false };
        std::atomic<unsigned>   m_generation{ 0 };
    };

    // A row of the page, one conversion: its name, the file open (icon, name, what it is), its rail of three steps, its
    // three cards (the content before a file is open, and once open), its notes; the card under the mouse (1 to 3).
    struct Row
    {
        wxStaticText *label{ nullptr };
        Canvas       *icon{ nullptr };
        wxStaticText *title{ nullptr };
        wxStaticText *subtitle{ nullptr };
        Rail         *rail{ nullptr };
        Card         *cards[3]{};
        wxWindow     *arrive[3]{};
        wxWindow     *opened[3]{};
        wxStaticText *notes{ nullptr };
        int           hovered{ 0 };
    };
    // The rows: a G-code into a mesh, a mesh into a STEP solid.
    enum { ROW_GCODE = 0, ROW_STEP = 1 };

    static Palette palette();

    void          build_ui();
    void          build_open_card();
    void          build_read_card();
    void          build_transform_card();
    void          build_mesh_arrive_cards();
    void          build_convert_cards();
    wxSizer      *make_title(wxWindow *parent, int step, const wxString &title);
    wxStaticText *add_text(wxWindow *parent, const wxString &label, const wxFont &font, int role, bool on_card, bool wrapped, long style = 0);
    void          set_text(wxStaticText *text, const wxString &label);
    void          read_pictures();
    void          update();
    void          update_recent();
    void          update_mesh();
    void          apply_colors();
    // Width of the column and of its texts, height of the illustrations, from the size of the page.
    void          rewrap(bool force);
    bool          wrap(const Text &entry);
    void          relayout();
    void          hover_card(int row, int index, bool hovered);
    // The page scrolled to show a row, once laid out.
    void          scroll_to(int row);
    void          show_setting(Scene scene);
    void          show_result(Scene scene);
    // The meshes of a well turned with the mouse, those of all the wells together; ready() says when there is one.
    void          turnable(Canvas *canvas, std::function<bool()> ready);
    void          forget_renders();
    void          paint_scene(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area, Scene scene) const;
    void          paint_top(wxGraphicsContext &gc, const wxRect2DDouble &area) const;
    void          paint_profile(wxGraphicsContext &gc, const wxRect2DDouble &area, const wxColour &colour) const;
    void          paint_meshes(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const;
    void          paint_part(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const;
    void          paint_deviation(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const;
    void          paint_regions(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const;
    void          paint_legend(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const;
    wxString      header_details() const;
    void          choose_file();
    void          choose_mesh();
    void          view();
    void          transform();
    wxString      object_name(size_t index) const;
    // The meshes of the G-code with the settings of card 2: those of card 3 when they have them, otherwise built now;
    // empty after an error message.
    std::vector<indexed_triangle_set> current_meshes();
    // The mesh to convert into STEP (row 2): from the G-code, or given.
    void          convert_gcode();
    void          show_mesh(indexed_triangle_set its, const wxString &name, const wxString &folder);
    void          repair_mesh();
    // The surfaces of the mesh recognized with the deviation of card 2, on the thread of the page, received by
    // on_recognized(); saved into a STEP file, received by on_saved().
    void          recognize();
    void          on_recognized(std::shared_ptr<const MeshToStep> step, size_t open, size_t shared, const std::string &error);
    void          save_step();
    // level: that of the solid saved (lower than 4 when OpenCASCADE did not find the one above valid); straight: level 4
    // with straight edges, its fitted edges did not make a valid solid; facets: surfaces of level 4 made of facets, their
    // faces not valid.
    void          on_saved(const MeshToStep::Result &result, int level, bool straight, size_t facets, const std::string &error,
                           const wxString &path);
    // The meshes for the settings of card 2, built on the thread of the page, received by on_meshes().
    void          request_meshes();
    void          on_meshes(std::vector<indexed_triangle_set> meshes, bool failed, bool undo, bool smooth);
    // What a job of a worker found, run on the interface thread unless the job is stale by then.
    void          finish(const Worker &worker, unsigned generation, std::function<void()> done);

    // Shared with the thread which builds the meshes.
    std::shared_ptr<const GCodeToMesh> m_gcode{ std::make_shared<GCodeToMesh>() };
    wxString                           m_path;
    bool                               m_undo_available{ false };

    // Pictures of the print: the thumbnail written by the slicer, the outer walls of some layers seen from above and the
    // width of each layer seen from the side, in mm.
    wxBitmap                        m_thumbnail;
    std::vector<std::vector<Vec2d>> m_top;
    Vec2d                           m_top_min{ Vec2d::Zero() };
    Vec2d                           m_top_max{ Vec2d::Zero() };
    std::vector<Bar>                m_profile;

    // Shown by the cards: the picture of the print, the setting under the mouse, the result or the button under it.
    Scene m_view{ Scene::Thumbnail };
    Scene m_focus{ Scene::Smooth };
    Scene m_result{ Scene::Result };
    // The animations of the empty cards.
    wxTimer     m_timer;
    wxStopWatch m_clock;

    wxScrolledWindow *m_page{ nullptr };
    wxBoxSizer       *m_column{ nullptr };
    Row               m_rows[2];
    // Row 1, before a G-code is open (m_rows[ROW_GCODE].arrive), and once open (.opened).
    Canvas           *m_drop{ nullptr };
    Canvas           *m_scan{ nullptr };
    Canvas           *m_mesh{ nullptr };
    ::Button         *m_open_button{ nullptr };
    wxStaticText     *m_recent_title{ nullptr };
    wxWindow         *m_recent{ nullptr };
    Canvas           *m_picture{ nullptr };
    Chips            *m_views{ nullptr };
    wxStaticText     *m_info[7]{};
    ::Button         *m_change_button{ nullptr };
    Canvas           *m_setting{ nullptr };
    wxStaticText     *m_setting_caption{ nullptr };
    Option           *m_undo{ nullptr };
    Option           *m_smooth{ nullptr };
    Canvas           *m_result_canvas{ nullptr };
    wxStaticText     *m_result_caption{ nullptr };
    wxStaticText     *m_size{ nullptr };
    ::Button         *m_transform_button{ nullptr };
    ::Button         *m_view_button{ nullptr };
    ::Button         *m_convert_button{ nullptr };
    // Row 2, before a mesh is open, and once open.
    Canvas           *m_mesh_drop{ nullptr };
    Canvas           *m_surfaces{ nullptr };
    Canvas           *m_step_file{ nullptr };
    ::Button         *m_open_mesh_button{ nullptr };
    Canvas           *m_part{ nullptr };
    wxStaticText     *m_part_info[4]{};
    ::Button         *m_repair_button{ nullptr };
    ::Button         *m_other_mesh_button{ nullptr };
    Canvas           *m_deviation{ nullptr };
    Canvas           *m_regions{ nullptr };
    Canvas           *m_legend{ nullptr };
    wxStaticText     *m_step_caption{ nullptr };
    ::Button         *m_save_button{ nullptr };
    std::vector<Text> m_texts;
    int               m_wrap_width{ 0 };
    int               m_card_width{ 0 };
    // Height of the page, and of all the illustrations of the cards.
    int               m_page_height{ 0 };
    int               m_well_height{ 0 };

    // The threads of the rows: the meshes of the G-code; the surfaces of the mesh and its save.
    Worker m_gcode_worker;
    Worker m_step_worker;
    // Settings of the latest request; the meshes received and their settings (kept on screen while others are built).
    bool                              m_wanted_undo{ false };
    bool                              m_wanted_smooth{ false };
    bool                              m_building{ false };
    bool                              m_build_failed{ false };
    std::vector<indexed_triangle_set> m_meshes;
    bool                              m_meshes_undo{ false };
    bool                              m_meshes_smooth{ false };
    // View of the meshes, turned with the mouse: around Z, then tilted toward the viewer, radians; the pictures drawn
    // last, drawn again when the view, the meshes or the theme change.
    double           m_yaw{ -0.52 };
    double           m_pitch{ 0.44 };
    bool             m_dragging{ false };
    wxPoint          m_drag_start;
    mutable wxBitmap m_render;
    mutable wxBitmap m_part_render;
    mutable wxBitmap m_regions_render;

    // The mesh to convert into STEP (row 2): its name, the folder it comes from, what it measures.
    std::shared_ptr<const indexed_triangle_set> m_mesh_its;
    wxString                                    m_mesh_name;
    wxString                                    m_mesh_folder;
    Vec3d                                       m_mesh_size{ Vec3d::Zero() };
    double                                      m_mesh_volume{ 0. };
    // Setting of card 2: the index of the deviation allowed. The recognition is always at level 4 (no facets), falling
    // back to straight edges, to the facets of level 3, to flat faces then to triangles when the solid is not valid.
    int m_tolerance{ 0 };
    // The surfaces recognized and the kind of each face of their mesh, or why not: closed (-1 not known yet, 0 no,
    // 1 yes), its faulty edges, another error. The save under way, its progress, its report.
    std::shared_ptr<const MeshToStep> m_step;
    std::vector<unsigned char>        m_face_kinds;
    bool                              m_recognizing{ false };
    int                               m_closed{ -1 };
    size_t                            m_open_edges{ 0 };
    size_t                            m_shared_edges{ 0 };
    wxString                          m_step_error;
    bool                              m_saving{ false };
    double                            m_save_progress{ 0. };
    wxString                          m_save_report;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GCodeToMeshPanel_hpp_
