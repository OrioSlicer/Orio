#ifndef slic3r_DetachedToolWindow_hpp_
#define slic3r_DetachedToolWindow_hpp_

#include <array>
#include <cfloat>
#include <chrono>
#include <deque>
#include <memory>
#include <vector>

#include <wx/gdicmn.h>
#include <wx/string.h>

#include <imgui/imgui.h>

class wxMouseEvent;
class wxKeyEvent;
struct ImGuiContext;

namespace Slic3r {
namespace GUI {

class DetachedToolFrame;

// Orio: the window of a tool taken out of the 3D view into a window of its own, which can go to another screen.
// A wxFrame holds a GL canvas sharing the GL context of the 3D views, and the window gets a second ImGui context sharing
// their font atlas. The code of the tool does not change: ImGuiWrapper::begin_tool_window() switches to that context while
// the tool draws its window, during the render of the 3D view as before (begin()); end_tool_window() keeps the draw lists
// (end()), which the canvas of the frame paints afterwards. The mouse and the keyboard of the frame go to that context, and
// each of their events renders the 3D view again, where the tool reads them.
class DetachedToolWindow
{
public:
    DetachedToolWindow(const wxString &title, const wxPoint &screen_pos);
    ~DetachedToolWindow();

    DetachedToolWindow(const DetachedToolWindow &)            = delete;
    DetachedToolWindow &operator=(const DetachedToolWindow &) = delete;

    // Switches to the ImGui context of the window and opens the window of the tool in it, with the style in effect in the
    // 3D view (the tool pushed its own before). end() renders it, keeps its draw lists and switches back.
    void begin(int flags);
    void end();

    // Not drawn by the last render of the 3D view (the tool was closed): hidden until the next begin().
    void hide();
    bool hidden() const { return m_hidden; }
    int  last_main_frame() const { return m_last_main_frame; }
    // The user closed the frame: the window goes back into the 3D view.
    bool closed_by_user() const { return m_closed_by_user; }
    // Top-left corner of the frame on the screen, wxDefaultPosition before the frame exists.
    wxPoint screen_position() const;
    // Native tooltip of the frame (shown in front of it, as the ones of the settings), empty = none.
    void set_tooltip(const wxString &tooltip);

    // Screen position of a point of the 3D view (of the current canvas of the plater).
    static wxPoint view_to_screen(const ImVec2 &pos);
    // Whether a frame placed there would be on one of the screens (one may have been unplugged since).
    static bool on_a_screen(const wxPoint &screen_pos);

    // Called by the frame.
    void create_frame();
    void frame_destroyed() { m_frame = nullptr; }
    void on_paint();
    void on_mouse(wxMouseEvent &evt);
    void on_key(wxKeyEvent &evt);
    void on_char_hook(wxKeyEvent &evt);
    void on_capture_lost();
    void on_close();

private:
    struct MouseStep
    {
        ImVec2              pos;
        std::array<bool, 3> down;
    };
    struct KeyStep
    {
        int  key;
        bool down;
        bool shift;
        bool ctrl;
        bool alt;
    };

    void clear_draw_lists();

    DetachedToolFrame       *m_frame{nullptr};
    wxString                 m_title;
    wxPoint                  m_initial_pos;
    ImGuiContext            *m_context{nullptr};
    ImGuiContext            *m_main_context{nullptr};
    std::vector<ImDrawList*> m_draw_lists; // owned, ImDrawList::CloneOutput()
    ImVec4                   m_background{1.f, 1.f, 1.f, 1.f};
    wxSize                   m_extent{1, 1}; // the window of the tool with its tooltips and popups, the client size of the frame
    ImVec2                   m_mouse_pos{-FLT_MAX, -FLT_MAX};
    std::array<bool, 3>      m_mouse_down{false, false, false};   // state after the last step played
    std::array<bool, 3>      m_queued_down{false, false, false};  // state after the last step queued
    float                    m_wheel{0.f};
    std::deque<MouseStep>    m_mouse_steps;
    std::deque<KeyStep>      m_key_steps;
    bool                     m_wants_text_input{false};
    wxString                 m_tooltip;
    bool                     m_closed_by_user{false};
    bool                     m_hidden{false};
    int                      m_last_main_frame{-1};
    std::chrono::steady_clock::time_point m_last_frame_time;
    // Guards the creation of the frame, deferred out of the render of the 3D view.
    std::shared_ptr<int>     m_alive;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_DetachedToolWindow_hpp_
