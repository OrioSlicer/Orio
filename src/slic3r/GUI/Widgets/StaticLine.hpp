#ifndef slic3r_GUI_StaticLine_hpp_
#define slic3r_GUI_StaticLine_hpp_

#include "../wxExtensions.hpp"
#include "wx/window.h"

#include <functional>

class StaticLine : public wxWindow
{
public:
    StaticLine(wxWindow *parent, bool vertical = false, const wxString &label = {}, const wxString &icon = {});

public:
    void SetLabel(const wxString& label) override;

    void SetIcon(const wxString& icon);

    void SetLineColour(wxColour color);
    
    void Rescale();

    // Orio: the title of a settings section that folds: a chevron at the right end (up: open, down: folded); a click
    // on the title calls on_click (given once, nullptr afterwards to change only the chevron).
    void SetCollapse(bool collapsed, std::function<void()> on_click = nullptr);

private:
    wxColour       lineColor;
    bool vertical;
    ScalableBitmap icon;
    ScalableBitmap chevron;
    std::function<void()> on_collapse_click;

private:
    void paintEvent(wxPaintEvent& evt);

    void messureSize();

    void render(wxDC &dc);

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_StaticLine_hpp_
