// Orio: window of a tool taken out of the 3D view (see DetachedToolWindow.hpp).
#include "DetachedToolWindow.hpp"

#include <algorithm>
#include <cmath>

#include <GL/glew.h>

#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/frame.h>
#include <wx/glcanvas.h>
#include <wx/sizer.h>

#include <imgui/imgui_internal.h>

#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "ImGuiWrapper.hpp"
#include "MainFrame.hpp"
#include "OpenGLManager.hpp"
#include "Plater.hpp"
#ifdef _MSW_DARK_MODE
#include "dark_mode.hpp"
#endif

namespace Slic3r {
namespace GUI {

namespace {

GLCanvas3D *current_canvas()
{
    Plater *plater = wxGetApp().plater();
    return plater != nullptr ? plater->get_current_canvas3D() : nullptr;
}

// The tool reads the inputs of its window when the 3D view renders: render it again (and once more, so that ImGui sees
// the release of a click after its press).
void render_3d_view_again()
{
    if (GLCanvas3D *canvas = current_canvas()) {
        canvas->set_as_dirty();
        canvas->request_extra_frame();
    }
    wxWakeUpIdle();
}

// The GL context is shared: after drawing in another window, every 3D view makes it current on its own canvas again before
// rendering (GLCanvas3D::_set_current only does it when told).
void give_context_back_to_3d_views()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    for (GLCanvas3D *canvas : {plater->get_view3D_canvas3D(), plater->get_preview_canvas3D(), plater->get_assmeble_canvas3D()})
        if (canvas != nullptr)
            canvas->mark_context_dirty();
}

} // namespace

class DetachedToolFrame : public wxFrame
{
public:
    DetachedToolFrame(DetachedToolWindow &owner, const wxString &title, const wxPoint &pos)
        : wxFrame(wxGetApp().mainframe, wxID_ANY, title, pos, wxDefaultSize,
                  wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU | wxFRAME_TOOL_WINDOW | wxFRAME_FLOAT_ON_PARENT | wxFRAME_NO_TASKBAR)
        , m_owner(&owner)
    {
        // Same pixel format as the 3D views, so that their GL context can be made current here.
        const auto     &ogl  = wxGetApp().get_opengl_manager();
        const EMSAAType msaa = (ogl && !ogl->is_fxaa_enabled()) ? ogl->get_msaa_type() : EMSAAType::Disabled;
        canvas               = OpenGLManager::create_wxglcanvas(*this, msaa);
        if (canvas != nullptr) {
            context     = wxGetApp().init_glcontext(*canvas);
            auto *sizer = new wxBoxSizer(wxVERTICAL);
            sizer->Add(canvas, 1, wxEXPAND);
            SetSizer(sizer);
#ifdef _MSW_DARK_MODE
            // The title bar of Windows dark with the dark theme, as the one of the main window.
            NppDarkMode::SetDarkTitleBar(GetHWND());
#endif

            canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
                wxPaintDC dc(canvas);
                if (m_owner != nullptr)
                    m_owner->on_paint();
            });
            canvas->Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent &) {});
            auto mouse = [this](wxMouseEvent &evt) {
                if (m_owner != nullptr)
                    m_owner->on_mouse(evt);
            };
            canvas->Bind(wxEVT_MOTION, mouse);
            canvas->Bind(wxEVT_LEFT_DOWN, mouse);
            canvas->Bind(wxEVT_LEFT_UP, mouse);
            canvas->Bind(wxEVT_LEFT_DCLICK, mouse);
            canvas->Bind(wxEVT_RIGHT_DOWN, mouse);
            canvas->Bind(wxEVT_RIGHT_UP, mouse);
            canvas->Bind(wxEVT_RIGHT_DCLICK, mouse);
            canvas->Bind(wxEVT_MIDDLE_DOWN, mouse);
            canvas->Bind(wxEVT_MIDDLE_UP, mouse);
            canvas->Bind(wxEVT_MIDDLE_DCLICK, mouse);
            canvas->Bind(wxEVT_MOUSEWHEEL, mouse);
            canvas->Bind(wxEVT_ENTER_WINDOW, mouse);
            canvas->Bind(wxEVT_LEAVE_WINDOW, mouse);
            canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
                if (m_owner != nullptr)
                    m_owner->on_capture_lost();
            });
            auto key = [this](wxKeyEvent &evt) {
                if (m_owner != nullptr)
                    m_owner->on_key(evt);
            };
            canvas->Bind(wxEVT_KEY_DOWN, key);
            canvas->Bind(wxEVT_KEY_UP, key);
            canvas->Bind(wxEVT_CHAR, key);
        }
        // The shortcuts of the main window (save, slice, preferences…), checked before the keys go to the canvas.
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &evt) {
            if (m_owner != nullptr)
                m_owner->on_char_hook(evt);
            else
                evt.Skip();
        });
        // Closing the frame puts the window back into the 3D view (the frame is destroyed by its owner).
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &evt) {
            if (m_owner == nullptr || !evt.CanVeto()) {
                evt.Skip();
                return;
            }
            evt.Veto();
            m_owner->on_close();
        });
    }

    ~DetachedToolFrame() override
    {
        if (m_owner != nullptr)
            m_owner->frame_destroyed();
    }

    // After the render of the 3D view: the size of the window of the tool, then paint it.
    void show_and_paint(const wxSize &client)
    {
        // Asked by a render of the 3D view before the tool was closed: not any more.
        if (canvas == nullptr || m_owner == nullptr || m_owner->hidden())
            return;
        if (GetClientSize() != client)
            SetClientSize(client);
        if (!IsShown())
            Show();
        canvas->Refresh(false);
    }

    void forget_owner() { m_owner = nullptr; }

    wxGLCanvas  *canvas{nullptr};
    wxGLContext *context{nullptr};

private:
    DetachedToolWindow *m_owner{nullptr};
};

DetachedToolWindow::DetachedToolWindow(const wxString &title, const wxPoint &screen_pos)
    : m_title(title), m_initial_pos(screen_pos), m_alive(std::make_shared<int>(0))
{
    // A second ImGui context with the fonts of the first one; CreateContext() leaves the current context as it was.
    ImGuiIO &main_io = ImGui::GetIO();
    m_main_context   = ImGui::GetCurrentContext();
    m_context        = ImGui::CreateContext(main_io.Fonts);
    ImGui::SetCurrentContext(m_context);
    ImGuiIO &io              = ImGui::GetIO();
    io.IniFilename           = nullptr;
    io.ConfigMacOSXBehaviors = false;
    for (int i = 0; i < ImGuiKey_COUNT; ++i)
        io.KeyMap[i] = main_io.KeyMap[i];
    io.SetClipboardTextFn = main_io.SetClipboardTextFn;
    io.GetClipboardTextFn = main_io.GetClipboardTextFn;
    io.ClipboardUserData  = main_io.ClipboardUserData;
    ImGui::SetCurrentContext(m_main_context);
    m_last_frame_time = std::chrono::steady_clock::now();

    // Created out of the render of the 3D view, which is running now.
    std::weak_ptr<int> alive = m_alive;
    wxGetApp().CallAfter([this, alive]() {
        if (alive.lock())
            this->create_frame();
    });
}

DetachedToolWindow::~DetachedToolWindow()
{
    clear_draw_lists();
    if (m_frame != nullptr) {
        m_frame->forget_owner();
        m_frame->Destroy();
        m_frame = nullptr;
    }
    if (m_context != nullptr)
        ImGui::DestroyContext(m_context);
}

void DetachedToolWindow::create_frame()
{
    if (m_frame != nullptr || m_closed_by_user || wxGetApp().mainframe == nullptr)
        return;
    m_frame = new DetachedToolFrame(*this, m_title, m_initial_pos);
    if (m_frame->canvas == nullptr || m_frame->context == nullptr) {
        // No GL canvas: back into the 3D view.
        m_closed_by_user = true;
        m_frame->forget_owner();
        m_frame->Destroy();
        m_frame = nullptr;
        render_3d_view_again();
        return;
    }
    m_frame->show_and_paint(m_extent);
}

void DetachedToolWindow::clear_draw_lists()
{
    for (ImDrawList *list : m_draw_lists)
        IM_DELETE(list);
    m_draw_lists.clear();
}

void DetachedToolWindow::begin(int flags)
{
    // Taken from the 3D view before switching: its frame, its style (with what the tool pushed), its font.
    m_last_main_frame               = ImGui::GetFrameCount();
    m_hidden                        = false;
    ImGuiStyle      style           = ImGui::GetStyle();
    ImFont         *font            = ImGui::GetFont();
    const float     font_scale      = ImGui::GetIO().FontGlobalScale;
    m_main_context                  = ImGui::GetCurrentContext();
    m_background                    = style.Colors[ImGuiCol_WindowBg];

    ImGui::SetCurrentContext(m_context);
    ImGuiIO &io = ImGui::GetIO();

    // The window auto-fits its content within the display: the display is as large as the screen of the frame, and the
    // frame takes the size of the window (with its tooltips and popups, see end()).
    wxSize screen(1920, 1080);
    if (m_frame != nullptr) {
        const int display = wxDisplay::GetFromWindow(m_frame);
        if (display != wxNOT_FOUND)
            screen = wxDisplay((unsigned int)display).GetClientArea().GetSize();
    }
    io.DisplaySize             = ImVec2((float)std::max(1, screen.x), (float)std::max(1, screen.y));
    io.DisplayFramebufferScale = ImVec2(1.f, 1.f);

    const auto now     = std::chrono::steady_clock::now();
    io.DeltaTime       = std::clamp(std::chrono::duration<float>(now - m_last_frame_time).count(), 0.0001f, 1.f);
    m_last_frame_time  = now;
    io.FontDefault     = font;
    io.FontGlobalScale = font_scale;

    // One change of the buttons per frame: a click shorter than a render of the 3D view is not lost.
    if (!m_mouse_steps.empty()) {
        const MouseStep step = m_mouse_steps.front();
        m_mouse_steps.pop_front();
        m_mouse_down = step.down;
        io.MousePos  = step.pos;
    } else
        io.MousePos = m_mouse_pos;
    for (int i = 0; i < 3; ++i)
        io.MouseDown[i] = m_mouse_down[i];
    io.MouseWheel = m_wheel;
    m_wheel       = 0.f;
    if (!m_key_steps.empty()) {
        const KeyStep step = m_key_steps.front();
        m_key_steps.pop_front();
        if (step.key >= 0 && step.key < IM_ARRAYSIZE(io.KeysDown))
            io.KeysDown[step.key] = step.down;
        io.KeyShift = step.shift;
        io.KeyCtrl  = step.ctrl;
        io.KeyAlt   = step.alt;
    }

    // The frame draws the borders: a square window filling it.
    style.WindowRounding   = 0.f;
    style.WindowBorderSize = 0.f;
    ImGui::GetStyle()      = style;

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
    ImGui::Begin("###detached_tool_window", nullptr,
                 (ImGuiWindowFlags) flags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
}

void DetachedToolWindow::end()
{
    ImGui::End();
    ImGui::Render();

    // The client size of the frame: the window of the tool and what it opened (tooltips, popups).
    ImGuiContext &g      = *ImGui::GetCurrentContext();
    float         extent_x = 1.f, extent_y = 1.f;
    for (ImGuiWindow *window : g.Windows)
        if (window != nullptr && window->Active && !window->Hidden && (window->Flags & ImGuiWindowFlags_ChildWindow) == 0) {
            extent_x = std::max(extent_x, window->Pos.x + window->Size.x);
            extent_y = std::max(extent_y, window->Pos.y + window->Size.y);
        }
    m_extent = wxSize((int)std::ceil(extent_x), (int)std::ceil(extent_y));

    clear_draw_lists();
    if (ImDrawData *data = ImGui::GetDrawData(); data != nullptr)
        for (int i = 0; i < data->CmdListsCount; ++i)
            m_draw_lists.push_back(data->CmdLists[i]->CloneOutput());
    m_wants_text_input     = ImGui::GetIO().WantTextInput;
    const bool more_inputs = !m_mouse_steps.empty() || !m_key_steps.empty();

    ImGui::SetCurrentContext(m_main_context);

    if (more_inputs)
        render_3d_view_again();
    if (m_frame != nullptr) {
        // After the render of the 3D view (running now). Dropped with the frame if it is destroyed before.
        DetachedToolFrame *frame  = m_frame;
        const wxSize       extent = m_extent;
        frame->CallAfter([frame, extent]() { frame->show_and_paint(extent); });
    }
}

void DetachedToolWindow::hide()
{
    m_hidden = true;
    if (m_frame != nullptr && m_frame->IsShown())
        m_frame->Hide();
}

wxPoint DetachedToolWindow::screen_position() const
{
    return m_frame != nullptr ? m_frame->GetPosition() : wxDefaultPosition;
}

wxPoint DetachedToolWindow::view_to_screen(const ImVec2 &pos)
{
    GLCanvas3D *canvas = current_canvas();
    wxWindow   *window = canvas != nullptr ? canvas->get_wxglcanvas() : nullptr;
    return window != nullptr ? window->ClientToScreen(wxPoint((int)pos.x, (int)pos.y)) : wxPoint((int)pos.x, (int)pos.y);
}

bool DetachedToolWindow::on_a_screen(const wxPoint &screen_pos)
{
    // A bit inside the title bar of the frame.
    return wxDisplay::GetFromPoint(wxPoint(screen_pos.x + 30, screen_pos.y + 10)) != wxNOT_FOUND;
}

void DetachedToolWindow::on_paint()
{
    if (m_frame == nullptr || m_frame->canvas == nullptr || m_frame->context == nullptr || !m_frame->canvas->IsShownOnScreen())
        return;
    if (!m_frame->canvas->SetCurrent(*m_frame->context))
        return;
    const wxSize size = m_frame->canvas->GetClientSize();
    // Left by a render of a 3D view: its frame buffer, its scissor.
    if (::glBindFramebuffer != nullptr)
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
    glsafe(::glDisable(GL_SCISSOR_TEST));
    glsafe(::glViewport(0, 0, size.x, size.y));
    glsafe(::glClearColor(m_background.x, m_background.y, m_background.z, 1.f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));

    ImDrawData data;
    data.Valid            = true;
    data.CmdLists         = m_draw_lists.empty() ? nullptr : m_draw_lists.data();
    data.CmdListsCount    = (int)m_draw_lists.size();
    data.DisplayPos       = ImVec2(0.f, 0.f);
    data.DisplaySize      = ImVec2((float)std::max(1, size.x), (float)std::max(1, size.y));
    data.FramebufferScale = ImVec2(1.f, 1.f);
    for (const ImDrawList *list : m_draw_lists) {
        data.TotalVtxCount += list->VtxBuffer.Size;
        data.TotalIdxCount += list->IdxBuffer.Size;
    }
    if (ImGuiWrapper *imgui = wxGetApp().imgui())
        imgui->render_tool_draw_data(&data);
    m_frame->canvas->SwapBuffers();

    give_context_back_to_3d_views();
}

void DetachedToolWindow::on_mouse(wxMouseEvent &evt)
{
    if (evt.Leaving())
        m_mouse_pos = ImVec2(-FLT_MAX, -FLT_MAX);
    else
        m_mouse_pos = ImVec2((float)evt.GetX(), (float)evt.GetY());
    if (evt.GetEventType() == wxEVT_MOUSEWHEEL && evt.GetWheelRotation() != 0)
        m_wheel += evt.GetWheelRotation() > 0 ? 1.f : -1.f;
    wxWindow *canvas = m_frame != nullptr ? m_frame->canvas : nullptr;
    if (canvas != nullptr && (evt.ButtonDown() || evt.ButtonDClick())) {
        // The keyboard follows the mouse into the window; the mouse is kept until released, even outside of it.
        canvas->SetFocus();
        if (!canvas->HasCapture())
            canvas->CaptureMouse();
    }
    const std::array<bool, 3> down{evt.LeftIsDown(), evt.RightIsDown(), evt.MiddleIsDown()};
    if (canvas != nullptr && evt.ButtonUp() && !down[0] && !down[1] && !down[2] && canvas->HasCapture())
        canvas->ReleaseMouse();
    if (down != m_queued_down) {
        m_mouse_steps.push_back({m_mouse_pos, down});
        m_queued_down = down;
    }
    render_3d_view_again();
}

void DetachedToolWindow::on_capture_lost()
{
    // Released somewhere we will never hear of.
    const std::array<bool, 3> up{false, false, false};
    if (m_queued_down != up) {
        m_mouse_steps.push_back({m_mouse_pos, up});
        m_queued_down = up;
    }
    render_3d_view_again();
}

void DetachedToolWindow::on_key(wxKeyEvent &evt)
{
    const wxEventType type = evt.GetEventType();
    // A key down not skipped gets no char event on Windows: the characters typed in a field, and Escape, which the 3D view
    // handles as a char (GLCanvas3D::on_char), come with it.
    if (type == wxEVT_KEY_DOWN)
        evt.Skip();
    // Typing in a field of the window: ImGui. Otherwise the 3D view, for its shortcuts (Escape, Delete, Ctrl+Z…); a key
    // released always goes to ImGui too, so that it never stays down there.
    if (m_wants_text_input || type == wxEVT_KEY_UP) {
        if (type == wxEVT_CHAR) {
            if (const wxChar c = evt.GetUnicodeKey(); c != 0) {
                ImGuiContext *previous = ImGui::GetCurrentContext();
                ImGui::SetCurrentContext(m_context);
                ImGui::GetIO().AddInputCharacter((unsigned int)c);
                ImGui::SetCurrentContext(previous);
            }
        } else
            m_key_steps.push_back({evt.GetKeyCode(), type == wxEVT_KEY_DOWN, evt.ShiftDown(), evt.ControlDown(), evt.AltDown()});
    }
    if (!m_wants_text_input) {
        GLCanvas3D *canvas = current_canvas();
        wxWindow   *window = canvas != nullptr ? canvas->get_wxglcanvas() : nullptr;
        if (window != nullptr) {
            wxKeyEvent copy(evt);
            copy.SetEventObject(window);
            window->GetEventHandler()->ProcessEvent(copy);
        }
    }
    render_3d_view_again();
}

void DetachedToolWindow::on_char_hook(wxKeyEvent &evt)
{
    // Typing in a field of the window: the field gets the keys.
    MainFrame *mainframe = wxGetApp().mainframe;
    if (m_wants_text_input || mainframe == nullptr) {
        evt.Skip();
        return;
    }
    // Otherwise the main window first, as if it had the keyboard (MainFrame handles its shortcuts in its char hook);
    // what it leaves goes on to the canvas of this window, which hands it to the 3D view (on_key).
    wxKeyEvent copy(evt);
    copy.SetEventObject(mainframe);
    if (!mainframe->GetEventHandler()->ProcessEvent(copy))
        evt.Skip();
}

void DetachedToolWindow::set_tooltip(const wxString &tooltip)
{
    if (tooltip == m_tooltip)
        return;
    m_tooltip = tooltip;
    if (m_frame == nullptr || m_frame->canvas == nullptr)
        return;
    if (tooltip.empty())
        m_frame->canvas->UnsetToolTip();
    else
        m_frame->canvas->SetToolTip(tooltip);
}

void DetachedToolWindow::on_close()
{
    m_closed_by_user = true;
    hide();
    render_3d_view_again();
}

} // namespace GUI
} // namespace Slic3r
