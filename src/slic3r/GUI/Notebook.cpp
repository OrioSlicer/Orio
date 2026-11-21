#include "Notebook.hpp"

//#ifdef _WIN32

#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"

//BBS set font size
#include "Widgets/Label.hpp"

#include <wx/button.h>
#include <wx/sizer.h>

#include <algorithm>

wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, wxCommandEvent);

ButtonsListCtrl::ButtonsListCtrl(wxWindow *parent, wxBoxSizer* side_tools) :
    wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    // Orio : même fond que les onglets, UpdateDarkUI le passe en #202023 en sombre : barre d'une seule couleur
    wxColour default_btn_bg("#3B4446"); // Gradient #414B4E

   
    SetBackgroundColour(default_btn_bg);

    int em = em_unit(this);// Slic3r::GUI::wxGetApp().em_unit();
    // BBS: no gap
    m_btn_margin = 0; // std::lround(0.3 * em);
    m_line_margin = std::lround(0.1 * em);

    m_sizer = new wxBoxSizer(wxHORIZONTAL);
    this->SetSizer(m_sizer);

    // Orio : bouton maison (onglet sans texte) collé au bord gauche, hors centrage
    m_pinned_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_sizer->Add(m_pinned_sizer, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM, m_btn_margin);

    // Orio : espace avant les autres onglets, dimensionné dans Layout() pour les centrer
    m_left_spacer = m_sizer->Add(0, 0);

    m_buttons_sizer = new wxFlexGridSizer(1, m_btn_margin, m_btn_margin);
    m_sizer->Add(m_buttons_sizer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxBOTTOM, m_btn_margin);

    if (side_tools != NULL) {
        m_sizer->AddStretchSpacer(1);
        for (size_t idx = 0; idx < side_tools->GetItemCount(); idx++) {
            wxSizerItem* item = side_tools->GetItem(idx);
            wxWindow* item_win = item->GetWindow();
            if (item_win) {
                item_win->Reparent(this);
            }
        }
        m_sizer->Add(side_tools, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, m_btn_margin);
        m_side_tools = side_tools;
        m_right_spacer = m_sizer->Add(0, 0);
    }

    // BBS: disable custom paint
    //this->Bind(wxEVT_PAINT, &ButtonsListCtrl::OnPaint, this);
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](auto& e){
    });
}

void ButtonsListCtrl::OnPaint(wxPaintEvent&)
{
    //Slic3r::GUI::wxGetApp().UpdateDarkUI(this);
    const wxSize sz = GetSize();
    wxPaintDC dc(this);

    if (m_selection < 0 || m_selection >= (int)m_pageButtons.size())
        return;

    wxColour selected_btn_bg("#1F8EEA");
    wxColour default_btn_bg("#3B4446"); // Gradient #414B4E
    const wxColour& btn_marker_color = Slic3r::GUI::wxGetApp().get_color_hovered_btn_label();

    // highlight selected notebook button

    for (int idx = 0; idx < int(m_pageButtons.size()); idx++) {
        Button* btn = m_pageButtons[idx];

        btn->SetBackgroundColor(idx == m_selection ? selected_btn_bg : default_btn_bg);

        wxPoint pos = btn->GetPosition();
        wxSize size = btn->GetSize();
        const wxColour& clr = idx == m_selection ? btn_marker_color : default_btn_bg;
        dc.SetPen(clr);
        dc.SetBrush(clr);
        dc.DrawRectangle(pos.x, pos.y + size.y, size.x, sz.y - size.y);
    }

#if 0
    // highlight selected mode button
    if (m_mode_sizer) {
        const std::vector<ModeButton*>& mode_btns = m_mode_sizer->get_btns();
        for (int idx = 0; idx < int(mode_btns.size()); idx++) {
            ModeButton* btn = mode_btns[idx];
            btn->SetBackgroundColor(btn->is_selected() ? selected_btn_bg : default_btn_bg);

            //wxPoint pos = btn->GetPosition();
            //wxSize size = btn->GetSize();
            //const wxColour& clr = btn->is_selected() ? btn_marker_color : default_btn_bg;
            //dc.SetPen(clr);
            //dc.SetBrush(clr);
            //dc.DrawRectangle(pos.x, pos.y + size.y, size.x, sz.y - size.y);
        }
    }
#endif

    // Draw orange bottom line

    dc.SetPen(btn_marker_color);
    dc.SetBrush(btn_marker_color);
    dc.DrawRectangle(1, sz.y - m_line_margin, sz.x, m_line_margin);
}

void ButtonsListCtrl::UpdateMode()
{
    //m_mode_sizer->SetMode(Slic3r::GUI::wxGetApp().get_mode());
}

void ButtonsListCtrl::Rescale()
{
    //m_mode_sizer->msw_rescale();
    int em = em_unit(this);
    for (Button* btn : m_pageButtons) {
        //BBS
        btn->SetMinSize({(btn->GetLabel().empty() ? 40 : 132) * em / 10, 36 * em / 10});
        btn->Rescale();
    }

    // BBS: no gap
    //m_btn_margin = std::lround(0.3 * em);
    //m_line_margin = std::lround(0.1 * em);
    //m_buttons_sizer->SetVGap(m_btn_margin);
    //m_buttons_sizer->SetHGap(m_btn_margin);

    Layout();
}

void ButtonsListCtrl::SetSelection(int sel)
{
    if (m_selection == sel)
        return;
    // BBS: change button color
    wxColour selected_btn_bg("#0874E0");    // Gradient #0874E0
    if (m_selection >= 0) {
        StateColor bg_color = StateColor(
        std::pair{wxColour(107, 107, 107), (int) StateColor::Hovered},
        std::pair{wxColour(59, 68, 70), (int) StateColor::Normal});
        m_pageButtons[m_selection]->SetBackgroundColor(bg_color);
        StateColor text_color = StateColor(
        std::pair{wxColour(254,254, 254), (int) StateColor::Normal}
        );
        m_pageButtons[m_selection]->SetSelected(false);
        m_pageButtons[m_selection]->SetTextColor(text_color);
    }
    m_selection = sel;

    StateColor bg_color = StateColor(
        std::pair{wxColour(8, 116, 224), (int) StateColor::Hovered},
        std::pair{wxColour(8, 119, 232), (int) StateColor::Normal});
    m_pageButtons[m_selection]->SetBackgroundColor(bg_color);

    StateColor text_color = StateColor(
        std::pair{wxColour(254, 254, 254), (int) StateColor::Normal}
        );
    m_pageButtons[m_selection]->SetSelected(true);
    m_pageButtons[m_selection]->SetTextColor(text_color);
    
    Refresh();
}

bool ButtonsListCtrl::InsertPage(size_t n, const wxString &text, bool bSelect /* = false*/, const std::string &bmp_name /* = ""*/, const std::string &inactive_bmp_name)
{
    Button * btn = new Button(this, text.empty() ? text : " " + text, bmp_name, wxNO_BORDER);
    btn->SetCornerRadius(0);

    int em = em_unit(this);
    //BBS set size for button
    btn->SetMinSize({(text.empty() ? 40 : 136) * em / 10, 36 * em / 10});

    StateColor bg_color = StateColor(
        std::pair{wxColour(107, 107, 107), (int) StateColor::Hovered},
        std::pair{wxColour(59, 68, 70), (int) StateColor::Normal});

    btn->SetBackgroundColor(bg_color);
    StateColor text_color = StateColor(
        std::pair{wxColour(254,254, 254), (int) StateColor::Normal});
    btn->SetTextColor(text_color);
    btn->SetInactiveIcon(inactive_bmp_name);
    btn->SetSelected(false);
    btn->Bind(wxEVT_BUTTON, [this, btn](wxCommandEvent& event) {
        if (auto it = std::find(m_pageButtons.begin(), m_pageButtons.end(), btn); it != m_pageButtons.end()) {
            auto sel = it - m_pageButtons.begin();
            //do it later
            //SetSelection(sel);
            
            wxCommandEvent evt = wxCommandEvent(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED);
            evt.SetId(sel);
            wxPostEvent(this->GetParent(), evt);
        }
    });
    Slic3r::GUI::wxGetApp().UpdateDarkUI(btn);
    if (text.empty()) {
        // Orio : l'onglet maison (icône seule) reste collé à gauche
        m_pinned_sizer->Add(btn);
    } else {
        // rang parmi les onglets centrés : ceux qui le précèdent, hors bouton maison
        size_t pos = 0;
        for (size_t i = 0; i < n && i < m_pageButtons.size(); ++i)
            if (m_pinned_sizer->GetItem(m_pageButtons[i]) == nullptr)
                ++pos;
        m_buttons_sizer->Insert(pos, new wxSizerItem(btn));
        m_buttons_sizer->SetCols(m_buttons_sizer->GetCols() + 1);
    }
    m_pageButtons.insert(m_pageButtons.begin() + n, btn);
    Layout();
    return true;
}

void ButtonsListCtrl::RemovePage(size_t n)
{
    Button* btn = m_pageButtons[n];
    m_pageButtons.erase(m_pageButtons.begin() + n);
    // Orio : détaché par pointeur, le bouton maison n'étant pas dans m_buttons_sizer
    if (!m_pinned_sizer->Detach(btn))
        m_buttons_sizer->Detach(btn);
#if __WXOSX__
    RemoveChild(btn);
#else
    btn->Reparent(nullptr);
#endif
    btn->Destroy();
    Layout();
}

void ButtonsListCtrl::ShowPage(size_t n, bool show)
{
    if (n >= m_pageButtons.size() || m_pageButtons[n]->IsShown() == show)
        return;
    // un bouton masqué ne compte plus dans m_buttons_sizer : les onglets restants se resserrent
    m_pageButtons[n]->Show(show);
    Layout();
}

// Orio : bouton maison au bord gauche, autres onglets centrés sur toute la largeur de
// la barre. Faute de place (outils de droite : Trancher, Imprimer), ils glissent vers la gauche
// sans jamais passer dessous, et au pire se collent au bouton maison comme avant.
bool ButtonsListCtrl::Layout()
{
    if (m_left_spacer != nullptr && m_buttons_sizer != nullptr && m_pinned_sizer != nullptr) {
        const int width   = GetClientSize().GetWidth();
        const int pinned  = m_pinned_sizer->CalcMin().GetWidth();
        const int buttons = m_buttons_sizer->CalcMin().GetWidth();
        const int tools   = m_side_tools != nullptr ? m_side_tools->GetMinSize().GetWidth() : 0;
        // bord gauche voulu des outils : tout à droite, ou m_tools_anchor_win centrée à m_tools_anchor_right du bord
        int tools_left    = width - tools;
        if (m_right_spacer != nullptr && m_tools_anchor_win != nullptr) {
            // milieu de m_tools_anchor_win depuis le bord gauche des outils
            int centre = 0;
            for (size_t idx = 0; idx < m_side_tools->GetItemCount(); idx++) {
                wxSizerItem* item = m_side_tools->GetItem(idx);
                if (item->GetWindow() == m_tools_anchor_win) {
                    centre += item->GetMinSize().GetWidth() / 2;
                    break;
                }
                centre += item->GetMinSizeWithBorder().GetWidth();
            }
            tools_left = std::min(tools_left, width - m_tools_anchor_right - centre);
        }
        // l'espace suit le bouton maison : bord gauche voulu des onglets moins sa largeur
        const int left    = std::max(0, std::min((width - buttons) / 2, tools_left - buttons) - pinned);
        m_left_spacer->SetMinSize(left, 0);
        // les outils ne passent jamais sur les onglets
        if (m_right_spacer != nullptr)
            m_right_spacer->SetMinSize(std::max(0, width - tools - std::max(tools_left, pinned + left + buttons)), 0);
    }
    return wxControl::Layout();
}

void ButtonsListCtrl::SetSideToolsAnchor(int right, wxWindow* win)
{
    if (right == m_tools_anchor_right && win == m_tools_anchor_win)
        return;
    m_tools_anchor_right = right;
    m_tools_anchor_win   = win;
    // appelé aussi pendant le rendu de la scène 3D : la barre se remet en place juste après
    CallAfter([this]() { Layout(); });
}

bool ButtonsListCtrl::SetPageImage(size_t n, const std::string& bmp_name) const
{
    if (n >= m_pageButtons.size())
        return false;
     
    // BBS
    //return m_pageButtons[n]->SetBitmap_(bmp_name);
    ScalableBitmap bitmap(NULL, bmp_name);
    //m_pageButtons[n]->SetBitmap_(bitmap);
    return true;
}

void ButtonsListCtrl::SetPageText(size_t n, const wxString& strText)
{
    Button* btn = m_pageButtons[n];
    btn->SetLabel(strText);
}

void ButtonsListCtrl::SetPageBadge(size_t n, const wxString& badge)
{
    if (n >= m_pageButtons.size())
        return;
    m_pageButtons[n]->SetBadge(badge);
    // le bouton s'élargit : onglets recentrés
    Layout();
}

wxString ButtonsListCtrl::GetPageText(size_t n) const
{
    Button* btn = m_pageButtons[n];
    return btn->GetLabel();
}

//#endif // _WIN32

void Notebook::Init()
{
    // We don't need any border as we don't have anything to separate the
    // page contents from.
    SetInternalBorder(0);

    // No effects by default.
    m_showEffect = m_hideEffect = wxSHOW_EFFECT_NONE;

    m_showTimeout = m_hideTimeout = 0;

    /* On Linux, Gstreamer wxMediaCtrl does not seem to get along well with
     * 32-bit X11 visuals (the overlay does not work).  Is this a wxWindows
     * bug?  Is this a Gstreamer bug?  No idea, but it is our problem ... 
     * and anyway, this transparency thing just isn't all that interesting,
     * so we just don't do it on Linux. 
     */
#ifndef __WXGTK__
    SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
#endif
}
