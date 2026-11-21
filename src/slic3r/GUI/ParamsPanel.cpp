///////////////////////////////////////////////////////////////////////////
// C++ code generated with wxFormBuilder (version 3.10.0-4761b0c)
// http://www.wxformbuilder.org/
//
// PLEASE DO *NOT* EDIT THIS FILE!
///////////////////////////////////////////////////////////////////////////
#include <set>
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "ParamsPanel.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "MainFrame.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"

#include "Widgets/Label.hpp"
#include "Widgets/SwitchButton.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StateColor.hpp"
#include "GUI_Factories.hpp"

#include <wx/dcbuffer.h>


namespace Slic3r {
namespace GUI {

//Orio: row of sheet tabs, as in a spreadsheet, at the top of the process settings in Global mode: "Global", then the
// plates having a "Traitement" sheet, each with a cross to delete it. Arrows to scroll when they don't fit.
class PlateSheetBar : public wxWindow
{
public:
    struct Item
    {
        int      plate_index; // -1: the global settings
        wxString label;
    };

    PlateSheetBar(wxWindow *parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, FromDIP(30)));
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(this);
            render(dc);
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent &evt) { on_motion(evt.GetPosition()); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &) { set_hover(-1, false); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &evt) { on_click(evt.GetPosition()); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
            Refresh();
            evt.Skip();
        });
    }

    std::function<void(int)> on_select; // plate index, -1: the global settings
    std::function<void(int)> on_delete; // plate index

    void set_items(std::vector<Item> items, int active)
    {
        m_scroll_to_active |= active != m_active || items.size() != m_items.size();
        m_items  = std::move(items);
        m_active = active;
        m_first  = std::clamp(m_first, 0, std::max(0, int(m_items.size()) - 1));
        m_hover  = -1;
        Refresh();
    }
    bool has_sheets() const { return m_items.size() > 1; }
    void msw_rescale()
    {
        SetMinSize(wxSize(-1, FromDIP(30)));
        Refresh();
    }

private:
    static wxColour color(const char *rgb)
    {
        const wxColour c(rgb);
        return GUI_App::dark_mode() ? StateColor::darkModeColorFor(c) : c;
    }

    int tab_width(wxDC &dc, const Item &item, bool active) const
    {
        dc.SetFont(active ? Label::Body_13.Bold() : Label::Body_13);
        int width = dc.GetTextExtent(item.label).GetWidth() + 2 * FromDIP(12);
        if (item.plate_index >= 0)
            width += FromDIP(8) + FromDIP(8); // gap and cross
        return width;
    }

    // Rectangles of the tabs from m_first, of the cross of each plate tab, and of the scroll arrows when the tabs don't fit.
    void layout(wxDC &dc)
    {
        const wxSize size  = GetClientSize();
        const int    gap   = FromDIP(2);
        const int    top   = FromDIP(4);
        std::vector<int> widths;
        int total = 0;
        for (const Item &item : m_items) {
            widths.emplace_back(tab_width(dc, item, item.plate_index == m_active));
            total += widths.back() + gap;
        }
        const bool overflow = total > size.GetWidth();
        const int  arrows   = overflow ? 2 * FromDIP(18) : 0;
        const int  right    = size.GetWidth() - arrows;
        auto last_shown = [&]() {
            int x = 0, last = m_first - 1;
            for (int i = m_first; i < int(m_items.size()) && x + widths[i] <= right; ++ i) {
                x += widths[i] + gap;
                last = i;
            }
            return last;
        };
        if (! overflow)
            m_first = 0;
        else if (m_scroll_to_active) {
            const int active = int(std::find_if(m_items.begin(), m_items.end(), [this](const Item &item) { return item.plate_index == m_active; }) - m_items.begin());
            if (active < int(m_items.size())) {
                if (active < m_first)
                    m_first = active;
                while (m_first < active && last_shown() < active)
                    ++ m_first;
            }
        }
        m_scroll_to_active = false;
        m_tab_rects.assign(m_items.size(), wxRect());
        m_close_rects.assign(m_items.size(), wxRect());
        int x = 0;
        for (int i = m_first; i < int(m_items.size()) && x + widths[i] <= right; ++ i) {
            m_tab_rects[i] = wxRect(x, top, widths[i], size.GetHeight() - top);
            if (m_items[i].plate_index >= 0) {
                const int cross = FromDIP(8);
                m_close_rects[i] = wxRect(x + widths[i] - FromDIP(12) - cross, top + (size.GetHeight() - top - cross) / 2, cross, cross);
            }
            x += widths[i] + gap;
        }
        m_prev_rect = overflow ? wxRect(right, top, arrows / 2, size.GetHeight() - top) : wxRect();
        m_next_rect = overflow ? wxRect(right + arrows / 2, top, arrows / 2, size.GetHeight() - top) : wxRect();
    }

    void render(wxDC &dc)
    {
        const wxSize   size       = GetClientSize();
        const wxColour background = color("#FFFFFF");
        const wxColour border     = color("#CECECE");
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        layout(dc);
        const int radius = FromDIP(4);
        for (size_t i = 0; i < m_items.size(); ++ i) {
            const wxRect &rect = m_tab_rects[i];
            if (rect.IsEmpty())
                continue;
            const bool active = m_items[i].plate_index == m_active;
            const bool hover  = int(i) == m_hover;
            {
                // Rounded top corners only.
                wxDCClipper clipper(dc, rect);
                dc.SetPen(wxPen(border, 1));
                dc.SetBrush(wxBrush(active ? background : color(hover ? "#E4E4E4" : "#F0F0F0")));
                dc.DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height + radius, radius);
            }
            if (active) {
                // Green line on top of the active tab.
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(wxColour("#0877E8")));
                dc.DrawRectangle(rect.x + radius / 2, rect.y, rect.width - radius, FromDIP(2));
            }
            dc.SetFont(active ? Label::Body_13.Bold() : Label::Body_13);
            dc.SetTextForeground(color(active ? "#262E30" : "#6B6B6B"));
            const wxSize text = dc.GetTextExtent(m_items[i].label);
            dc.DrawText(m_items[i].label, rect.x + FromDIP(12), rect.y + (rect.height - text.GetHeight()) / 2);
            const wxRect &cross = m_close_rects[i];
            if (! cross.IsEmpty()) {
                const bool hover_cross = hover && m_hover_close;
                if (hover_cross) {
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush(color("#FCE4E4")));
                    dc.DrawCircle(cross.GetLeft() + cross.width / 2, cross.GetTop() + cross.height / 2, cross.width);
                }
                dc.SetPen(wxPen(hover_cross ? wxColour("#E14747") : color("#909090"), std::max(1, FromDIP(1))));
                dc.DrawLine(cross.GetLeft(), cross.GetTop(), cross.GetRight() + 1, cross.GetBottom() + 1);
                dc.DrawLine(cross.GetLeft(), cross.GetBottom(), cross.GetRight() + 1, cross.GetTop() - 1);
            }
        }
        // Line under the tabs, but under the active one which opens on its settings.
        dc.SetPen(wxPen(border, 1));
        int x = 0;
        for (size_t i = 0; i < m_items.size(); ++ i)
            if (! m_tab_rects[i].IsEmpty() && m_items[i].plate_index == m_active) {
                dc.DrawLine(x, size.GetHeight() - 1, m_tab_rects[i].GetLeft() + 1, size.GetHeight() - 1);
                x = m_tab_rects[i].GetRight();
            }
        dc.DrawLine(x, size.GetHeight() - 1, size.GetWidth(), size.GetHeight() - 1);
        // Scroll arrows.
        for (const auto &[rect, prev] : { std::make_pair(m_prev_rect, true), std::make_pair(m_next_rect, false) }) {
            if (rect.IsEmpty())
                continue;
            const bool enabled = prev ? m_first > 0 : m_tab_rects.empty() || m_tab_rects.back().IsEmpty();
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(color(enabled ? "#6B6B6B" : "#CECECE")));
            const int  h = FromDIP(5);
            const int  cx = rect.x + rect.width / 2, cy = rect.y + rect.height / 2;
            wxPoint    points[3];
            if (prev) {
                points[0] = wxPoint(cx - h / 2, cy);
                points[1] = wxPoint(cx + h / 2, cy - h);
                points[2] = wxPoint(cx + h / 2, cy + h);
            } else {
                points[0] = wxPoint(cx + h / 2, cy);
                points[1] = wxPoint(cx - h / 2, cy - h);
                points[2] = wxPoint(cx - h / 2, cy + h);
            }
            dc.DrawPolygon(3, points);
        }
    }

    // Item under the point and whether on its cross, -1 for none.
    std::pair<int, bool> hit(const wxPoint &pt) const
    {
        for (size_t i = 0; i < m_tab_rects.size(); ++ i)
            if (m_tab_rects[i].Contains(pt)) {
                // The cross with a margin, easier to click (no const wxRect::Inflate() in this wxWidgets).
                wxRect cross = m_close_rects[i];
                cross.Inflate(FromDIP(4));
                return { int(i), ! m_close_rects[i].IsEmpty() && cross.Contains(pt) };
            }
        return { -1, false };
    }

    void set_hover(int item, bool close)
    {
        if (item == m_hover && close == m_hover_close)
            return;
        m_hover       = item;
        m_hover_close = close;
        SetToolTip(item >= 0 && close ? _L("Delete this sheet") :
                   item >= 0 && m_items[item].plate_index >= 0 ? _L("Process and printer settings of this plate only") : wxString());
        Refresh();
    }

    void on_motion(const wxPoint &pt)
    {
        const auto [item, close] = hit(pt);
        set_hover(item, close);
    }

    void on_click(const wxPoint &pt)
    {
        if (m_prev_rect.Contains(pt)) {
            m_first = std::max(0, m_first - 1);
            Refresh();
            return;
        }
        if (m_next_rect.Contains(pt)) {
            if (! m_tab_rects.empty() && m_tab_rects.back().IsEmpty())
                m_first = std::min(int(m_items.size()) - 1, m_first + 1);
            Refresh();
            return;
        }
        const auto [item, close] = hit(pt);
        if (item < 0)
            return;
        const int plate_index = m_items[item].plate_index;
        if (close) {
            if (on_delete)
                on_delete(plate_index);
        } else if (on_select)
            on_select(plate_index);
    }

    std::vector<Item>   m_items;
    std::vector<wxRect> m_tab_rects;   // by item, empty when not shown
    std::vector<wxRect> m_close_rects; // by item, empty for "Global"
    wxRect              m_prev_rect;
    wxRect              m_next_rect;
    int                 m_active { -1 }; // plate index of the active tab
    int                 m_first { 0 };   // first tab shown
    int                 m_hover { -1 };  // item under the mouse
    bool                m_hover_close { false };
    bool                m_scroll_to_active { true };
};


TipsDialog::TipsDialog(wxWindow *parent, const wxString &title, const wxString &description, std::string app_key, long style,std::map<wxStandardID,wxString> option_map)
    : DPIDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX),
    m_app_key(app_key)
{
    SetBackgroundColour(*wxWHITE);
    std::string icon_path = (boost::format("%1%/images/BambuStudioTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(wxColour(166, 169, 170));

    m_sizer_main->Add(m_top_line, 0, wxEXPAND, 0);

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(20));

    m_msg = new wxStaticText(this, wxID_ANY, description, wxDefaultPosition, wxDefaultSize, 0);
    m_msg->Wrap(-1);
    m_msg->SetFont(::Label::Body_13);
    m_msg->SetForegroundColour(wxColour(107, 107, 107));
    m_msg->SetBackgroundColour(wxColour(255, 255, 255));

    m_sizer_main->Add(m_msg, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(40));

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(5));

    wxBoxSizer *m_sizer_bottom = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *m_sizer_left   = new wxBoxSizer(wxHORIZONTAL);

    auto dont_show_again = create_item_checkbox(_L("Don't show again"), this, _L("Don't show again"), "do_not_show_tips");
    m_sizer_left->Add(dont_show_again, 1, wxALL, FromDIP(0));

    m_sizer_bottom->Add(m_sizer_left, 1, wxEXPAND, FromDIP(5));

    wxBoxSizer *m_sizer_right = new wxBoxSizer(wxHORIZONTAL);

    if (style & wxOK) {
        wxString str = _L("OK");
        if (auto iter = option_map.find(wxID_OK); iter != option_map.end())
            str = iter->second;
        Button* btn = add_button(wxID_OK, str, true);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }
    if (style & wxYES) {
        wxString str = _L("Yes");
        if (auto iter = option_map.find(wxID_YES); iter != option_map.end())
            str = iter->second;
        Button *btn = add_button(wxID_YES, str, true);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }
    if (style & wxNO) {
        wxString str = _L("No");
        if (auto iter = option_map.find(wxID_NO); iter != option_map.end())
            str = iter->second;
        Button *btn = add_button(wxID_NO, str, false);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }
    if (style & wxCANCEL) {
        wxString str = _L("Cancel");
        if (auto iter = option_map.find(wxID_CANCEL); iter != option_map.end())
            str = iter->second;
        Button *btn = add_button(wxID_CANCEL, str, false);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }

    m_sizer_bottom->Add(m_sizer_right, 0, wxEXPAND, FromDIP(5));
    m_sizer_main->Add(m_sizer_bottom, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(40));
    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(20));

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    Centre(wxBOTH);

    wxGetApp().UpdateDlgDarkUI(this);
}

wxBoxSizer *TipsDialog::create_item_checkbox(wxString title, wxWindow *parent, wxString tooltip, std::string param)
{
    wxBoxSizer *m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);

     //m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 5);

    auto checkbox = new ::CheckBox(parent);
    m_sizer_checkbox->Add(checkbox, 0, wxALIGN_CENTER, 0);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxSize(-1, -1), 0);
    checkbox_title->SetForegroundColour(wxColour(144, 144, 144));
    checkbox_title->SetFont(::Label::Body_13);
    checkbox_title->Wrap(-1);
    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);

    m_show_again = wxGetApp().app_config->has(param);
    checkbox->SetValue(m_show_again);

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, param](wxCommandEvent &e) {
        m_show_again = m_show_again ? false : true;
        e.Skip();
    });

    return m_sizer_checkbox;
}

Button *TipsDialog::add_button(wxWindowID btn_id, const wxString &label, bool set_focus /*= false*/)
{
    Button* btn = new Button(this, label, "", 0, 0, btn_id);
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(5, 96, 194), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(43, 140, 242), StateColor::Hovered),
                            std::pair<wxColour, int>(wxColour(8, 116, 224), StateColor::Normal));

    StateColor btn_bd_green(std::pair<wxColour, int>(wxColour(8, 116, 224), StateColor::Normal));

    StateColor btn_text_green(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Normal));

    StateColor btn_bg_white(
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal)
    );

    StateColor btn_bd_white(std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Normal));

    StateColor btn_text_white(std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Normal));

    if (btn_id == wxID_OK || btn_id == wxID_YES) {
        btn->SetBackgroundColor(btn_bg_green);
        btn->SetBorderColor(btn_bd_green);
        btn->SetTextColor(btn_text_green);
    }

    if (btn_id == wxID_CANCEL || btn_id == wxID_NO) {
        btn->SetBackgroundColor(btn_bg_white);
        btn->SetBorderColor(btn_bd_white);
        btn->SetTextColor(btn_text_white);
    }

    if (set_focus)
        btn->SetFocus();

    btn->SetSize(TIPS_DIALOG_BUTTON_SIZE);
    btn->SetMinSize(TIPS_DIALOG_BUTTON_SIZE);
    btn->SetCornerRadius(FromDIP(12));
    btn->Bind(wxEVT_BUTTON, [this, btn_id](wxCommandEvent &) {
        if (m_show_again) {
            if (!m_app_key.empty()) {
                if (btn_id == wxID_OK || btn_id == wxID_YES) {
                    wxGetApp().app_config->set_bool(m_app_key, true);
                }

                if (btn_id == wxID_NO) {
                    wxGetApp().app_config->set_bool(m_app_key, false);
                }
            }
        }
        EndModal(btn_id);
    });
    return btn;
}

void TipsDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    if (m_confirm) m_confirm->SetMinSize(TIPS_DIALOG_BUTTON_SIZE);
    if (m_cancel) m_cancel->SetMinSize(TIPS_DIALOG_BUTTON_SIZE);
    Fit();
    Refresh();
}

void ParamsPanel::Highlighter::set_timer_owner(wxEvtHandler *owner, int timerid /* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void ParamsPanel::Highlighter::init(std::pair<wxWindow *, bool *> params, wxWindow *parent)
    {
    if (m_timer.IsRunning()) invalidate();
    if (!params.first || !params.second) return;

    m_timer.Start(300, false);

    m_bitmap         = params.first;
    m_show_blink_ptr = params.second;
    m_parent         = parent;

    *m_show_blink_ptr = true;
    }

void ParamsPanel::Highlighter::invalidate()
{
    m_timer.Stop();

    if (m_bitmap && m_show_blink_ptr) {
        *m_show_blink_ptr = false;
        m_bitmap->Show(*m_show_blink_ptr);
        if (m_parent) {
            m_parent->Layout();
            m_parent->Refresh();
        }
        m_show_blink_ptr = nullptr;
        m_bitmap         = nullptr;
        m_parent         = nullptr;
    }

    m_blink_counter = 0;
}

void ParamsPanel::Highlighter::blink()
{
    if (m_bitmap && m_show_blink_ptr) {
        *m_show_blink_ptr = !*m_show_blink_ptr;
        m_bitmap->Show(*m_show_blink_ptr);
        if (m_parent) {
            m_parent->Layout();
            m_parent->Refresh();
        }
    } else
        return;

    if ((++m_blink_counter) == 11) invalidate();
}

//Orio: text of the "Global" / "Objects" buttons; "Objects" in the "modified" color when objects have their own
// settings (0xfffffe: white kept as is in the dark theme).
static StateColor mode_button_text_color(bool modified)
{
    return StateColor(std::pair{0xfffffe, (int) StateColor::Checked},
                      std::pair{modified ? wxGetApp().get_label_clr_modified() : wxColour("#262E30"), 0});
}

ParamsPanel::ParamsPanel( wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name )
    : wxPanel( parent, id, pos, size, style, name )
{
    // BBS: new layout
    SetBackgroundColour(*wxWHITE);
#if __WXOSX__
    m_top_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_top_sizer->SetSizeHints(this);
    this->SetSizer(m_top_sizer);

    // Create additional panel to Fit() it from OnActivate()
    // It's needed for tooltip showing on OSX
    m_tmp_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    auto  sizer = new wxBoxSizer(wxHORIZONTAL);
    m_tmp_panel->SetSizer(sizer);
    m_tmp_panel->Layout();

#else
    ParamsPanel*panel = this;
    m_top_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_top_sizer->SetSizeHints(panel);
    panel->SetSizer(m_top_sizer);
#endif //__WXOSX__

    if (dynamic_cast<Notebook*>(parent)) {
        // BBS: new layout
        m_top_panel = new StaticBox(this, wxID_ANY, wxDefaultPosition);
        m_top_panel->SetBackgroundColor(0xF8F8F8);
        m_top_panel->SetBackgroundColor2(0xF1F1F1);

        m_process_icon = new ScalableButton(m_top_panel, wxID_ANY, "process");

        m_title_label = new Label(m_top_panel, _L("Process"));

        //int width, height;
        // BBS: new layout
        m_mode_region = new SwitchButton(m_top_panel);
        m_mode_region->SetMaxSize({em_unit(this) * 12, -1});
        m_mode_region->SetLabels(_L("Global"), _L("Objects"));
        //m_mode_region->GetSize(&width, &height);
        //Orio: two separate buttons instead of the switch, which stays (hidden) as the state for all its users.
        m_mode_region->Hide();
        m_global_btn  = new Button(m_top_panel, _L("Global"));
        m_objects_btn = new Button(m_top_panel, _L("Objects"));
        for (Button *btn : {m_global_btn, m_objects_btn}) {
            const bool objects = btn == m_objects_btn;
            btn->SetFont(Label::Head_14);
            btn->SetPaddingSize(wxSize(FromDIP(14), FromDIP(4)));
            btn->SetCornerRadius(FromDIP(12));
            btn->SetCanFocus(false);
            btn->SetBackgroundColor(StateColor(std::pair{wxColour("#0877E8"), (int) StateColor::Checked},
                                               std::pair{*wxWHITE, (int) StateColor::Normal}));
            btn->SetBorderColor(StateColor(std::pair{wxColour("#0874E0"), (int) StateColor::Checked},
                                           std::pair{wxColour("#0874E0"), (int) StateColor::Hovered},
                                           std::pair{wxColour("#DBDBDB"), (int) StateColor::Normal}));
            btn->SetTextColor(mode_button_text_color(false));
            btn->Bind(wxEVT_BUTTON, [this, objects](wxCommandEvent &) {
                if (m_mode_region->GetValue() == objects)
                    return;
                m_mode_region->SetValue(objects);
                wxWindowUpdateLocker locker(GetParent());
                set_active_tab(nullptr);
            });
        }
        m_global_btn->SetValue(true);
        m_tips_arrow = new ScalableButton(m_top_panel, wxID_ANY, "tips_arrow");
        m_tips_arrow->Hide();

        m_title_view = new Label(m_top_panel, _L("Advance"));
        m_mode_view = new SwitchButton(m_top_panel, wxID_ABOUT);

        // BBS: new layout
        //m_search_btn = new ScalableButton(m_top_panel, wxID_ANY, "search", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
        //m_search_btn->SetToolTip(format_wxstr(_L("Search in settings [%1%]"), "Ctrl+F"));
        //m_search_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().plater()->search(false); });

        m_compare_btn = new ScalableButton(m_top_panel, wxID_ANY, "compare", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true);
        m_compare_btn->SetToolTip(_L("Compare presets"));
        m_compare_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { wxGetApp().mainframe->diff_dialog.show(); }));

        m_highlighter.set_timer_owner(this, 0);
        this->Bind(wxEVT_TIMER, [this](wxTimerEvent &)
        {
            m_highlighter.blink();
        });

        //Orio: tabs of the "Traitement" sheets of the plates.
        m_sheet_bar = new PlateSheetBar(this);
        m_sheet_bar->Hide();
        m_sheet_bar->on_select = [this](int plate_index) {
            if (plate_index < 0)
                show_plate_sheet(-1);
            else
                wxGetApp().plater()->show_plate_sheet(plate_index);
        };
        m_sheet_bar->on_delete = [](int plate_index) { wxGetApp().plater()->delete_plate_sheet(plate_index); };
        this->Bind(wxEVT_IDLE, [this](wxIdleEvent &evt) {
            update_plate_sheets();
            evt.Skip();
        });
    }

    //m_export_to_file = new Button( this, wxT("Export To File"), "");
    //m_import_from_file = new Button( this, wxT("Import From File") );

    // Initialize the page.
#if __WXOSX__
    auto page_parent = m_tmp_panel;
#else
    auto page_parent = this;
#endif

    // BBS: fix scroll to tip view
    class PageScrolledWindow : public wxScrolledWindow
    {
    public:
        PageScrolledWindow(wxWindow *parent)
            : wxScrolledWindow(parent,
                               wxID_ANY,
                               wxDefaultPosition,
                               wxDefaultSize,
                               wxVSCROLL) // hide hori-bar will cause hidden field mis-position
        {
            // ShowScrollBar(GetHandle(), SB_BOTH, FALSE);
            Bind(wxEVT_SCROLL_CHANGED, [this](auto &e) {
                wxWindow *child = dynamic_cast<wxWindow *>(e.GetEventObject());
                if (child != this)
                    EnsureVisible(child);
            });
        }
        virtual bool ShouldScrollToChildOnFocus(wxWindow *child)
        {
            EnsureVisible(child);
            return false;
        }
        void EnsureVisible(wxWindow* win)
        {
            const wxRect viewRect(m_targetWindow->GetClientRect());
            const wxRect winRect(m_targetWindow->ScreenToClient(win->GetScreenPosition()), win->GetSize());
            if (viewRect.Contains(winRect)) {
                return;
            }
            if (winRect.GetWidth() > viewRect.GetWidth() || winRect.GetHeight() > viewRect.GetHeight()) {
                return;
            }
            int stepx, stepy;
            GetScrollPixelsPerUnit(&stepx, &stepy);

            int startx, starty;
            GetViewStart(&startx, &starty);
            // first in vertical direction:
            if (stepy > 0) {
                int diff = 0;

                if (winRect.GetTop() < 0) {
                    diff = winRect.GetTop();
                } else if (winRect.GetBottom() > viewRect.GetHeight()) {
                    diff = winRect.GetBottom() - viewRect.GetHeight() + 1;
                    // round up to next scroll step if we can't get exact position,
                    // so that the window is fully visible:
                    diff += stepy - 1;
                }
                starty = (starty * stepy + diff) / stepy;
            }
            // then horizontal:
            if (stepx > 0) {
                int diff = 0;
                if (winRect.GetLeft() < 0) {
                    diff = winRect.GetLeft();
                } else if (winRect.GetRight() > viewRect.GetWidth()) {
                    diff = winRect.GetRight() - viewRect.GetWidth() + 1;
                    // round up to next scroll step if we can't get exact position,
                    // so that the window is fully visible:
                    diff += stepx - 1;
                }
                startx = (startx * stepx + diff) / stepx;
            }
            Scroll(startx, starty);
        }
    };

    m_page_view = new PageScrolledWindow(page_parent);
    m_page_view->SetBackgroundColour(*wxWHITE);
    m_page_sizer = new wxBoxSizer(wxVERTICAL);

    m_page_view->SetSizer(m_page_sizer);
    m_page_view->SetScrollbars(1, 20, 1, 2);
    //m_page_view->SetScrollRate( 5, 5 );

    if (m_mode_region)
        m_mode_region->Bind(wxEVT_TOGGLEBUTTON, &ParamsPanel::OnToggled, this);
    if (m_mode_view)
        m_mode_view->Bind(wxEVT_TOGGLEBUTTON, &ParamsPanel::OnToggled, this);
    Bind(wxEVT_TOGGLEBUTTON, &ParamsPanel::OnToggled, this); // For Tab's mode switch
    //Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().plater()->search(false); }, wxID_FIND);
    //m_export_to_file->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().mainframe->export_config(); });
    //m_import_from_file->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().mainframe->load_config_file(); });
}

void ParamsPanel::create_layout()
{
#ifdef __WINDOWS__
    this->SetDoubleBuffered(true);
    m_page_view->SetDoubleBuffered(true);
#endif //__WINDOWS__

    m_left_sizer = new wxBoxSizer( wxVERTICAL );
    // BBS: new layout
    m_left_sizer->SetMinSize( wxSize(40 * em_unit(this), -1 ) );

    if (m_top_panel) {
        m_mode_sizer = new wxBoxSizer( wxHORIZONTAL );
        m_mode_sizer->AddSpacer(FromDIP(10));
        m_mode_sizer->Add(m_process_icon, 0, wxALIGN_CENTER);
        m_mode_sizer->AddSpacer(FromDIP(10));
        m_mode_sizer->Add( m_title_label, 0, wxALIGN_CENTER );
        //Orio: "Global" / "Objects" centered between the title and the right buttons.
        m_mode_sizer->AddStretchSpacer(1);
        //Orio: 6 px above and below the buttons (the row grows with them): away from the green line on top.
        m_mode_sizer->Add(m_global_btn, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(6));
        m_mode_sizer->Add(m_objects_btn, 0, wxALIGN_CENTER | wxLEFT, FromDIP(6));
        m_mode_sizer->Add(m_tips_arrow, 0, wxALIGN_CENTER | wxLEFT, FromDIP(4));
        m_mode_sizer->AddStretchSpacer(1);
        m_mode_sizer->Add( m_title_view, 0, wxALIGN_CENTER );
        m_mode_sizer->AddSpacer(FromDIP(2));
        m_mode_sizer->Add(m_mode_view, 0, wxALIGN_CENTER);
        m_mode_sizer->AddSpacer(FromDIP(12));
        m_mode_sizer->Add(m_compare_btn, 0, wxALIGN_CENTER);
        m_mode_sizer->AddSpacer(FromDIP(16));
        //m_mode_sizer->Add( m_search_btn, 0, wxALIGN_CENTER );
        //m_mode_sizer->AddSpacer(16);
        m_mode_sizer->SetMinSize(-1, FromDIP(30));
        m_top_panel->SetSizer(m_mode_sizer);
        //m_left_sizer->Add( m_top_panel, 0, wxEXPAND );
    }

    //Orio: tabs of the sheets of the plates, above the process settings.
    if (m_sheet_bar)
        m_left_sizer->Add(m_sheet_bar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

    if (m_tab_print) {
        //m_print_sizer = new wxBoxSizer( wxHORIZONTAL );
        //m_print_sizer->Add( m_tab_print, 1, wxEXPAND | wxALL, 5 );
        //m_left_sizer->Add( m_print_sizer, 1, wxEXPAND, 5 );
        m_left_sizer->Add( m_tab_print, 0, wxEXPAND );
    }

    //Orio: "Traitement" sheet of a plate.
    if (m_tab_plate_sheet)
        m_left_sizer->Add(m_tab_plate_sheet, 0, wxEXPAND);

    if (m_tab_print_plate) {
        m_left_sizer->Add(m_tab_print_plate, 0, wxEXPAND);
    }

    if (m_tab_print_object) {
        m_left_sizer->Add( m_tab_print_object, 0, wxEXPAND );
    }

    if (m_tab_print_part) {
        m_left_sizer->Add( m_tab_print_part, 0, wxEXPAND );
    }

    if (m_tab_print_layer) {
        m_left_sizer->Add(m_tab_print_layer, 0, wxEXPAND);
    }

    if (m_tab_filament) {
        //m_filament_sizer = new wxBoxSizer( wxVERTICAL );
        //m_filament_sizer->Add( m_tab_filament, 1, wxEXPAND | wxALL, 5 );
       // m_left_sizer->Add( m_filament_sizer, 1, wxEXPAND, 5 );
        m_left_sizer->Add( m_tab_filament, 0, wxEXPAND );
    }

    if (m_tab_printer) {
        //m_printer_sizer = new wxBoxSizer( wxVERTICAL );
        //m_printer_sizer->Add( m_tab_printer, 1, wxEXPAND | wxALL, 5 );
        m_left_sizer->Add( m_tab_printer, 0, wxEXPAND );
    }

    //m_left_sizer->Add( m_printer_sizer, 1, wxEXPAND, 1 );

    //m_button_sizer = new wxBoxSizer( wxHORIZONTAL );

    //m_button_sizer->Add( m_export_to_file, 0, wxALL, 5 );

    //m_button_sizer->Add( m_import_from_file, 0, wxALL, 5 );

    //m_left_sizer->Add( m_button_sizer, 0, wxALIGN_CENTER, 5 );

    m_top_sizer->Add(m_left_sizer, 1, wxEXPAND);

    //m_right_sizer = new wxBoxSizer( wxVERTICAL );

    //m_right_sizer->Add( m_page_view, 1, wxEXPAND | wxALL, 5 );

    //m_top_sizer->Add( m_right_sizer, 1, wxEXPAND, 5 );
    // BBS: new layout
    m_left_sizer->AddSpacer(6 * em_unit(this) / 10);
#if __WXOSX__
    m_left_sizer->Add(m_tmp_panel, 1, wxEXPAND | wxALL, 0);
    m_tmp_panel->GetSizer()->Add( m_page_view, 1, wxEXPAND );
#else
    m_left_sizer->Add( m_page_view, 1, wxEXPAND );
#endif

    //this->SetSizer( m_top_sizer );
    this->Layout();
}

void ParamsPanel::rebuild_panels()
{
    refresh_tabs();
    free_sizers();
    create_layout();
}

void ParamsPanel::refresh_tabs()
{
    auto& tabs_list = wxGetApp().tabs_list;
    auto print_tech = wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology();
    for (auto tab : tabs_list)
        if (tab->supports_printer_technology(print_tech))
        {
            if (tab->GetParent() != this) continue;
            switch (tab->type())
            {
                case Preset::TYPE_PRINT:
                case Preset::TYPE_SLA_PRINT:
                    m_tab_print = tab;
                    break;

                case Preset::TYPE_FILAMENT:
                case Preset::TYPE_SLA_MATERIAL:
                    m_tab_filament = tab;
                    break;

                case Preset::TYPE_PRINTER:
                    m_tab_printer = tab;
                    break;
                default:
                    break;
            }
        }
    if (m_top_panel) {
        m_tab_print_plate = wxGetApp().get_plate_tab();
        m_tab_print_object = wxGetApp().get_model_tab();
        m_tab_print_part = wxGetApp().get_model_tab(true);
        m_tab_print_layer = wxGetApp().get_layer_tab();
        //Orio: "Traitement" sheet of a plate.
        m_tab_plate_sheet = wxGetApp().get_plate_sheet_tab();
    }
    return;
}

void ParamsPanel::clear_page()
{
    if (m_page_sizer)
        m_page_sizer->Clear(true);
}


void ParamsPanel::OnActivate()
{
    if (m_current_tab == NULL)
    {
        //the first time
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": first time opened, set current tab to print");
        // BBS: open/close tab
        //m_current_tab = m_tab_print;
        set_active_tab(m_tab_print ? m_tab_print : m_tab_filament);
    }
    Tab* cur_tab = dynamic_cast<Tab *> (m_current_tab);
    if (cur_tab)
        cur_tab->OnActivate();
}

void ParamsPanel::OnToggled(wxCommandEvent& event)
{
    if (m_mode_region && m_mode_region->GetId() == event.GetId()) {
        wxWindowUpdateLocker locker(GetParent());
        set_active_tab(nullptr);
        event.Skip();
        return;
    }

    if (wxID_ABOUT != event.GetId()) {
        return;
    }

    // this is from tab's mode switch
    bool value = dynamic_cast<SwitchButton*>(event.GetEventObject())->GetValue();
    int mode_id;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": Advanced mode toogle to %1%") % value;

    if (value)
    {
        //m_mode_region->SetBitmap(m_toggle_on_icon);
        mode_id = comAdvanced;
    }
    else
    {
        //m_mode_region->SetBitmap(m_toggle_off_icon);
        mode_id = comSimple;
    }

    Slic3r::GUI::wxGetApp().save_mode(mode_id);
    event.Skip();
}

// This is special, DO NOT call it from outer except from Tab
void ParamsPanel::set_active_tab(wxPanel* tab)
{
    Tab* cur_tab = dynamic_cast<Tab *> (tab);

    if (cur_tab == nullptr) {
        //Orio: the buttons show the state of the hidden switch (every change of it comes here).
        if (m_global_btn != nullptr) {
            m_global_btn->SetValue(!m_mode_region->GetValue());
            m_objects_btn->SetValue(m_mode_region->GetValue());
        }
        if (!m_mode_region->GetValue()) {
            //Orio: the "Traitement" sheet of a plate, else the global process settings.
            auto *sheet_tab = dynamic_cast<TabPlateSheet*>(m_tab_plate_sheet);
            if (sheet_tab != nullptr && m_active_sheet >= 0) {
                sheet_tab->load_plate(m_active_sheet);
                cur_tab = sheet_tab;
            } else
                cur_tab = (Tab*) m_tab_print;
        } else if (m_tab_print_part && ((TabPrintModel*) m_tab_print_part)->has_model_config()) {
            cur_tab = (Tab*) m_tab_print_part;
        } else if (m_tab_print_layer && ((TabPrintModel*)m_tab_print_layer)->has_model_config()) {
            cur_tab = (Tab*)m_tab_print_layer;
        } else if (m_tab_print_object && ((TabPrintModel*) m_tab_print_object)->has_model_config()) {
            cur_tab = (Tab*) m_tab_print_object;
        } else if (m_tab_print_plate && ((TabPrintPlate*)m_tab_print_plate)->has_model_config()) {
            cur_tab = (Tab*)m_tab_print_plate;
        }
        Show(cur_tab != nullptr);
        wxGetApp().sidebar().show_object_list(m_mode_region->GetValue());
        //Orio: the tabs of the sheets in Global mode.
        if (m_sheet_bar != nullptr && m_sheet_bar->IsShown() != (!m_mode_region->GetValue() && m_sheet_bar->has_sheets())) {
            m_sheet_bar->Show(!m_mode_region->GetValue() && m_sheet_bar->has_sheets());
            Layout();
        }
        if (m_current_tab == cur_tab)
            return;
        if (cur_tab)
            cur_tab->restore_last_select_item();
        return;
    }

    m_current_tab = tab;
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": set current to %1%, type=%2%") % cur_tab % cur_tab?cur_tab->type():-1;
    //Orio: the global process settings shown (search...): no sheet of a plate active.
    if (tab == m_tab_print && m_active_sheet >= 0) {
        m_active_sheet = -1;
        m_sheet_state.clear();
    }
    update_mode();

    // BBS: open/close tab
    for (auto t : std::vector<std::pair<wxPanel*, wxStaticLine*>>({
            {m_tab_print, m_staticline_print},
            {m_tab_print_object, m_staticline_print_object},
            {m_tab_print_part, m_staticline_print_part},
            {m_tab_print_layer, nullptr},
            {m_tab_print_plate, nullptr},
            {m_tab_plate_sheet, nullptr},
            {m_tab_filament, m_staticline_filament},
            {m_tab_printer, m_staticline_printer}})) {
        if (!t.first) continue;
        t.first->Show(tab == t.first);
        if (!t.second) continue;
        t.second->Show(tab == t.first);
        //m_left_sizer->GetItem(t)->SetProportion(tab == t ? 1 : 0);
    }
    m_left_sizer->Layout();
    if (auto dialog = dynamic_cast<wxDialog*>(GetParent())) {
        wxString title = cur_tab->type() == Preset::TYPE_FILAMENT ? _L("Filament settings") : _L("Printer settings");
        dialog->SetTitle(title);
    }
}

bool ParamsPanel::is_active_and_shown_tab(wxPanel* tab)
{
    if (m_current_tab == tab)
        return true;
    else
        return false;
}

void ParamsPanel::update_mode()
{
    int app_mode = Slic3r::GUI::wxGetApp().get_mode();
    SwitchButton * mode_view = m_current_tab ? dynamic_cast<Tab*>(m_current_tab)->m_mode_view : nullptr;
    if (mode_view == nullptr) mode_view = m_mode_view;
    if (mode_view == nullptr) return;

    //BBS: disable the mode tab and return directly when enable develop mode
    if (app_mode == comDevelop)
    {
        mode_view->Disable();
        return;
    }
    if (!mode_view->IsEnabled())
        mode_view->Enable();

    if (app_mode == comAdvanced)
    {
        mode_view->SetValue(true);
    }
    else
    {
        mode_view->SetValue(false);
    }
}

void ParamsPanel::msw_rescale()
{
    if (m_process_icon) m_process_icon->msw_rescale();
    if (m_search_btn) m_search_btn->msw_rescale();
    if (m_compare_btn) m_compare_btn->msw_rescale();
    if (m_tips_arrow) m_tips_arrow->msw_rescale();
    if (m_left_sizer) m_left_sizer->SetMinSize(wxSize(40 * em_unit(this), -1));
    if (m_mode_sizer)
        m_mode_sizer->SetMinSize(-1, 3 * em_unit(this));
    if (m_mode_region)
        ((SwitchButton* )m_mode_region)->Rescale();
    for (Button *btn : {m_global_btn, m_objects_btn})
        if (btn) btn->Rescale();
    if (m_mode_view)
        ((SwitchButton* )m_mode_view)->Rescale();
    for (auto tab : {m_tab_print, m_tab_print_plate, m_tab_print_object, m_tab_print_part, m_tab_print_layer, m_tab_plate_sheet, m_tab_filament, m_tab_printer}) {
        if (tab) dynamic_cast<Tab*>(tab)->msw_rescale();
    }
    if (m_sheet_bar)
        m_sheet_bar->msw_rescale();
    //((Button*)m_export_to_file)->Rescale();
    //((Button*)m_import_from_file)->Rescale();
}

void ParamsPanel::switch_to_global()
{
    m_mode_region->SetValue(false);
    set_active_tab(nullptr);
}

void ParamsPanel::switch_to_object(bool with_tips)
{
    m_mode_region->SetValue(true);
    set_active_tab(nullptr);
    if (with_tips) {
        m_highlighter.init(std::pair(m_tips_arrow, &m_tips_arror_blink), m_top_panel);
        m_highlighter.blink();
    }
}

void ParamsPanel::notify_object_config_changed()
{
    auto & model = wxGetApp().model();
    bool has_config = false;
    for (auto obj : model.objects) {
        if (!obj->config.empty()) {
            SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&obj->config.get(), true);
            if (cat_options.size() > 0) {
                has_config = true;
                break;
            }
        }
        for (auto volume : obj->volumes) {
            if (!volume->config.empty()) {
                SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&volume->config.get(), true);
                if (cat_options.size() > 0) {
                    has_config = true;
                    break;
                }
            }
        }
        if (has_config) break;
    }
    if (has_config == m_has_object_config) return;
    m_has_object_config = has_config;
    if (has_config)
        m_mode_region->SetTextColor2(StateColor(std::pair{0xfffffe, (int) StateColor::Checked}, std::pair{wxGetApp().get_label_clr_modified(), 0}));
    else
        m_mode_region->SetTextColor2(StateColor());
    m_mode_region->Rescale();
    if (m_objects_btn)
        m_objects_btn->SetTextColor(mode_button_text_color(has_config));
}

void ParamsPanel::switch_to_object_if_has_object_configs()
{
    if (m_has_object_config)
        m_mode_region->SetValue(true);
    set_active_tab(nullptr);
}

//Orio: see ParamsPanel.hpp.
void ParamsPanel::show_plate_sheet(int plate_index)
{
    if (m_sheet_bar == nullptr || m_mode_region == nullptr)
        return;
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        m_sheet_current_plate = plater->get_partplate_list().get_curr_plate_index();
    m_active_sheet = plate_index;
    if (m_mode_region->GetValue())
        m_mode_region->SetValue(false);
    update_plate_sheets(true);
    set_active_tab(nullptr);
}

void ParamsPanel::update_plate_sheets(bool force)
{
    Plater *plater = wxGetApp().plater();
    if (m_sheet_bar == nullptr || m_mode_region == nullptr || plater == nullptr)
        return;
    PartPlateList &plates  = plater->get_partplate_list();
    const int      count   = plates.get_plate_count();
    const int      current = plates.get_curr_plate_index();
    auto           sheet_of = [&plates, count](int plate_index) {
        PartPlate *plate = plate_index >= 0 && plate_index < count ? plates.get_plate(plate_index) : nullptr;
        return plate != nullptr && plate->has_sheet() ? plate : nullptr;
    };
    // A new current plate shows its sheet, else the global settings.
    if (current != m_sheet_current_plate) {
        m_sheet_current_plate = current;
        m_active_sheet        = sheet_of(current) != nullptr ? current : -1;
    }
    if (m_active_sheet >= 0 && sheet_of(m_active_sheet) == nullptr)
        m_active_sheet = -1;

    const bool global_mode = !m_mode_region->GetValue();
    std::vector<PlateSheetBar::Item> items{ { -1, _L("Global") } };
    std::string state = std::to_string(current) + ";" + std::to_string(m_active_sheet) + ";" + std::to_string(global_mode) + ";" +
                        wxGetApp().preset_bundle->printers.get_edited_preset().name;
    //Orio: a sheet shared by several plates (same group) shows one tab, labelled with its plates; the others are deduped.
    std::set<int> groups_seen;
    for (int i = 0; i < count; ++ i)
        if (PartPlate *plate = sheet_of(i); plate != nullptr) {
            const int g = plate->sheet_group();
            if (g > 0 && ! groups_seen.insert(g).second)
                continue;
            std::vector<int> members;
            if (g > 0) {
                for (int j = 0; j < count; ++ j)
                    if (PartPlate *q = sheet_of(j); q != nullptr && q->sheet_group() == g)
                        members.push_back(j);
            } else
                members.push_back(i);
            // Orio: label with the name of EACH linked plate (or "Plate N"); the compact "Plates 2, 4" only when none is named.
            bool any_named = false;
            for (int m : members)
                if (PartPlate *q = sheet_of(m); q != nullptr && ! q->get_plate_name().empty()) { any_named = true; break; }
            wxString label;
            if (any_named)
                for (size_t k = 0; k < members.size(); ++ k) {
                    PartPlate *q = sheet_of(members[k]);
                    const std::string mn = q != nullptr ? q->get_plate_name() : std::string();
                    label += (k ? ", " : "") + (mn.empty() ? _L("Plate") + wxString::Format(" %d", members[k] + 1) : from_u8(mn));
                }
            else if (members.size() > 1) {
                label = _L("Plates") + " ";
                for (size_t k = 0; k < members.size(); ++ k)
                    label += (k ? ", " : "") + wxString::Format("%d", members[k] + 1);
            } else
                label = _L("Plate") + wxString::Format(" %d", i + 1);
            items.push_back({ i, label });
            state += ";" + std::to_string(i) + ":" + std::to_string(plate->sheet_revision());
            for (int m : members) {
                PartPlate *q = sheet_of(m);
                state += "," + std::to_string(m) + ":" + (q != nullptr ? q->get_plate_name() : std::string());
            }
        }
    if (!force && state == m_sheet_state)
        return;
    m_sheet_state = state;

    //Orio: highlight the tab of the current plate's group (its first member, the one the tab was created for).
    int active_item = m_active_sheet;
    if (PartPlate *plate = sheet_of(m_active_sheet); plate != nullptr && plate->sheet_group() > 0)
        for (int i = 0; i < count; ++ i)
            if (PartPlate *q = sheet_of(i); q != nullptr && q->sheet_group() == plate->sheet_group()) { active_item = i; break; }
    m_sheet_bar->set_items(std::move(items), active_item);
    const bool show_bar = global_mode && m_sheet_bar->has_sheets();
    if (m_sheet_bar->IsShown() != show_bar) {
        m_sheet_bar->Show(show_bar);
        Layout();
    }
    if (auto *sheet_tab = dynamic_cast<TabPlateSheet*>(m_tab_plate_sheet); sheet_tab != nullptr)
        sheet_tab->load_plate(m_active_sheet);
    // Tab shown, once the panel was activated (OnActivate()).
    if (global_mode && m_current_tab != nullptr) {
        wxPanel *tab = m_active_sheet >= 0 && m_tab_plate_sheet != nullptr ? m_tab_plate_sheet : m_tab_print;
        if (tab != nullptr && m_current_tab != tab)
            dynamic_cast<Tab*>(tab)->restore_last_select_item();
    }
}

void ParamsPanel::free_sizers()
{
    if (m_top_sizer)
    {
        m_top_sizer->Clear(false);
        //m_top_sizer = nullptr;
    }

    m_left_sizer = nullptr;
    //m_right_sizer = nullptr;
    m_mode_sizer = nullptr;
    //m_print_sizer = nullptr;
    //m_filament_sizer = nullptr;
    //m_printer_sizer = nullptr;
    m_button_sizer = nullptr;
}

void ParamsPanel::delete_subwindows()
{
    if (m_title_label)
    {
        delete m_title_label;
        m_title_label = nullptr;
    }

    if (m_mode_region)
    {
        delete m_mode_region;
        m_mode_region = nullptr;
    }

    if (m_mode_view)
    {
        delete m_mode_view;
        m_mode_view = nullptr;
    }

    if (m_title_view)
    {
        delete m_title_view;
        m_title_view = nullptr;
    }

    if (m_search_btn)
    {
        delete m_search_btn;
        m_search_btn = nullptr;
    }

    if (m_staticline_print)
    {
        delete m_staticline_print;
        m_staticline_print = nullptr;
    }

    if (m_staticline_print_part)
    {
        delete m_staticline_print_part;
        m_staticline_print_part = nullptr;
    }

    if (m_staticline_print_object)
    {
        delete m_staticline_print_object;
        m_staticline_print_object = nullptr;
    }

    if (m_staticline_filament)
    {
        delete m_staticline_filament;
        m_staticline_filament = nullptr;
    }

    if (m_staticline_printer)
    {
        delete m_staticline_printer;
        m_staticline_printer = nullptr;
    }

    if (m_export_to_file)
    {
        delete m_export_to_file;
        m_export_to_file = nullptr;
    }

    if (m_import_from_file)
    {
        delete m_import_from_file;
        m_import_from_file = nullptr;
    }

    if (m_page_view)
    {
        delete m_page_view;
        m_page_view = nullptr;
    }
}

ParamsPanel::~ParamsPanel()
{
#if 0
    free_sizers();
    delete m_top_sizer;

    delete_subwindows();
#endif
    // BBS: fix double destruct of OG_CustomCtrl
    Tab* cur_tab = dynamic_cast<Tab*> (m_current_tab);
    if (cur_tab)
        cur_tab->clear_pages();
}

} // GUI
} // Slic3r
