#include "StaticLine.hpp"
#include "Label.hpp"
#include "StateColor.hpp"

#include <wx/dcclient.h>
#include <wx/dcgraph.h>

BEGIN_EVENT_TABLE(StaticLine, wxWindow)

// catch paint events
EVT_PAINT(StaticLine::paintEvent)

END_EVENT_TABLE()

StaticLine::StaticLine(wxWindow *parent, bool vertical, const wxString &label, const wxString &icon)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , vertical(vertical)
{
    wxWindow::SetBackgroundColour(parent->GetBackgroundColour());
    this->lineColor = wxColour("#EEEEEE");
    DisableFocusFromKeyboard();
    SetFont(Label::Body_14);
    wxWindow::SetLabel(label);
    SetIcon(icon);
}

void StaticLine::SetLabel(const wxString& label)
{
    wxWindow::SetLabel(label);
    messureSize();
    Refresh();
}

void StaticLine::SetIcon(const wxString &icon)
{
    this->icon = icon.IsEmpty() ? ScalableBitmap()
        : ScalableBitmap(this, icon.ToStdString(), 18);
    messureSize();
    Refresh();
}

void StaticLine::SetLineColour(wxColour color)
{
    this->lineColor = color;
}

void StaticLine::Rescale()
{
    if (this->icon.bmp().IsOk())
        this->icon.msw_rescale();
    if (this->chevron.bmp().IsOk())
        this->chevron.msw_rescale();
    messureSize();
}

void StaticLine::SetCollapse(bool collapsed, std::function<void()> on_click)
{
    if (on_click) {
        if (!this->on_collapse_click) {
            SetCursor(wxCursor(wxCURSOR_HAND));
            // The second click of a quick double click comes as a double click.
            auto toggle = [this](wxMouseEvent &) { if (this->on_collapse_click) this->on_collapse_click(); };
            Bind(wxEVT_LEFT_DOWN, toggle);
            Bind(wxEVT_LEFT_DCLICK, toggle);
        }
        this->on_collapse_click = on_click;
    }
    // The 24 px of the chevron of the sheet header (Tab::m_sheet_collapse_btn).
    this->chevron = ScalableBitmap(this, collapsed ? "sheet_collapse_down" : "sheet_collapse_up", 24);
    messureSize();
    Refresh();
}

void StaticLine::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
    render(dc);
}

void StaticLine::messureSize()
{
    wxClientDC dc(this);
    wxSize textSize = dc.GetTextExtent(GetLabel());
    wxSize szContent = textSize;
    if (this->icon.bmp().IsOk()) {
        if (szContent.y > 0) {
            // BBS norrow size between text and icon
            szContent.x += 5;
        }
        wxSize szIcon = this->icon.GetBmpSize();
        szContent.x += szIcon.x;
        if (szIcon.y > szContent.y) szContent.y = szIcon.y;
    }
    // Orio: the chevron does not make the line higher: it is a thin stroke in the middle of its bitmap.
    if (this->chevron.bmp().IsOk())
        szContent.x += 5 + this->chevron.GetBmpWidth();
    if (vertical)
        szContent = {std::max(szContent.x, 1), szContent.y + 10};
    else
        szContent = { szContent.x + 10, std::max(szContent.y, 1)};
    SetMinSize(szContent);
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void StaticLine::render(wxDC& dc)
{
    wxSize size = GetSize();
    wxSize textSize;
    auto   label = GetLabel();
    if (!label.IsEmpty()) textSize = dc.GetTextExtent(label);
    wxRect titleRect{{0, 0}, size};
    titleRect.height = wxMax(icon.GetBmpHeight(), textSize.GetHeight());
    int contentWidth = icon.GetBmpWidth() + ((icon.bmp().IsOk() && textSize.GetWidth() > 0) ? 5 : 0) +
                textSize.GetWidth();
    if (vertical) titleRect.Deflate((size.GetWidth() - contentWidth) / 2, 0);
    // Orio: the chevron of a section that folds, at the right end; the line stops before it.
    int line_end = size.x;
    if (chevron.bmp().IsOk()) {
        line_end -= chevron.GetBmpWidth();
        dc.DrawBitmap(chevron.bmp(), {line_end, (size.y - chevron.GetBmpHeight()) / 2});
        line_end -= 5;
    }
    if (icon.bmp().IsOk()) {
        dc.DrawBitmap(icon.bmp(), {0, (size.y - icon.GetBmpHeight()) / 2});
        titleRect.x += icon.GetBmpWidth() + 5;
    }
    if (!label.IsEmpty()) {
        dc.SetTextForeground(StateColor::darkModeColorFor(GetForegroundColour()));
        dc.DrawText(label, titleRect.x, (size.GetHeight() - textSize.GetHeight()) / 2);
        titleRect.x += textSize.GetWidth() + 5;
    }
    dc.SetPen(wxPen(StateColor::darkModeColorFor(lineColor)));
    if (vertical) {
        size.x /= 2;
        if (titleRect.y > 0) titleRect.y += 5;
        dc.DrawLine(size.x, titleRect.y, size.x, size.y);
    } else {
        size.y /= 2;
        dc.DrawLine(titleRect.x, size.y, line_end, size.y);
    }
}
