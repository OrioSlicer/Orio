#include "GCodeSketchPanel.hpp"
#include "GCodeConsoleDialog.hpp"
#include "GCodeSketchDialog.hpp"
#include "GCodeSketchView.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"

#include "libslic3r/Emboss.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/fontenum.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace Slic3r {
namespace GUI {

namespace {

constexpr size_t MAX_UNDO       = 100;
// Large drawings (imported G-code) keep fewer undo steps.
constexpr size_t MAX_UNDO_LARGE = 5;
constexpr size_t LARGE_DRAWING  = 20000;
// Beyond this number of segments, the other layers aren't shown in the view.
constexpr size_t MAX_STACKED    = 200000;
// Steps of the grid in mm, in the order of the choices, 0 for none.
constexpr double GRID_STEPS[]   = { 0., 0.1, 0.5, 1., 5., 10. };
constexpr int    DEFAULT_GRID   = 3;
// Beyond this size, the G-code isn't written (as GCodeSketchDialog).
constexpr double MAX_GCODE_MB   = 64.;
// mm between the curves (ellipses, splines, conics) and the arcs which print them.
constexpr double CURVE_TOLERANCE = 0.01;
// Sides of a polygon at most.
constexpr int    MAX_SIDES       = 64;
// Copies of a pattern along each direction at most, and segments the copies of a tool add at most.
constexpr int    MAX_COPIES        = 1000;
constexpr size_t MAX_COPY_SEGMENTS = 200000;
// Units of the font in a unit of the points of a glyph (SHAPE_SCALE of Emboss.cpp).
constexpr double GLYPH_UNIT        = 0.001;
// Outlines of a text at most.
constexpr size_t MAX_TEXT_OUTLINES = 20000;
// Beyond this number of segments, the view picks nothing under the mouse (MAX_DETAILED of GCodeSketchView).
constexpr size_t MAX_PICKED        = 20000;

// Value of a numeric option, the first one of a vector.
double config_float(const DynamicPrintConfig &config, const std::string &key, double fallback)
{
    const ConfigOption *option = config.option(key);
    if (option == nullptr)
        return fallback;
    if (const auto *value = dynamic_cast<const ConfigOptionFloat*>(option))
        return value->value;
    if (const auto *values = dynamic_cast<const ConfigOptionFloats*>(option))
        return values->values.empty() ? fallback : values->values.front();
    if (const auto *values = dynamic_cast<const ConfigOptionFloatsNullable*>(option))
        return (values->values.empty() || values->is_nil(0)) ? fallback : values->values.front();
    if (const auto *value = dynamic_cast<const ConfigOptionInt*>(option))
        return double(value->value);
    if (const auto *values = dynamic_cast<const ConfigOptionInts*>(option))
        return values->values.empty() ? fallback : double(values->values.front());
    if (const auto *values = dynamic_cast<const ConfigOptionIntsNullable*>(option))
        return (values->values.empty() || values->is_nil(0)) ? fallback : double(values->values.front());
    return fallback;
}

wxString file_wildcard()
{
    return _L("G-code files") + " (*.gcode;*.gco;*.g)|*.gcode;*.gco;*.g|" + _L("All files") + " (*.*)|*.*";
}

wxString issue_text(const GCodeSketch::Issue &issue)
{
    wxString problem;
    switch (issue.type) {
    case GCodeSketch::IssueType::OutsideBed: problem = _L("outside of the bed"); break;
    case GCodeSketch::IssueType::InExcludeArea: problem = _L("in the excluded area of the bed"); break;
    case GCodeSketch::IssueType::BelowBed: problem = _L("below the bed"); break;
    case GCodeSketch::IssueType::AboveMaxHeight: problem = _L("above the printable height"); break;
    case GCodeSketch::IssueType::AlignedArc: problem = _L("arc through aligned points, printed as a line"); break;
    }
    return wxString::Format(_L("Point %d: %s"), int(issue.point + 1), problem);
}

// Box of the bed, in the coordinates of the G-code.
void bed_box(const GCodeSketch::Machine &machine, Vec2d &min, Vec2d &max)
{
    min = Vec2d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    max = -min;
    for (const Vec2d &p : machine.bed_shape) {
        min = min.cwiseMin(p);
        max = max.cwiseMax(p);
    }
    if (machine.bed_shape.empty() || min.x() >= max.x() || min.y() >= max.y()) {
        min = Vec2d::Zero();
        max = Vec2d(256., 256.);
    }
}

wxColour to_colour(const std::array<float, 3> &rgb, unsigned char alpha = 255)
{
    return wxColour((unsigned char) std::lround(rgb[0] * 255.f), (unsigned char) std::lround(rgb[1] * 255.f),
                    (unsigned char) std::lround(rgb[2] * 255.f), alpha);
}

wxString format_point(const Vec3d &p)
{
    return wxString::Format("X %.2f   Y %.2f   Z %.2f", p.x(), p.y(), p.z());
}

// Text tool: its text, its font and its size, asked before it is drawn.
class TextDialog : public DPIDialog
{
public:
    TextDialog(wxWindow *parent, const wxString &text, const wxString &font, bool bold, bool italic, double height, double angle,
               double spacing)
        : DPIDialog(parent, wxID_ANY, _L("Text"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        SetBackgroundColour(*wxWHITE);
        const int em    = em_unit();
        auto     *sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, _L("Text (Enter: a new line)")), 0, wxLEFT | wxRIGHT | wxTOP, em);
        m_text = new wxTextCtrl(this, wxID_ANY, text, wxDefaultPosition, wxSize(40 * em, 6 * em), wxTE_MULTILINE);
        sizer->Add(m_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

        auto *grid = new wxFlexGridSizer(2, em / 2, em);
        grid->AddGrowableCol(1);
        auto row = [&](const wxString &label, wxWindow *control) {
            grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(control, 0, wxEXPAND);
        };
        // The fonts of the system, sorted, without the vertical ones (@).
        wxArrayString faces = wxFontEnumerator::GetFacenames(wxFONTENCODING_SYSTEM);
        faces.Sort();
        m_face = new wxChoice(this, wxID_ANY);
        for (const wxString &face : faces)
            if (!face.StartsWith("@"))
                m_face->Append(face);
        const wxString wanted    = font.empty() ? wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).GetFaceName() : font;
        int            selection = m_face->FindString(wanted);
        if (selection == wxNOT_FOUND) {
            m_face->Append(wanted);
            selection = int(m_face->GetCount()) - 1;
        }
        m_face->SetSelection(selection);
        row(_L("Font"), m_face);
        auto *style = new wxBoxSizer(wxHORIZONTAL);
        m_bold      = new wxCheckBox(this, wxID_ANY, _L("Bold"));
        m_italic    = new wxCheckBox(this, wxID_ANY, _L("Italic"));
        m_bold->SetValue(bold);
        m_italic->SetValue(italic);
        style->Add(m_bold, 0, wxRIGHT, em);
        style->Add(m_italic, 0);
        grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
        grid->Add(style, 0);
        auto spin = [&](double min, double max, double increment, unsigned int digits, double value) {
            auto *control = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(10 * em, -1), wxSP_ARROW_KEYS, min, max,
                                                 value, increment);
            control->SetDigits(digits);
            return control;
        };
        m_height  = spin(0.5, 500., 1., 2, height);
        m_angle   = spin(-360., 360., 5., 1, angle);
        m_spacing = spin(-20., 50., 0.1, 2, spacing);
        row(_L("Height of the capitals (mm)"), m_height);
        row(_L("Angle (degrees)"), m_angle);
        row(_L("Space added between the letters (mm)"), m_spacing);
        sizer->Add(grid, 0, wxEXPAND | wxALL, em);
        sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);
        SetSizer(sizer);
        Fit();
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
        m_text->SetFocus();
        m_text->SelectAll();
    }

    wxString text() const { return m_text->GetValue(); }
    wxString font() const { return m_face->GetStringSelection(); }
    bool     bold() const { return m_bold->GetValue(); }
    bool     italic() const { return m_italic->GetValue(); }
    double   height() const { return m_height->GetValue(); }
    double   angle() const { return m_angle->GetValue(); }
    double   spacing() const { return m_spacing->GetValue(); }

protected:
    void on_dpi_changed(const wxRect &) override
    {
        Fit();
        Refresh();
    }

private:
    wxTextCtrl       *m_text    { nullptr };
    wxChoice         *m_face    { nullptr };
    wxCheckBox       *m_bold    { nullptr };
    wxCheckBox       *m_italic  { nullptr };
    wxSpinCtrlDouble *m_height  { nullptr };
    wxSpinCtrlDouble *m_angle   { nullptr };
    wxSpinCtrlDouble *m_spacing { nullptr };
};

} // namespace

std::array<float, 3> sketch_plane_rgb(SketchPlane plane)
{
    switch (plane) {
    case SketchPlane::Front: return { 0.10f, 0.65f, 0.60f };
    case SketchPlane::Side: return { 0.85f, 0.35f, 0.65f };
    default: return { 0.50f, 0.47f, 0.87f };
    }
}

wxString sketch_tool_name(SketchTool tool)
{
    switch (tool) {
    case SketchTool::Select: return _L("Select (V)");
    case SketchTool::Line: return _L("Line (L)");
    case SketchTool::Arc: return _L("3-point arc (A)");
    case SketchTool::Rectangle: return _L("2-point rectangle (R)");
    case SketchTool::Circle: return _L("Center diameter circle (C)");
    case SketchTool::Rectangle3Points: return _L("3-point rectangle");
    case SketchTool::RectangleCenter: return _L("Center rectangle");
    case SketchTool::Circle2Points: return _L("2-point circle");
    case SketchTool::Circle3Points: return _L("3-point circle");
    case SketchTool::CircleTangent2: return _L("2-tangent circle");
    case SketchTool::CircleTangent3: return _L("3-tangent circle");
    case SketchTool::ArcCenter: return _L("Center point arc");
    case SketchTool::ArcTangent: return _L("Tangent arc");
    case SketchTool::PolygonCircumscribed: return _L("Circumscribed polygon");
    case SketchTool::PolygonInscribed: return _L("Inscribed polygon");
    case SketchTool::PolygonEdge: return _L("Edge polygon");
    case SketchTool::Ellipse: return _L("Ellipse");
    case SketchTool::SlotCenters: return _L("Center to center slot");
    case SketchTool::SlotOverall: return _L("Overall slot");
    case SketchTool::SlotCenter: return _L("Center point slot");
    case SketchTool::SlotArc3Points: return _L("Three point arc slot");
    case SketchTool::SlotArcCenter: return _L("Center point arc slot");
    case SketchTool::Spline: return _L("Fit point spline");
    case SketchTool::Conic: return _L("Conic curve");
    case SketchTool::Point: return _L("Point");
    case SketchTool::Text: return _L("Text");
    case SketchTool::Mirror: return _L("Mirror");
    case SketchTool::CircularPattern: return _L("Circular pattern");
    case SketchTool::RectangularPattern: return _L("Rectangular pattern");
    case SketchTool::Fillet: return _L("Fillet");
    case SketchTool::ChamferEqual: return _L("Equal distance chamfer");
    case SketchTool::ChamferDistanceAngle: return _L("Distance and angle chamfer");
    case SketchTool::ChamferTwoDistances: return _L("Two distance chamfer");
    case SketchTool::Offset: return _L("Offset (O)");
    case SketchTool::Trim: return _L("Trim (T)");
    case SketchTool::Extend: return _L("Extend");
    case SketchTool::Break: return _L("Break");
    case SketchTool::Scale: return _L("Sketch scale");
    case SketchTool::Move: return _L("Move (M)");
    case SketchTool::Copy: return _L("Copy");
    case SketchTool::Rotate: return _L("Rotate");
    }
    return wxString();
}

// Toolbox on the left of the view, as the sketch toolbar of Fusion 360: the tools in two columns, each kind of tool
// (rectangles, circles, arcs, polygons, slots, patterns, chamfers...) in one button showing the last one used, the others
// in its menu (its small triangle, or a right click); then the extrusion and the working planes. Each button has its icon
// and its name. Taller than the view, it scrolls with the wheel.
class GCodeSketchToolbox : public wxPanel
{
public:
    enum class Icon {
        Select,
        Line,
        Arc,
        ArcCenter,
        ArcTangent,
        Rectangle,
        Rectangle3Points,
        RectangleCenter,
        Circle,
        Circle2Points,
        Circle3Points,
        CircleTangent2,
        CircleTangent3,
        PolygonCircumscribed,
        PolygonInscribed,
        PolygonEdge,
        Ellipse,
        SlotCenters,
        SlotOverall,
        SlotCenter,
        SlotArc3Points,
        SlotArcCenter,
        Spline,
        Conic,
        Point,
        Text,
        Mirror,
        CircularPattern,
        RectangularPattern,
        Fillet,
        ChamferEqual,
        ChamferDistanceAngle,
        ChamferTwoDistances,
        Offset,
        Trim,
        Extend,
        Break,
        Scale,
        Move,
        Copy,
        Rotate,
        Extrude,
        Horizontal,
        Front,
        Side,
        Frame
    };
    // A tool of a button.
    struct Variant
    {
        SketchTool tool;
        Icon       icon;
        // Name under the icon: the kind of the tool.
        wxString   label;
        wxString   tooltip;
    };

    GCodeSketchToolbox(wxWindow *parent, GCodeSketchPanel &panel)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER)
        , m_panel(panel)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(WIDTH), FromDIP(2 * MARGIN + ITEM_H)));
        Bind(wxEVT_PAINT, &GCodeSketchToolbox::on_paint, this);
        Bind(wxEVT_MOTION, [this](wxMouseEvent &evt) { set_hover(item_at(evt.GetPosition())); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &) { set_hover(-1); });
        // The second click of a double click comes as a double click.
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &evt) { on_click(evt.GetPosition(), false); });
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent &evt) { on_click(evt.GetPosition(), false); });
        Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent &evt) { on_click(evt.GetPosition(), true); });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent &evt) {
            if (evt.GetWheelRotation() != 0)
                scroll_by(evt.GetWheelRotation() > 0 ? -1 : 1);
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
            scroll_by(0);
            evt.Skip();
        });
    }

    // A button of tools of one kind: the first one, then the last one used; its menu lists them.
    void add_tools(std::vector<Variant> variants)
    {
        Item item;
        item.variants = std::move(variants);
        place(std::move(item));
    }
    // A button running a command.
    void add_command(Icon icon, std::function<wxString()> label, std::function<wxString()> tooltip, std::function<bool()> active,
                     std::function<void()> action)
    {
        Item item;
        item.icon    = icon;
        item.label   = std::move(label);
        item.tooltip = std::move(tooltip);
        item.active  = std::move(active);
        item.action  = std::move(action);
        place(std::move(item));
    }
    // A title over the next buttons, which start a new row.
    void add_section(const wxString &title)
    {
        if (m_column != 0) {
            m_column = 0;
            m_next_y += ITEM_H + SPACE;
        }
        m_next_y += GAP;
        m_sections.push_back({ title, wxRect(MARGIN, m_next_y, WIDTH - 2 * MARGIN, TITLE_H) });
        m_next_y += TITLE_H;
    }

private:
    // In DIP.
    static constexpr int ITEM_W  = 49;
    static constexpr int ITEM_H  = 46;
    static constexpr int SPACE   = 3;
    static constexpr int MARGIN  = 5;
    static constexpr int WIDTH   = 2 * MARGIN + 2 * ITEM_W + SPACE;
    static constexpr int ICON    = 22;
    static constexpr int GAP     = 6;
    static constexpr int TITLE_H = 16;
    // Corner of the buttons with a menu, where their small triangle is.
    static constexpr int FLYOUT  = 14;

    struct Item
    {
        // Tools of one kind; none for a command.
        std::vector<Variant>      variants;
        size_t                    current { 0 };
        Icon                      icon { Icon::Select };
        std::function<wxString()> label;
        std::function<wxString()> tooltip;
        std::function<bool()>     active;
        std::function<void()>     action;
        // In DIP.
        wxRect                    rect;
    };
    struct Section
    {
        wxString title;
        // In DIP.
        wxRect   rect;
    };

    // Next place of the grid: two buttons a row.
    void place(Item &&item)
    {
        item.rect = wxRect(MARGIN + m_column * (ITEM_W + SPACE), m_next_y, ITEM_W, ITEM_H);
        // Its height: the view may be shorter, the toolbox then scrolls.
        m_bottom  = std::max(m_bottom, item.rect.GetBottom() + 1 + MARGIN);
        m_items.push_back(std::move(item));
        if (++ m_column == 2) {
            m_column = 0;
            m_next_y += ITEM_H + SPACE;
        }
    }

    // In pixels, scrolled.
    wxRect scaled(const wxRect &rect) const
    {
        return wxRect(FromDIP(rect.x), FromDIP(rect.y) - m_scroll, FromDIP(rect.width), FromDIP(rect.height));
    }

    // Scrolled by rows of buttons when it is taller than the view; 0: kept in its range after a resize.
    void scroll_by(int rows)
    {
        const int room   = GetClientSize().GetHeight();
        const int most   = std::max(0, FromDIP(m_bottom) - room);
        const int scroll = std::clamp(m_scroll + rows * FromDIP(ITEM_H + SPACE), 0, most);
        if (scroll == m_scroll)
            return;
        m_scroll = scroll;
        set_hover(-1);
        Refresh();
    }

    wxRect flyout(const wxRect &rect) const
    {
        const int size = FromDIP(FLYOUT);
        return wxRect(rect.GetRight() + 1 - size, rect.GetBottom() + 1 - size, size, size);
    }

    int item_at(const wxPoint &position) const
    {
        for (int i = 0; i < int(m_items.size()); ++ i)
            if (scaled(m_items[size_t(i)].rect).Contains(position))
                return i;
        return -1;
    }

    // The tool in use shows on the button of its kind, whoever chose it (shortcut, menu of the view).
    void sync_current()
    {
        const SketchTool tool = m_panel.tool();
        for (Item &item : m_items)
            for (size_t i = 0; i < item.variants.size(); ++ i)
                if (item.variants[i].tool == tool)
                    item.current = i;
    }

    bool is_active(const Item &item) const
    {
        if (!item.variants.empty())
            return item.variants[std::min(item.current, item.variants.size() - 1)].tool == m_panel.tool();
        return item.active && item.active();
    }

    Icon icon_of(const Item &item) const
    {
        return item.variants.empty() ? item.icon : item.variants[std::min(item.current, item.variants.size() - 1)].icon;
    }

    wxString label_of(const Item &item) const
    {
        if (item.variants.empty())
            return item.label ? item.label() : wxString();
        return item.variants[std::min(item.current, item.variants.size() - 1)].label;
    }

    wxString tooltip_of(const Item &item) const
    {
        if (item.variants.empty())
            return item.tooltip ? item.tooltip() : wxString();
        wxString text = item.variants[std::min(item.current, item.variants.size() - 1)].tooltip;
        if (item.variants.size() > 1)
            text += "\n" + _L("Small triangle or right click: the other tools of this kind.");
        return text;
    }

    void set_hover(int index)
    {
        if (index == m_hover)
            return;
        m_hover = index;
        sync_current();
        SetToolTip(index >= 0 ? tooltip_of(m_items[size_t(index)]) : wxString());
        Refresh();
    }

    void on_click(const wxPoint &position, bool menu)
    {
        sync_current();
        const int index = item_at(position);
        if (index >= 0) {
            const Item  &item = m_items[size_t(index)];
            const wxRect rect = scaled(item.rect);
            if (item.variants.size() > 1 && (menu || flyout(rect).Contains(position)))
                show_variants(size_t(index), rect);
            else if (!item.variants.empty() && !menu)
                m_panel.set_tool(item.variants[std::min(item.current, item.variants.size() - 1)].tool);
            else if (item.action && !menu)
                item.action();
        }
        Refresh();
        // The shortcuts and the typed values keep going to the view.
        m_panel.focus_view();
    }

    // Menu of the tools of a button, with their icons.
    void show_variants(size_t index, const wxRect &rect)
    {
        wxMenu menu;
        for (size_t i = 0; i < m_items[index].variants.size(); ++ i) {
            const Variant &variant = m_items[index].variants[i];
            auto          *entry   = new wxMenuItem(&menu, wxID_ANY, sketch_tool_name(variant.tool));
            entry->SetBitmap(icon_bitmap(variant.icon));
            menu.Append(entry);
            const SketchTool tool = variant.tool;
            menu.Bind(
                wxEVT_MENU,
                [this, index, i, tool](wxCommandEvent &) {
                    m_items[index].current = i;
                    m_panel.set_tool(tool);
                },
                entry->GetId());
        }
        PopupMenu(&menu, wxPoint(rect.GetRight() + 1, rect.GetTop()));
    }

    // Icon in the colour of the text of the menus, on a transparent bitmap.
    wxBitmap icon_bitmap(Icon icon) const
    {
        const int size = FromDIP(18);
        wxImage   image(size, size);
        image.InitAlpha();
        std::fill(image.GetAlpha(), image.GetAlpha() + size * size, (unsigned char) 0);
        {
            // The image gets the drawing when the context is destroyed.
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(image));
            if (gc)
                draw_icon(*gc, icon, wxRect(0, 0, size, size),
                          wxGetApp().dark_mode() ? wxColour(240, 240, 242) : wxSystemSettings::GetColour(wxSYS_COLOUR_MENUTEXT), false);
        }
        return wxBitmap(image);
    }

    void on_paint(wxPaintEvent &)
    {
        sync_current();
        wxAutoBufferedPaintDC dc(this);
        const bool            dark = wxGetApp().dark_mode();
        dc.SetBackground(wxBrush(dark ? wxColour(37, 37, 41) : wxColour(235, 237, 240)));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        // Buttons in relief, dark icons: they look like what can be clicked.
        const wxColour face  = dark ? wxColour(60, 60, 66) : wxColour(255, 255, 255);
        const wxColour edge  = dark ? wxColour(88, 88, 96) : wxColour(196, 201, 207);
        const wxColour ink   = dark ? wxColour(244, 244, 246) : wxColour(26, 30, 34);
        const wxColour faint = dark ? wxColour(165, 165, 172) : wxColour(98, 104, 110);
        const wxColour hover = dark ? wxColour(16, 64, 95) : wxColour(225, 239, 251);
        const wxColour accent(8, 116, 224); // Orio : bleu des boutons
        const wxColour white(255, 255, 255);
        const wxFont  &font = wxGetApp().small_font();

        // Titles of the sections, with a line after them.
        for (const Section &section : m_sections) {
            const wxRect rect   = scaled(section.rect);
            double       width  = 0.;
            double       height = 0.;
            gc->SetFont(font, faint);
            gc->GetTextExtent(section.title, &width, &height);
            gc->DrawText(section.title, rect.x + FromDIP(2), rect.y + 0.5 * (rect.height - height));
            const double from = rect.x + FromDIP(8) + width;
            if (from < rect.GetRight()) {
                gc->SetPen(wxPen(edge, 1));
                gc->StrokeLine(from, rect.y + 0.5 * rect.height, rect.GetRight(), rect.y + 0.5 * rect.height);
            }
        }

        for (int i = 0; i < int(m_items.size()); ++ i) {
            const Item  &item      = m_items[size_t(i)];
            const wxRect rect      = scaled(item.rect);
            const bool   is_on     = is_active(item);
            const bool   is_hover  = i == m_hover;
            const bool   with_menu = item.variants.size() > 1;
            // The tool or the plane in use is blue.
            gc->SetPen(wxPen(is_on || is_hover ? accent : edge, 1));
            gc->SetBrush(wxBrush(is_on ? accent : is_hover ? hover : face));
            gc->DrawRoundedRectangle(rect.x + 0.5, rect.y + 0.5, rect.width - 1., rect.height - 1., FromDIP(7));
            const wxColour &tone = is_on ? white : ink;
            const int       icon = FromDIP(ICON);
            draw_icon(*gc, icon_of(item), wxRect(rect.x + (rect.width - icon) / 2, rect.y + FromDIP(3), icon, icon), tone, is_on);
            // Its name under it, smaller if it is too long.
            const wxString label = label_of(item);
            if (!label.empty()) {
                double width  = 0.;
                double height = 0.;
                gc->SetFont(font, tone);
                gc->GetTextExtent(label, &width, &height);
                const double room = rect.width - FromDIP(with_menu ? 12 : 6);
                if (width > room && width > 0.) {
                    gc->SetFont(font.Scaled(float(room / width)), tone);
                    gc->GetTextExtent(label, &width, &height);
                }
                gc->DrawText(label, rect.x + 0.5 * (rect.width - width), rect.GetBottom() - FromDIP(3) - height);
            }
            // The other tools of its kind are in its menu.
            if (with_menu) {
                const wxRect   corner = flyout(rect);
                const double   x      = corner.GetRight() - FromDIP(2);
                const double   y      = corner.GetBottom() - FromDIP(2);
                const double   d      = FromDIP(6);
                wxGraphicsPath mark   = gc->CreatePath();
                mark.MoveToPoint(x, y - d);
                mark.AddLineToPoint(x, y);
                mark.AddLineToPoint(x - d, y);
                mark.CloseSubpath();
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(is_on ? white : accent));
                gc->FillPath(mark);
            }
        }

        // More buttons above or below: a band with an arrow, the wheel scrolls.
        const wxSize size  = GetClientSize();
        const double band  = FromDIP(12);
        const double arrow = FromDIP(5);
        auto scroll_mark = [&](double top, bool up) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(dark ? wxColour(37, 37, 41, 230) : wxColour(235, 237, 240, 230)));
            gc->DrawRectangle(0., top, size.GetWidth(), band);
            const double   cx   = 0.5 * size.GetWidth();
            const double   cy   = top + 0.5 * band;
            wxGraphicsPath mark = gc->CreatePath();
            mark.MoveToPoint(cx - arrow, up ? cy + 0.5 * arrow : cy - 0.5 * arrow);
            mark.AddLineToPoint(cx + arrow, up ? cy + 0.5 * arrow : cy - 0.5 * arrow);
            mark.AddLineToPoint(cx, up ? cy - 0.5 * arrow : cy + 0.5 * arrow);
            mark.CloseSubpath();
            gc->SetBrush(wxBrush(faint));
            gc->FillPath(mark);
        };
        if (m_scroll > 0)
            scroll_mark(0., true);
        if (FromDIP(m_bottom) - m_scroll > size.GetHeight())
            scroll_mark(size.GetHeight() - band, false);
    }

    // Icons drawn on a grid of 24 x 24, its y axis down.
    void draw_icon(wxGraphicsContext &gc, Icon icon, const wxRect &rect, const wxColour &ink, bool active) const
    {
        using Points = std::vector<std::pair<double, double>>;
        const double   s      = std::min(rect.width, rect.height) / 24.;
        const double   x0     = rect.x + 0.5 * (rect.width - 24. * s);
        const double   y0     = rect.y + 0.5 * (rect.height - 24. * s);
        // The points to click in blue, the surfaces tinted, the construction lines light.
        const wxColour accent = active ? wxColour(255, 255, 255) : wxColour(8, 119, 232);
        const wxColour tint   = active ? wxColour(255, 255, 255, 70) : wxColour(8, 119, 232, 50);
        const wxColour faint(ink.Red(), ink.Green(), ink.Blue(), 110);
        auto X = [&](double x) { return x0 + x * s; };
        auto Y = [&](double y) { return y0 + y * s; };
        auto pen = [&](const wxColour &colour, double width) {
            gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour).Width(width * s).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
        };
        auto line = [&](double xa, double ya, double xb, double yb) { gc.StrokeLine(X(xa), Y(ya), X(xb), Y(yb)); };
        auto dot  = [&](double x, double y, double radius, const wxColour &colour) {
            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.SetBrush(wxBrush(colour));
            gc.DrawEllipse(X(x) - radius * s, Y(y) - radius * s, 2. * radius * s, 2. * radius * s);
            gc.SetBrush(*wxTRANSPARENT_BRUSH);
            pen(ink, 2.);
        };
        // Polyline through points of the grid, closed and filled if asked.
        auto stroke = [&](const Points &points, bool closed, const wxColour *fill) {
            if (points.size() < 2)
                return;
            wxGraphicsPath path = gc.CreatePath();
            path.MoveToPoint(X(points.front().first), Y(points.front().second));
            for (size_t i = 1; i < points.size(); ++ i)
                path.AddLineToPoint(X(points[i].first), Y(points[i].second));
            if (closed)
                path.CloseSubpath();
            if (fill != nullptr) {
                gc.SetBrush(wxBrush(*fill));
                gc.FillPath(path);
                gc.SetBrush(*wxTRANSPARENT_BRUSH);
            }
            gc.StrokePath(path);
        };
        // Points of an arc of the grid, its angles in degrees, counterclockwise on the screen.
        auto arc_points = [](double cx, double cy, double r, double from, double to) {
            Points    points;
            const int steps = std::max(4, int(std::abs(to - from) / 10.));
            for (int i = 0; i <= steps; ++ i) {
                const double a = (from + (to - from) * double(i) / double(steps)) * PI / 180.;
                points.emplace_back(cx + r * std::cos(a), cy - r * std::sin(a));
            }
            return points;
        };
        auto circle = [&](double cx, double cy, double r, const wxColour *fill) {
            if (fill != nullptr)
                gc.SetBrush(wxBrush(*fill));
            gc.DrawEllipse(X(cx - r), Y(cy - r), 2. * r * s, 2. * r * s);
            gc.SetBrush(*wxTRANSPARENT_BRUSH);
        };
        // Regular hexagon, a vertex at the angle start in degrees.
        auto hexagon = [](double cx, double cy, double r, double start) {
            Points points;
            for (int k = 0; k < 6; ++ k) {
                const double a = (start + 60. * k) * PI / 180.;
                points.emplace_back(cx + r * std::cos(a), cy - r * std::sin(a));
            }
            return points;
        };
        // Straight slot between the centers (xa, y) and (xb, y).
        auto stadium = [&](double xa, double xb, double y, double r) {
            Points points = arc_points(xb, y, r, -90., 90.);
            for (const auto &p : arc_points(xa, y, r, 90., 270.))
                points.push_back(p);
            return points;
        };
        auto plane_fill = [](SketchPlane plane) { return to_colour(sketch_plane_rgb(plane), 215); };
        pen(ink, 2.);
        gc.SetBrush(*wxTRANSPARENT_BRUSH);

        switch (icon) {
        case Icon::Select:
            pen(ink, 1.6);
            stroke({ { 7., 3. }, { 7., 19. }, { 11., 15. }, { 14., 21. }, { 16.5, 20. }, { 13.5, 14. }, { 19., 14. } }, true, &ink);
            break;
        case Icon::Line:
            line(5., 19., 19., 5.);
            dot(5., 19., 2.6, accent);
            dot(19., 5., 2.6, accent);
            break;
        case Icon::Arc:
            // Its start and its end, then a point of it.
            stroke(arc_points(12., 17., 8., 180., 0.), false, nullptr);
            dot(4., 17., 2.6, accent);
            dot(20., 17., 2.6, accent);
            dot(12., 9., 2.2, active ? accent : wxColour(26, 166, 153));
            break;
        case Icon::ArcCenter:
            // Its center, its start, then its end.
            pen(faint, 1.2);
            line(6., 18., 20., 18.);
            pen(ink, 2.);
            stroke(arc_points(6., 18., 14., 0., 80.), false, nullptr);
            dot(6., 18., 1.9, ink);
            dot(20., 18., 2.4, accent);
            dot(6. + 14. * std::cos(80. * PI / 180.), 18. - 14. * std::sin(80. * PI / 180.), 2.4, accent);
            break;
        case Icon::ArcTangent:
            // Along a line, then turning away from it.
            line(3., 19., 11., 19.);
            stroke(arc_points(11., 11., 8., -90., 40.), false, nullptr);
            dot(11., 19., 2.4, accent);
            dot(11. + 8. * std::cos(40. * PI / 180.), 11. - 8. * std::sin(40. * PI / 180.), 2.4, accent);
            break;
        case Icon::Rectangle:
            gc.SetBrush(wxBrush(tint));
            gc.DrawRectangle(X(4.), Y(6.), 16. * s, 12. * s);
            gc.SetBrush(*wxTRANSPARENT_BRUSH);
            // Its two opposite corners.
            dot(4., 18., 2.4, accent);
            dot(20., 6., 2.4, accent);
            break;
        case Icon::Rectangle3Points:
            // A side, then the other side.
            stroke({ { 4., 12. }, { 12., 4. }, { 19., 11. }, { 11., 19. } }, true, &tint);
            dot(4., 12., 2.3, accent);
            dot(12., 4., 2.3, accent);
            dot(19., 11., 2.3, accent);
            break;
        case Icon::RectangleCenter:
            gc.SetBrush(wxBrush(tint));
            gc.DrawRectangle(X(4.), Y(6.), 16. * s, 12. * s);
            gc.SetBrush(*wxTRANSPARENT_BRUSH);
            // Its center, then a corner.
            dot(12., 12., 2.2, accent);
            dot(20., 6., 2.4, accent);
            break;
        case Icon::Circle:
            circle(12., 12., 8.5, &tint);
            // Its center, then a point of it.
            dot(12., 12., 2., ink);
            dot(20.5, 12., 2.4, accent);
            break;
        case Icon::Circle2Points:
            circle(12., 12., 8.5, &tint);
            pen(faint, 1.2);
            line(3.5, 12., 20.5, 12.);
            pen(ink, 2.);
            dot(3.5, 12., 2.4, accent);
            dot(20.5, 12., 2.4, accent);
            break;
        case Icon::Circle3Points:
            circle(12., 12., 8.5, &tint);
            for (const double a : { 90., 210., 330. })
                dot(12. + 8.5 * std::cos(a * PI / 180.), 12. - 8.5 * std::sin(a * PI / 180.), 2.4, accent);
            break;
        case Icon::CircleTangent2:
            // In the corner of two lines.
            line(3., 21., 3., 3.);
            line(3., 21., 21., 21.);
            circle(10.5, 13.5, 7.5, &tint);
            dot(3., 13.5, 2.2, accent);
            dot(10.5, 21., 2.2, accent);
            break;
        case Icon::CircleTangent3: {
            // Inside a triangle.
            const double ax = 2., ay = 21., bx = 22., by = 21., cx = 12., cy = 3.;
            stroke({ { ax, ay }, { bx, by }, { cx, cy } }, true, nullptr);
            const double la = std::hypot(bx - cx, by - cy);
            const double lb = std::hypot(ax - cx, ay - cy);
            const double lc = std::hypot(ax - bx, ay - by);
            const double ix = (la * ax + lb * bx + lc * cx) / (la + lb + lc);
            const double iy = (la * ay + lb * by + lc * cy) / (la + lb + lc);
            const double r  = std::abs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) / (la + lb + lc);
            circle(ix, iy, r, &tint);
            dot(ix, ay, 2.2, accent);
            break;
        }
        case Icon::PolygonCircumscribed:
            // Around a circle: its center, then the middle of a side.
            pen(faint, 1.2);
            circle(12., 12., 7.8, nullptr);
            pen(ink, 2.);
            stroke(hexagon(12., 12., 9., 0.), true, nullptr);
            dot(12., 12., 1.8, ink);
            dot(12., 12. - 7.8, 2.3, accent);
            break;
        case Icon::PolygonInscribed:
            // In a circle: its center, then a vertex.
            pen(faint, 1.2);
            circle(12., 12., 9., nullptr);
            pen(ink, 2.);
            stroke(hexagon(12., 12., 9., 0.), true, nullptr);
            dot(12., 12., 1.8, ink);
            dot(21., 12., 2.3, accent);
            break;
        case Icon::PolygonEdge:
            // A side, then the side where it is.
            stroke(hexagon(12., 11., 9., 0.), true, &tint);
            dot(7.5, 18.8, 2.3, accent);
            dot(16.5, 18.8, 2.3, accent);
            break;
        case Icon::Ellipse:
            gc.SetBrush(wxBrush(tint));
            gc.DrawEllipse(X(2.5), Y(6.5), 19. * s, 11. * s);
            gc.SetBrush(*wxTRANSPARENT_BRUSH);
            dot(12., 12., 1.9, ink);
            dot(21.5, 12., 2.3, accent);
            dot(12., 6.5, 2.3, accent);
            break;
        case Icon::SlotCenters:
            stroke(stadium(7., 17., 12., 5.), true, &tint);
            dot(7., 12., 2.2, accent);
            dot(17., 12., 2.2, accent);
            break;
        case Icon::SlotOverall:
            stroke(stadium(7., 17., 12., 5.), true, &tint);
            dot(2., 12., 2.2, accent);
            dot(22., 12., 2.2, accent);
            break;
        case Icon::SlotCenter:
            stroke(stadium(7., 17., 12., 5.), true, &tint);
            dot(12., 12., 2.2, accent);
            dot(17., 12., 1.7, ink);
            break;
        case Icon::SlotArc3Points:
        case Icon::SlotArcCenter: {
            // Along an arc around (12, 22) of radius 10, 3 wide each side, from the left to the right over the top.
            const double ex = 12. + 10. * std::cos(30. * PI / 180.);
            const double sx = 12. + 10. * std::cos(150. * PI / 180.);
            const double ey = 22. - 10. * std::sin(30. * PI / 180.);
            Points       outline = arc_points(12., 22., 13., 150., 30.);
            for (const Points &part : { arc_points(ex, ey, 3., 30., -150.), arc_points(12., 22., 7., 30., 150.), arc_points(sx, ey, 3., 330., 150.) })
                outline.insert(outline.end(), part.begin(), part.end());
            stroke(outline, true, &tint);
            if (icon == Icon::SlotArc3Points) {
                dot(sx, ey, 2.2, accent);
                dot(ex, ey, 2.2, accent);
                dot(12., 12., 2.2, accent);
            } else {
                dot(12., 21.5, 1.9, ink);
                dot(sx, ey, 2.2, accent);
                dot(ex, ey, 2.2, accent);
            }
            break;
        }
        case Icon::Spline: {
            // Through its points.
            Points points;
            for (int i = 0; i <= 24; ++ i) {
                const double t = double(i) / 24.;
                points.emplace_back(3. + 18. * t, 13. - 7. * std::sin(2. * PI * t));
            }
            stroke(points, false, nullptr);
            dot(3., 13., 2.2, accent);
            dot(7.5, 6., 2.2, accent);
            dot(16.5, 20., 2.2, accent);
            dot(21., 13., 2.2, accent);
            break;
        }
        case Icon::Conic: {
            // Its ends, its apex.
            pen(faint, 1.2);
            line(3., 20., 12., 3.);
            line(12., 3., 21., 20.);
            pen(ink, 2.);
            Points points;
            for (int i = 0; i <= 20; ++ i) {
                const double t  = double(i) / 20.;
                const double wa = (1. - t) * (1. - t);
                const double wb = 2. * t * (1. - t);
                const double wc = t * t;
                points.emplace_back(wa * 3. + wb * 12. + wc * 21., wa * 20. + wb * 3. + wc * 20.);
            }
            stroke(points, false, nullptr);
            dot(3., 20., 2.3, accent);
            dot(21., 20., 2.3, accent);
            dot(12., 3., 1.8, ink);
            break;
        }
        case Icon::Point:
            line(12., 3.5, 12., 8.5);
            line(12., 15.5, 12., 20.5);
            line(3.5, 12., 8.5, 12.);
            line(15.5, 12., 20.5, 12.);
            dot(12., 12., 3., accent);
            break;
        case Icon::Text:
            // A letter, from the start of its baseline.
            stroke({ { 5., 20. }, { 12., 4. }, { 19., 20. } }, false, nullptr);
            line(8., 14., 16., 14.);
            dot(4., 21., 2.2, accent);
            break;
        case Icon::Mirror:
            // A shape, its line, its mirrored copy.
            pen(faint, 1.2);
            for (double y = 2.; y < 22.; y += 5.)
                line(12., y, 12., std::min(y + 3., 22.));
            pen(ink, 2.);
            stroke({ { 3., 19. }, { 9., 19. }, { 9., 6. } }, true, &tint);
            pen(accent, 2.);
            stroke({ { 21., 19. }, { 15., 19. }, { 15., 6. } }, true, nullptr);
            pen(ink, 2.);
            break;
        case Icon::CircularPattern:
            // Copies around a center.
            dot(12., 12., 1.6, ink);
            for (int k = 0; k < 6; ++ k) {
                const double a  = (90. + 60. * k) * PI / 180.;
                const double cx = 12. + 8. * std::cos(a);
                const double cy = 12. - 8. * std::sin(a);
                if (k == 0)
                    dot(cx, cy, 2.4, ink);
                else {
                    pen(accent, 1.5);
                    circle(cx, cy, 2., nullptr);
                }
            }
            pen(ink, 2.);
            break;
        case Icon::RectangularPattern:
            // Copies in rows.
            for (int row = 0; row < 2; ++ row)
                for (int column = 0; column < 3; ++ column) {
                    const double x = 3. + 7. * column;
                    const double y = 6. + 7. * row;
                    if (row == 0 && column == 0) {
                        gc.SetBrush(wxBrush(ink));
                        gc.DrawRectangle(X(x), Y(y), 5. * s, 5. * s);
                        gc.SetBrush(*wxTRANSPARENT_BRUSH);
                    } else {
                        pen(accent, 1.5);
                        stroke({ { x, y }, { x + 5., y }, { x + 5., y + 5. }, { x, y + 5. } }, true, nullptr);
                    }
                }
            pen(ink, 2.);
            break;
        case Icon::Fillet:
        case Icon::ChamferEqual:
        case Icon::ChamferDistanceAngle:
        case Icon::ChamferTwoDistances: {
            // A corner, rounded or cut; the corner before, light.
            const double down   = icon == Icon::ChamferTwoDistances ? 15. : 11.;
            const double across = icon == Icon::ChamferDistanceAngle ? 13. : 11.;
            pen(faint, 1.2);
            line(5., down, 5., 5.);
            line(5., 5., across, 5.);
            pen(ink, 2.);
            line(5., 21., 5., down);
            line(across, 5., 21., 5.);
            pen(accent, 2.);
            if (icon == Icon::Fillet)
                stroke(arc_points(11., 11., 6., 180., 90.), false, nullptr);
            else
                line(5., down, across, 5.);
            if (icon == Icon::ChamferDistanceAngle) {
                pen(faint, 1.2);
                stroke(arc_points(5., down, 4.5, 90. - std::atan2(across - 5., down - 5.) * 180. / PI, 90.), false, nullptr);
            }
            pen(ink, 2.);
            break;
        }
        case Icon::Offset:
            // A path and its parallel copy.
            line(8., 4., 8., 13.);
            stroke(arc_points(12., 13., 4., 180., 360.), false, nullptr);
            line(16., 13., 16., 4.);
            pen(accent, 2.);
            line(3.5, 4., 3.5, 13.);
            stroke(arc_points(12., 13., 8.5, 180., 360.), false, nullptr);
            line(20.5, 13., 20.5, 4.);
            pen(ink, 2.);
            break;
        case Icon::Trim:
            // The part beyond the line crossing it, removed.
            line(9., 3., 9., 21.);
            line(3., 12., 9., 12.);
            pen(wxColour(226, 75, 74), 2.);
            for (double x = 11.; x < 21.; x += 4.)
                line(x, 12., std::min(x + 2., 21.), 12.);
            pen(ink, 2.);
            break;
        case Icon::Extend:
            // Going on up to the line.
            line(19., 3., 19., 21.);
            line(3., 12., 10., 12.);
            pen(accent, 2.);
            for (double x = 12.; x < 18.; x += 3.5)
                line(x, 12., std::min(x + 2., 18.), 12.);
            dot(19., 12., 2.3, accent);
            break;
        case Icon::Break:
            // Split where the line crosses it.
            pen(faint, 1.2);
            line(12., 4., 12., 20.);
            pen(ink, 2.);
            line(3., 12., 10., 12.);
            line(14., 12., 21., 12.);
            dot(10., 12., 2., accent);
            dot(14., 12., 2., accent);
            break;
        case Icon::Scale:
            // Bigger from a base point.
            stroke({ { 4., 14. }, { 10., 14. }, { 10., 20. }, { 4., 20. } }, true, &tint);
            pen(accent, 2.);
            stroke({ { 4., 4. }, { 20., 4. }, { 20., 20. }, { 4., 20. } }, true, nullptr);
            pen(faint, 1.2);
            line(10., 14., 17., 7.);
            pen(ink, 2.);
            dot(4., 20., 2.2, ink);
            break;
        case Icon::Move:
            // Four arrows.
            line(12., 3., 12., 21.);
            line(3., 12., 21., 12.);
            stroke({ { 9.5, 5.5 }, { 12., 3. }, { 14.5, 5.5 } }, false, nullptr);
            stroke({ { 9.5, 18.5 }, { 12., 21. }, { 14.5, 18.5 } }, false, nullptr);
            stroke({ { 5.5, 9.5 }, { 3., 12. }, { 5.5, 14.5 } }, false, nullptr);
            stroke({ { 18.5, 9.5 }, { 21., 12. }, { 18.5, 14.5 } }, false, nullptr);
            break;
        case Icon::Copy:
            // A shape and its copy.
            stroke({ { 3., 9. }, { 13., 9. }, { 13., 19. }, { 3., 19. } }, true, &tint);
            pen(accent, 2.);
            stroke({ { 11., 5. }, { 21., 5. }, { 21., 15. }, { 11., 15. } }, true, nullptr);
            pen(ink, 2.);
            break;
        case Icon::Rotate: {
            // Turning around a center.
            stroke(arc_points(12., 12., 8., 40., 320.), false, nullptr);
            const double a  = 40. * PI / 180.;
            const double ex = 12. + 8. * std::cos(a);
            const double ey = 12. - 8. * std::sin(a);
            stroke({ { ex - 3.5, ey - 1. }, { ex, ey }, { ex + 0.5, ey + 3.5 } }, false, nullptr);
            dot(12., 12., 1.8, accent);
            break;
        }
        case Icon::Extrude: {
            // Nozzle above a bead: orange when extruding, dashed blue when travelling, the colours of the view.
            pen(ink, 1.6);
            stroke({ { 8., 2.5 }, { 16., 2.5 }, { 16., 7. }, { 12., 12. }, { 8., 7. } }, true, &ink);
            if (m_panel.extrude_shown()) {
                pen(wxColour(242, 140, 38), 3.4);
                line(4., 18., 20., 18.);
            } else {
                pen(wxColour(64, 140, 242), 2.4);
                for (double x = 3.; x < 20.; x += 6.)
                    line(x, 18., x + 3., 18.);
            }
            break;
        }
        case Icon::Horizontal: {
            const wxColour fill = plane_fill(SketchPlane::Horizontal);
            pen(ink, 1.6);
            stroke({ { 2., 17. }, { 15., 17. }, { 22., 9. }, { 9., 9. } }, true, &fill);
            break;
        }
        case Icon::Front: {
            const wxColour fill = plane_fill(SketchPlane::Front);
            pen(ink, 1.6);
            stroke({ { 5., 4. }, { 19., 4. }, { 19., 20. }, { 5., 20. } }, true, &fill);
            break;
        }
        case Icon::Side: {
            const wxColour fill = plane_fill(SketchPlane::Side);
            pen(ink, 1.6);
            stroke({ { 8., 3. }, { 16., 8. }, { 16., 21. }, { 8., 16. } }, true, &fill);
            break;
        }
        case Icon::Frame:
            line(4., 9., 4., 4.);
            line(4., 4., 9., 4.);
            line(15., 4., 20., 4.);
            line(20., 4., 20., 9.);
            line(20., 15., 20., 20.);
            line(20., 20., 15., 20.);
            line(9., 20., 4., 20.);
            line(4., 20., 4., 15.);
            gc.SetBrush(wxBrush(tint));
            gc.DrawRectangle(X(8.), Y(10.), 8. * s, 4. * s);
            gc.SetBrush(*wxTRANSPARENT_BRUSH);
            break;
        }
    }

    GCodeSketchPanel     &m_panel;
    std::vector<Item>     m_items;
    std::vector<Section>  m_sections;
    int                   m_hover  { -1 };
    // Where the next button goes, and the bottom of the last one, in DIP.
    int                   m_column { 0 };
    int                   m_next_y { MARGIN };
    int                   m_bottom { 2 * MARGIN + ITEM_H };
    // Pixels scrolled from its top.
    int                   m_scroll { 0 };
};

// Bar on the right of the view: position of the working plane along its normal, the points marked on it. Dragged to
// move the plane, double clicked to type its position.
class GCodeSketchPlaneBar : public wxPanel
{
public:
    GCodeSketchPlaneBar(wxWindow *parent, GCodeSketchPanel &panel)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNO_BORDER)
        , m_panel(panel)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(WIDTH), FromDIP(TOP + BOTTOM + 40)));
        SetToolTip(_L("Working plane, which follows the last point drawn. Drag to move it, wheel to step it, "
                      "double click to type its position."));
        Bind(wxEVT_PAINT, &GCodeSketchPlaneBar::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &evt) {
            if (evt.GetY() < track_top() - FromDIP(8))
                return;
            if (!HasCapture())
                CaptureMouse();
            m_dragging = true;
            m_panel.set_plane_offset(y_to_value(evt.GetY()));
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent &evt) {
            if (m_dragging && evt.LeftIsDown())
                m_panel.set_plane_offset(y_to_value(evt.GetY()));
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
            m_dragging = false;
            if (HasCapture())
                ReleaseMouse();
            m_panel.focus_view();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) { m_dragging = false; });
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent &) { type_position(); });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent &evt) {
            if (evt.GetWheelRotation() != 0)
                m_panel.step_plane(evt.GetWheelRotation() > 0 ? 1 : -1);
        });
    }

private:
    static constexpr int WIDTH  = 66;
    static constexpr int TOP    = 48;
    static constexpr int BOTTOM = 16;

    int track_top() const { return FromDIP(TOP); }
    int track_bottom() const { return std::max(track_top() + FromDIP(20), GetClientSize().GetHeight() - FromDIP(BOTTOM)); }

    int value_to_y(double value) const
    {
        const double low  = m_panel.plane_min();
        const double high = m_panel.plane_max();
        const double f    = high > low ? std::clamp((value - low) / (high - low), 0., 1.) : 0.;
        return int(std::lround(track_bottom() - f * (track_bottom() - track_top())));
    }

    double y_to_value(int y) const
    {
        const double low   = m_panel.plane_min();
        const double high  = m_panel.plane_max();
        const double f     = std::clamp(double(track_bottom() - y) / double(std::max(1, track_bottom() - track_top())), 0., 1.);
        double       value = low + f * (high - low);
        const double step  = m_panel.plane_step();
        if (step > 0.)
            value = std::round(value / step) * step;
        return value;
    }

    void type_position()
    {
        wxTextEntryDialog dialog(this, _L("Position of the working plane (mm):"), _L("Working plane"),
                                 wxString::Format("%.2f", m_panel.plane_offset()));
        if (dialog.ShowModal() == wxID_OK) {
            wxString text = dialog.GetValue();
            text.Replace(",", ".");
            double value = 0.;
            if (text.Trim().Trim(false).ToCDouble(&value))
                m_panel.set_plane_offset(value);
        }
        m_panel.focus_view();
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        const bool     dark  = wxGetApp().dark_mode();
        const wxColour ink   = dark ? wxColour(225, 225, 228) : wxColour(50, 50, 52);
        const wxColour faint = dark ? wxColour(90, 90, 96) : wxColour(205, 205, 210);
        const wxColour mark  = dark ? wxColour(200, 200, 205) : wxColour(90, 90, 95);
        const int      cx    = GetClientSize().GetWidth() / 2;

        // Axis along which the plane moves, and its position.
        static const char *axes[] = { "X", "Y", "Z" };
        const int          axis   = std::clamp(m_panel.plane_axis(), 0, 2);
        dc.SetTextForeground(ink);
        dc.SetFont(wxGetApp().bold_font());
        const wxString name = axes[axis];
        wxSize         size = dc.GetTextExtent(name);
        dc.DrawText(name, cx - size.x / 2, FromDIP(4));
        dc.SetFont(wxGetApp().normal_font());
        const wxString value = wxString::Format("%.2f", m_panel.plane_offset());
        size                 = dc.GetTextExtent(value);
        dc.DrawText(value, cx - size.x / 2, FromDIP(22));

        // Track, with the points.
        dc.SetPen(wxPen(faint, FromDIP(2)));
        dc.DrawLine(cx, track_top(), cx, track_bottom());
        dc.SetPen(wxPen(mark, 1));
        for (double point : m_panel.plane_marks()) {
            const int y = value_to_y(point);
            dc.DrawLine(cx - FromDIP(10), y, cx - FromDIP(3), y);
        }

        // Thumb, in the colour of the plane.
        const int y = value_to_y(m_panel.plane_offset());
        dc.SetPen(wxPen(ink, 1));
        dc.SetBrush(wxBrush(to_colour(sketch_plane_rgb(m_panel.plane()))));
        dc.DrawRoundedRectangle(cx - FromDIP(13), y - FromDIP(5), FromDIP(26), FromDIP(10), FromDIP(3));
    }

    GCodeSketchPanel &m_panel;
    bool              m_dragging { false };
};

GCodeSketchPanel::GCodeSketchPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
{
    SetBackgroundColour(*wxWHITE);
    // An exported .gcode file is printed: a program run alone by the printer, unless chosen otherwise.
    m_export.standalone = true;
    build_ui();
    load_printers();
    load_filaments();
    apply_machine();
    apply_printer_settings();
    apply_filament_settings();
    write_settings();
    set_plane_offset(z_step());
    on_drawing_changed();
    on_tool_changed();
    build_selection_ui();
    update_export_tooltip();
    wxGetApp().UpdateDarkUIWin(this);
    rewrap();
}

GCodeSketchPanel::~GCodeSketchPanel()
{
    delete m_view;
    m_view = nullptr;
}

void GCodeSketchPanel::build_ui()
{
    const int em         = wxGetApp().em_unit();
    auto     *main_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Toolbox on the left of the view: the sketch tools of Fusion 360, those of a kind in one button.
    using Icon    = GCodeSketchToolbox::Icon;
    using Variant = GCodeSketchToolbox::Variant;
    m_toolbox     = new GCodeSketchToolbox(this, *this);
    auto text  = [](const wxString &value) { return std::function<wxString()>([value]() { return value; }); };
    auto plane = [this](SketchPlane plane) { return std::function<bool()>([this, plane]() { return m_plane == plane; }); };
    m_toolbox->add_tools({ Variant{ SketchTool::Select, Icon::Select, _L("Selection"),
                                    _L("Select (V): click a segment, a rectangle or a circle to edit its values on the right, double click: "
                                       "its whole path. Drag: move it.") } });
    m_toolbox->add_command(
        Icon::Extrude, [this]() { return extrude_shown() ? _L("Extrusion") : _L("Travel"); },
        [this]() {
            // With a selection, the button changes it.
            if (first_selected() != nullptr)
                return extrude_shown() ? _L("The selection extrudes. Click (E): it travels instead.") :
                                         _L("The selection travels without extruding. Click (E): it extrudes instead.");
            return m_extrude ? _L("Extrusion (E): click to draw travel moves") : _L("Travel moves (E): click to extrude again");
        },
        nullptr, [this]() { toggle_extrude(); });
    m_toolbox->add_section(_L("Create"));
    m_toolbox->add_tools({ Variant{ SketchTool::Line, Icon::Line, _L("Line"),
                                    _L("Line (L): click its points one after the other. Double click: end the line. Drag from its last "
                                       "point: tangent arc.") } });
    m_toolbox->add_tools(
        { Variant{ SketchTool::Rectangle, Icon::Rectangle, _L("Rectangle"), _L("2-point rectangle (R): two opposite corners, or type its sides.") },
          Variant{ SketchTool::Rectangle3Points, Icon::Rectangle3Points, _L("Rectangle"),
                   _L("3-point rectangle: a side, in any direction, then the other side.") },
          Variant{ SketchTool::RectangleCenter, Icon::RectangleCenter, _L("Rectangle"), _L("Center rectangle: its center, then a corner.") } });
    m_toolbox->add_tools(
        { Variant{ SketchTool::Circle, Icon::Circle, _L("Circle"),
                   _L("Center diameter circle (C): its center, then a point of it, or type its diameter.") },
          Variant{ SketchTool::Circle2Points, Icon::Circle2Points, _L("Circle"), _L("2-point circle: the ends of a diameter.") },
          Variant{ SketchTool::Circle3Points, Icon::Circle3Points, _L("Circle"), _L("3-point circle: three points of it.") },
          Variant{ SketchTool::CircleTangent2, Icon::CircleTangent2, _L("Circle"),
                   _L("2-tangent circle: click two lines, then its center between them, or type its radius.") },
          Variant{ SketchTool::CircleTangent3, Icon::CircleTangent3, _L("Circle"),
                   _L("3-tangent circle: click three lines, near where it touches them.") } });
    m_toolbox->add_tools(
        { Variant{ SketchTool::Arc, Icon::Arc, _L("Arc"), _L("3-point arc (A): click its start, its end, then a point of the arc.") },
          Variant{ SketchTool::ArcCenter, Icon::ArcCenter, _L("Arc"),
                   _L("Center point arc: its center, its start, then its end, turning the way the mouse turns.") },
          Variant{ SketchTool::ArcTangent, Icon::ArcTangent, _L("Arc"),
                   _L("Tangent arc: from the end of a line or of an arc, going on along it, then its end.") } });
    m_toolbox->add_tools(
        { Variant{ SketchTool::PolygonCircumscribed, Icon::PolygonCircumscribed, _L("Polygon"),
                   _L("Circumscribed polygon: its center, then the middle of a side. Its number of sides is typed (Tab).") },
          Variant{ SketchTool::PolygonInscribed, Icon::PolygonInscribed, _L("Polygon"),
                   _L("Inscribed polygon: its center, then a vertex. Its number of sides is typed (Tab).") },
          Variant{ SketchTool::PolygonEdge, Icon::PolygonEdge, _L("Polygon"),
                   _L("Edge polygon: a side, then the side where it is. Its number of sides is typed (Tab).") } });
    m_toolbox->add_tools({ Variant{ SketchTool::Ellipse, Icon::Ellipse, _L("Ellipse"),
                                    _L("Ellipse: its center, the end of an axis, then the other axis.") } });
    m_toolbox->add_tools(
        { Variant{ SketchTool::SlotCenters, Icon::SlotCenters, _L("Slot"), _L("Center to center slot: the centers of its ends, then its width.") },
          Variant{ SketchTool::SlotOverall, Icon::SlotOverall, _L("Slot"), _L("Overall slot: its ends, then its width.") },
          Variant{ SketchTool::SlotCenter, Icon::SlotCenter, _L("Slot"),
                   _L("Center point slot: its center, the center of an end, then its width.") },
          Variant{ SketchTool::SlotArc3Points, Icon::SlotArc3Points, _L("Slot"),
                   _L("Three point arc slot: the centers of its ends, a point of its arc, then its width.") },
          Variant{ SketchTool::SlotArcCenter, Icon::SlotArcCenter, _L("Slot"),
                   _L("Center point arc slot: the center of its arc, the centers of its ends, then its width.") } });
    m_toolbox->add_tools({ Variant{ SketchTool::Spline, Icon::Spline, _L("Spline"),
                                    _L("Fit point spline: click its points; double click or Enter: end it; click its first point: close "
                                       "it.") } });
    m_toolbox->add_tools({ Variant{ SketchTool::Conic, Icon::Conic, _L("Conic"),
                                    _L("Conic curve: its start, its end, then its apex, where its tangents meet. Its rho is typed (0.5: "
                                       "parabola).") } });
    m_toolbox->add_tools({ Variant{ SketchTool::Point, Icon::Point, _L("Point"),
                                    _L("Point: the nozzle goes there without extruding, to start the next shape from there or to park "
                                       "it.") } });
    m_toolbox->add_tools({ Variant{ SketchTool::Text, Icon::Text, _L("Text"),
                                    _L("Text: click where it starts, then type it and choose its font and its size.") } });
    m_toolbox->add_tools(
        { Variant{ SketchTool::Mirror, Icon::Mirror, _L("Mirror"),
                   _L("Mirror: click the shapes (or select them first), Enter, then a line of the drawing or two points: their mirrored "
                      "copies.") },
          Variant{ SketchTool::CircularPattern, Icon::CircularPattern, _L("Patterns"),
                   _L("Circular pattern: click the shapes (or select them first), Enter, then the center; type the number of copies (Tab: "
                      "the angle).") },
          Variant{ SketchTool::RectangularPattern, Icon::RectangularPattern, _L("Patterns"),
                   _L("Rectangular pattern: click the shapes (or select them first), Enter, then space the copies with the mouse; type "
                      "their numbers and spacings (Tab).") } });
    m_toolbox->add_section(_L("Modify"));
    m_toolbox->add_tools(
        { Variant{ SketchTool::Fillet, Icon::Fillet, _L("Fillet"), _L("Fillet: click corners to round them, with the radius typed.") },
          Variant{ SketchTool::ChamferEqual, Icon::ChamferEqual, _L("Chamfer"),
                   _L("Equal distance chamfer: click corners between two lines to cut them, with the distance typed.") },
          Variant{ SketchTool::ChamferDistanceAngle, Icon::ChamferDistanceAngle, _L("Chamfer"),
                   _L("Distance and angle chamfer: click corners between two lines to cut them, with the distance on the first one "
                      "and the angle typed.") },
          Variant{ SketchTool::ChamferTwoDistances, Icon::ChamferTwoDistances, _L("Chamfer"),
                   _L("Two distance chamfer: click corners between two lines to cut them, with the distance on each one typed.") } });
    m_toolbox->add_tools(
        { Variant{ SketchTool::Trim, Icon::Trim, _L("Trim"),
                   _L("Trim (T): click the part of a curve to remove, between the lines crossing it.") },
          Variant{ SketchTool::Extend, Icon::Extend, _L("Extend"),
                   _L("Extend: click near the free end of a line or of an arc: it goes on up to the first line it meets.") },
          Variant{ SketchTool::Break, Icon::Break, _L("Break"),
                   _L("Break: click a curve to split it where the lines cross it, or at the point clicked.") } });
    m_toolbox->add_tools({ Variant{ SketchTool::Offset, Icon::Offset, _L("Offset"),
                                    _L("Offset (O): click a path, then the side and the distance with the mouse, or type the distance: a "
                                       "parallel copy.") } });
    m_toolbox->add_tools(
        { Variant{ SketchTool::Move, Icon::Move, _L("Move"),
                   _L("Move (M): click the shapes (or select them first), Enter, then the base point and where it goes.") },
          Variant{ SketchTool::Copy, Icon::Copy, _L("Copy"),
                   _L("Copy: click the shapes (or select them first), Enter, then the base point and where each copy goes.") },
          Variant{ SketchTool::Rotate, Icon::Rotate, _L("Rotate"),
                   _L("Rotate: click the shapes (or select them first), Enter, then the center, a point and where it goes.") },
          Variant{ SketchTool::Scale, Icon::Scale, _L("Scaling"),
                   _L("Sketch scale: click the shapes (or select them first), Enter, then the base point and the factor.") } });
    m_toolbox->add_section(_L("Working plane"));
    m_toolbox->add_command(Icon::Horizontal, text("XY"), text(_L("Horizontal working plane XY (P or Alt: next plane)")),
                           plane(SketchPlane::Horizontal), [this]() { set_plane(SketchPlane::Horizontal); });
    m_toolbox->add_command(Icon::Front, text("XZ"), text(_L("Front working plane XZ, vertical (P or Alt: next plane)")), plane(SketchPlane::Front),
                           [this]() { set_plane(SketchPlane::Front); });
    m_toolbox->add_command(Icon::Side, text("YZ"), text(_L("Side working plane YZ, vertical (P or Alt: next plane)")), plane(SketchPlane::Side),
                           [this]() { set_plane(SketchPlane::Side); });
    m_toolbox->add_command(Icon::Frame, text(_L("Fit view")), text(_L("Frame the bed (Z)")), nullptr, [this]() { frame_bed(); });
    main_sizer->Add(m_toolbox, 0, wxEXPAND);

    // 3D view, with what to do, the position of the cursor and the statistics under it.
    auto *view_sizer = new wxBoxSizer(wxVERTICAL);
    m_view           = new GCodeSketchView(*this, this);
    if (m_view->window() != nullptr)
        view_sizer->Add(m_view->window(), 1, wxEXPAND);
    m_hint_text = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    m_hint_text->SetFont(wxGetApp().bold_font());
    view_sizer->Add(m_hint_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em / 2);
    auto *cursor_line = new wxBoxSizer(wxHORIZONTAL);
    m_cursor_text = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(46 * em, -1), wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    m_status_text = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                     wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END | wxALIGN_RIGHT);
    cursor_line->Add(m_cursor_text, 0, wxALIGN_CENTER_VERTICAL);
    cursor_line->Add(m_status_text, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, em);
    view_sizer->Add(cursor_line, 0, wxEXPAND | wxALL, em / 2);
    main_sizer->Add(view_sizer, 1, wxEXPAND);

    // Working plane on the right of the view.
    m_plane_bar = new GCodeSketchPlaneBar(this, *this);
    main_sizer->Add(m_plane_bar, 0, wxEXPAND);

    // Side panel: the settings and the selection, scrolled, then the export always in sight. Nothing in it is wider
    // than it: its texts wrap at its width, its labels are cut, its fields shrink.
    auto *column = new wxBoxSizer(wxVERTICAL);
    m_side       = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(36 * em, -1), wxVSCROLL);
    m_side->SetScrollRate(0, em);
    m_side->SetBackgroundColour(*wxWHITE);
    wxScrolledWindow *side  = m_side;
    auto             *sizer = new wxBoxSizer(wxVERTICAL);

    auto title = [&](const wxString &label) {
        auto *text_label = new wxStaticText(side, wxID_ANY, label);
        text_label->SetFont(wxGetApp().bold_font());
        sizer->Add(text_label, 0, wxLEFT | wxRIGHT | wxTOP, em);
    };
    // Label cut if the column is narrow, its whole text in its tooltip.
    auto label = [&](const wxString &text) {
        auto *control = new wxStaticText(side, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        control->SetMinSize(wxSize(em, -1));
        control->SetToolTip(text);
        return control;
    };
    auto add = [&](wxWindow *window) { sizer->Add(window, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em / 2); };
    auto row = [&](const wxString &text, wxWindow *control) {
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        line->Add(label(text), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
        line->Add(control, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em / 2);
    };
    auto spin = [&](double min, double max, double increment, unsigned int digits, int width) {
        auto *control = new wxSpinCtrlDouble(side, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(width * em, -1),
                                             wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER, min, max, min, increment);
        control->SetDigits(digits);
        return control;
    };
    auto check = [&](const wxString &text, const wxString &tooltip, bool value) {
        auto *control = new wxCheckBox(side, wxID_ANY, text);
        control->SetToolTip(tooltip);
        control->SetValue(value);
        return control;
    };
    // Buttons side by side, as wide as each other.
    auto buttons = [&](std::initializer_list<std::pair<wxString, std::function<void()>>> items) {
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        for (const auto &item : items)
            line->Add(make_button(side, item.first, item.second), 1, wxRIGHT, em / 2);
        sizer->Add(line, 0, wxEXPAND | wxLEFT | wxTOP, em / 2);
    };

    title(_L("Printer"));
    m_printer = new wxChoice(side, wxID_ANY);
    // The name of a preset never widens the column.
    m_printer->SetMinSize(wxSize(8 * em, -1));
    add(m_printer);
    title(_L("Filament"));
    m_filament = new wxChoice(side, wxID_ANY);
    m_filament->SetMinSize(wxSize(8 * em, -1));
    add(m_filament);

    // Values of what the Select tool selected, built with the selection.
    title(_L("Selection"));
    m_selection_box = new wxPanel(side, wxID_ANY);
    m_selection_box->SetBackgroundColour(*wxWHITE);
    m_selection_box->SetSizer(new wxBoxSizer(wxVERTICAL));
    sizer->Add(m_selection_box, 0, wxEXPAND | wxLEFT | wxRIGHT, em / 2);

    title(_L("Extrusion"));
    m_line_width   = spin(0.05, 5., 0.01, 2, 8);
    m_layer_height = spin(0.01, 5., 0.01, 2, 8);
    m_print_speed  = spin(1., 1000., 1., 0, 8);
    m_travel_speed = spin(1., 1000., 10., 0, 8);
    m_flow         = spin(1., 500., 1., 0, 8);
    m_retract      = spin(0., 20., 0.1, 2, 8);
    m_z_hop        = spin(0., 10., 0.1, 2, 8);
    row(_L("Line width (mm)"), m_line_width);
    row(_L("Layer height (mm)"), m_layer_height);
    row(_L("Print speed (mm/s)"), m_print_speed);
    row(_L("Travel speed (mm/s)"), m_travel_speed);
    row(_L("Flow (%)"), m_flow);
    row(_L("Retraction (mm)"), m_retract);
    row(_L("Z hop (mm)"), m_z_hop);

    title(_L("Layers"));
    m_layers = new wxSpinCtrl(side, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(8 * em, -1), wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER, 1,
                              10000, 1);
    m_layers->SetToolTip(_L("The drawing is printed on this number of layers, one layer height above the other: draw a closed "
                            "outline to print an object."));
    row(_L("Number of layers"), m_layers);
    m_spiral = check(_L("Spiral (vase mode)"), _L("Spiral (the height rises continuously, as in vase mode)"), false);
    add(m_spiral);

    title(_L("Snapping and constraints"));
    m_grid = new wxChoice(side, wxID_ANY);
    m_grid->Append(_L("None"));
    for (size_t i = 1; i < sizeof(GRID_STEPS) / sizeof(GRID_STEPS[0]); ++ i)
        m_grid->Append(wxString::Format("%g mm", GRID_STEPS[i]));
    m_grid->SetSelection(DEFAULT_GRID);
    row(_L("Grid"), m_grid);
    m_snap_points = check(_L("Snap to the points"), _L("Snap to the ends, middles and centers"), true);
    add(m_snap_points);
    m_snap_axes = check(_L("Snap to the axes of the last point"),
                        _L("Near the axes through the last point, the cursor follows them: vertical lines are drawn without any key."), true);
    add(m_snap_axes);
    m_constraints = check(_L("Perpendicular, parallel, tangent, aligned"),
                          _L("Constraints found while drawing, as in Fusion 360. Hold Ctrl to draw without any of them."), true);
    add(m_constraints);

    title(_L("File"));
    buttons({ { _L("New"), [this]() { new_drawing(); } }, { _L("Open..."), [this]() { open_drawing(); } } });
    buttons({ { _L("Undo"), [this]() { undo(); } }, { _L("Redo"), [this]() { redo(); } } });

    const wxString help_text =
        _L("The tools work as the sketch tools of Fusion 360; the buttons with a small triangle hold several of them (right "
           "click). Line: click its points, double click to end it, drag from its last point for a tangent arc.\n"
           "While drawing, the dimensions show on the shape: type a value (length, angle, sides, diameter, radius, width), Tab "
           "goes to the next one and locks the value, Enter places the point. Enter alone: done. Esc: back to Select.\n"
           "Mirror, patterns, move, copy, rotate, scale: click the shapes (or select them first), Enter, then their points. "
           "Fillet, chamfer, trim, extend, break: click the corners or the curves. Offset: a path, then the distance.\n"
           "Constraints: the ends, middles and centers, the X, Y, Z axes of the last point, the points lined up, "
           "perpendicular, parallel, tangent. Ctrl: none. Shift: up and down out of the plane.\n"
           "The working plane follows the last point. Alt or P: next plane. Drag its grid with the middle button, or the bar on "
           "the right of the view, Ctrl + wheel, Page Up / Page Down: move it, even while drawing.\n"
           "Drag: rotate the view. Right click: menu. Right or middle drag: pan. Wheel: zoom. Z: frame the bed. 0 to 6: views. "
           "K: perspective. E: extrude or travel, the selection too. Ctrl + Z / Ctrl + Y: undo / redo. Ctrl + E: export. T: trim. O: offset. M: move.");
    auto *help = new wxStaticText(side, wxID_ANY, help_text);
    help->SetFont(wxGetApp().small_font());
    // Wrapped again at the width of the column once it is known (rewrap()).
    help->Wrap(32 * em);
    m_wrapped.emplace_back(help, help_text);
    sizer->Add(help, 0, wxEXPAND | wxALL, em);

    side->SetSizer(sizer);
    column->Add(side, 1, wxEXPAND);

    // Export of the G-code, always in sight.
    m_export_button = new ::Button(this, _L("Export G-code..."));
    StateColor green(std::pair<wxColour, int>(wxColour(5, 96, 194), StateColor::Pressed),
                     std::pair<wxColour, int>(wxColour(43, 140, 242), StateColor::Hovered),
                     std::pair<wxColour, int>(wxColour(8, 116, 224), StateColor::Normal));
    m_export_button->SetBackgroundColor(green);
    m_export_button->SetBorderWidth(0);
    m_export_button->SetTextColor(wxColour("#FEFEFE"));
    m_export_button->SetFont(Label::Head_14);
    m_export_button->SetCornerRadius(FromDIP(8));
    m_export_button->SetMinSize(wxSize(FromDIP(80), FromDIP(38)));
    m_export_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { export_gcode(); });
    column->Add(m_export_button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
    ::Button *gcode = make_button(this, _L("Show the G-code..."), [this]() { show_gcode(); });
    gcode->SetToolTip(_L("Show the G-code of the drawing and its options (program or block, temperatures), to copy it, save it or "
                         "send it to the printer"));
    column->Add(gcode, 0, wxEXPAND | wxALL, em);
    main_sizer->Add(column, 0, wxEXPAND);
    SetSizer(main_sizer);

    // The texts of the column wrap again at its new width.
    m_side->Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
        evt.Skip();
        if (m_side->GetClientSize().GetWidth() != m_wrap_width)
            rewrap();
    });

    // Events.
    m_printer->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int selection = m_printer->GetSelection();
        if (m_updating || selection < 0 || size_t(selection) >= m_printer_names.size())
            return;
        m_printer_name = m_printer_names[size_t(selection)];
        apply_machine();
        apply_printer_settings();
        write_settings();
        on_drawing_changed();
    });
    m_filament->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int selection = m_filament->GetSelection();
        if (m_updating || selection < 0 || size_t(selection) >= m_filament_names.size())
            return;
        m_filament_name = m_filament_names[size_t(selection)];
        apply_filament_settings();
        write_settings();
        on_drawing_changed();
    });
    m_grid->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { m_view->refresh(); });
    for (wxCheckBox *control : { m_snap_points, m_snap_axes, m_constraints })
        control->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { m_view->refresh(); });
    for (wxSpinCtrlDouble *control : { m_line_width, m_layer_height, m_print_speed, m_travel_speed, m_flow, m_retract, m_z_hop })
        control->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent &) {
            if (m_updating)
                return;
            read_settings();
            on_drawing_changed();
        });
    m_layers->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
        if (m_updating)
            return;
        read_settings();
        on_drawing_changed();
    });
    m_spiral->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        if (m_updating)
            return;
        read_settings();
        on_drawing_changed();
    });
}

::Button *GCodeSketchPanel::make_button(wxWindow *parent, const wxString &label, std::function<void()> action)
{
    auto *button = new ::Button(parent, label);
    // Light colours, which StateColor turns into their dark mode ones.
    StateColor background(std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Pressed),
                          std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Hovered),
                          std::pair<wxColour, int>(wxColour("#FFFFFF"), StateColor::Normal));
    button->SetBackgroundColor(background);
    button->SetBorderColor(StateColor(wxColour("#ACACAC")));
    button->SetTextColor(StateColor(wxColour("#262E30")));
    button->SetFont(Label::Body_13);
    button->SetCornerRadius(FromDIP(6));
    button->SetMinSize(wxSize(FromDIP(40), FromDIP(28)));
    button->Bind(wxEVT_BUTTON, [action](wxCommandEvent &) { action(); });
    return button;
}

void GCodeSketchPanel::rewrap()
{
    if (m_side == nullptr)
        return;
    const int em    = wxGetApp().em_unit();
    m_wrap_width    = m_side->GetClientSize().GetWidth();
    const int width = std::max(8 * em, m_wrap_width - 2 * em);
    m_side->Freeze();
    for (const auto &[text, label] : m_wrapped) {
        text->SetLabel(label);
        text->Wrap(width);
    }
    // The fields of the selection are in its box, em / 2 further in on each side.
    for (const auto &[text, label] : m_selection_wrapped) {
        text->SetLabel(label);
        text->Wrap(width - em);
    }
    for (const TextField &field : m_text_fields) {
        field.text->SetLabel(field.shown);
        field.text->Wrap(width - em);
    }
    if (m_selection_box != nullptr)
        m_selection_box->Layout();
    m_side->Layout();
    m_side->FitInside();
    m_side->Thaw();
}

void GCodeSketchPanel::on_activate()
{
    const std::string printer  = m_printer_name;
    const std::string filament = m_filament_name;
    load_printers();
    load_filaments();
    // The printer may have been edited in the meantime.
    apply_machine();
    if (m_printer_name != printer)
        apply_printer_settings();
    if (m_filament_name != filament)
        apply_filament_settings();
    write_settings();
    on_drawing_changed();
    on_tool_changed();
}

void GCodeSketchPanel::release_gl()
{
    if (m_view != nullptr)
        m_view->release_gl();
}

void GCodeSketchPanel::focus_view()
{
    if (m_view != nullptr && m_view->window() != nullptr)
        m_view->window()->SetFocus();
}

void GCodeSketchPanel::frame_bed()
{
    if (m_view != nullptr)
        m_view->zoom_to_bed();
}

void GCodeSketchPanel::load_printers()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    m_printer_names.clear();
    if (bundle != nullptr) {
        for (const Preset &preset : bundle->printers)
            if (preset.is_visible)
                m_printer_names.push_back(preset.name);
        // The printer of the project by default, or when the one chosen no longer exists.
        if (std::find(m_printer_names.begin(), m_printer_names.end(), m_printer_name) == m_printer_names.end())
            m_printer_name = bundle->printers.get_selected_preset_name();
    }
    if (std::find(m_printer_names.begin(), m_printer_names.end(), m_printer_name) == m_printer_names.end() && !m_printer_name.empty())
        m_printer_names.push_back(m_printer_name);

    m_updating = true;
    m_printer->Clear();
    for (const std::string &name : m_printer_names)
        m_printer->Append(from_u8(name));
    const auto it = std::find(m_printer_names.begin(), m_printer_names.end(), m_printer_name);
    if (it != m_printer_names.end())
        m_printer->SetSelection(int(it - m_printer_names.begin()));
    m_updating = false;
}

void GCodeSketchPanel::load_filaments()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    m_filament_names.clear();
    if (bundle != nullptr) {
        // The filaments of the project.
        for (const std::string &name : bundle->filament_presets)
            if (!name.empty() && std::find(m_filament_names.begin(), m_filament_names.end(), name) == m_filament_names.end() &&
                bundle->filaments.find_preset(name) != nullptr)
                m_filament_names.push_back(name);
        if (std::find(m_filament_names.begin(), m_filament_names.end(), m_filament_name) == m_filament_names.end())
            m_filament_name = m_filament_names.empty() ? std::string() : m_filament_names.front();
    }

    m_updating = true;
    m_filament->Clear();
    for (const std::string &name : m_filament_names)
        m_filament->Append(from_u8(name));
    const auto it = std::find(m_filament_names.begin(), m_filament_names.end(), m_filament_name);
    if (it != m_filament_names.end())
        m_filament->SetSelection(int(it - m_filament_names.begin()));
    m_updating = false;
}

const DynamicPrintConfig *GCodeSketchPanel::printer_config() const
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || m_printer_name.empty())
        return nullptr;
    // The printer of the project, with its unsaved changes.
    if (bundle->printers.get_edited_preset().name == m_printer_name)
        return &bundle->printers.get_edited_preset().config;
    const Preset *preset = bundle->printers.find_preset(m_printer_name);
    return preset != nullptr ? &preset->config : nullptr;
}

const DynamicPrintConfig *GCodeSketchPanel::filament_config() const
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || m_filament_name.empty())
        return nullptr;
    if (bundle->filaments.get_edited_preset().name == m_filament_name)
        return &bundle->filaments.get_edited_preset().config;
    const Preset *preset = bundle->filaments.find_preset(m_filament_name);
    return preset != nullptr ? &preset->config : nullptr;
}

void GCodeSketchPanel::apply_machine()
{
    GCodeSketch::Machine machine;
    if (const DynamicPrintConfig *config = printer_config()) {
        if (const auto *area = config->option<ConfigOptionPoints>("printable_area"))
            machine.bed_shape = area->values;
        if (const auto *exclude = config->option<ConfigOptionPoints>("bed_exclude_area"))
            machine.exclude_area = exclude->values;
        machine.max_height = config_float(*config, "printable_height", machine.max_height);
        if (const auto *relative = config->option<ConfigOptionBool>("use_relative_e_distances"))
            machine.relative_e = relative->value;
    }
    if (machine.bed_shape.size() < 3)
        machine.bed_shape = { Vec2d(0., 0.), Vec2d(256., 0.), Vec2d(256., 256.), Vec2d(0., 256.) };
    const bool changed = machine.bed_shape != m_machine.bed_shape || machine.exclude_area != m_machine.exclude_area ||
                         machine.max_height != m_machine.max_height;
    m_machine = machine;
    set_plane_offset(m_plane_offset);
    if (m_view != nullptr) {
        if (changed)
            m_view->on_machine_changed();
        else
            m_view->refresh();
    }
}

void GCodeSketchPanel::apply_printer_settings()
{
    if (const DynamicPrintConfig *config = printer_config()) {
        const double nozzle = config_float(*config, "nozzle_diameter", 0.4);
        m_settings.line_width     = std::round(nozzle * 105.) / 100.;
        m_settings.retract_length = config_float(*config, "retraction_length", m_settings.retract_length);
        m_settings.retract_speed  = config_float(*config, "retraction_speed", m_settings.retract_speed);
        m_settings.z_hop          = config_float(*config, "z_hop", m_settings.z_hop);
    }
    if (PresetBundle *bundle = wxGetApp().preset_bundle) {
        const DynamicPrintConfig &print = bundle->prints.get_edited_preset().config;
        m_settings.layer_height = config_float(print, "layer_height", m_settings.layer_height);
        m_settings.travel_speed = config_float(print, "travel_speed", m_settings.travel_speed);
    }
}

void GCodeSketchPanel::apply_filament_settings()
{
    if (const DynamicPrintConfig *config = filament_config()) {
        m_settings.filament_diameter = config_float(*config, "filament_diameter", m_settings.filament_diameter);
        m_settings.filament_density  = config_float(*config, "filament_density", m_settings.filament_density);
        m_settings.flow_ratio        = config_float(*config, "filament_flow_ratio", 1.);
        m_export.nozzle_temp         = int(std::lround(config_float(*config, "nozzle_temperature", m_export.nozzle_temp)));
        m_export.bed_temp            = int(std::lround(config_float(*config, "hot_plate_temp", m_export.bed_temp)));
    }
}

void GCodeSketchPanel::read_settings()
{
    m_settings.line_width     = m_line_width->GetValue();
    m_settings.layer_height   = m_layer_height->GetValue();
    m_settings.print_speed    = m_print_speed->GetValue();
    m_settings.travel_speed   = m_travel_speed->GetValue();
    m_settings.flow_ratio     = m_flow->GetValue() / 100.;
    m_settings.retract_length = m_retract->GetValue();
    m_settings.z_hop          = m_z_hop->GetValue();
    m_settings.layers         = std::max(1, m_layers->GetValue());
    m_settings.spiral         = m_spiral->GetValue();
}

void GCodeSketchPanel::write_settings()
{
    m_updating = true;
    m_line_width->SetValue(m_settings.line_width);
    m_layer_height->SetValue(m_settings.layer_height);
    m_print_speed->SetValue(m_settings.print_speed);
    m_travel_speed->SetValue(m_settings.travel_speed);
    m_flow->SetValue(m_settings.flow_ratio * 100.);
    m_retract->SetValue(m_settings.retract_length);
    m_z_hop->SetValue(m_settings.z_hop);
    m_layers->SetValue(std::max(1, m_settings.layers));
    m_spiral->SetValue(m_settings.spiral);
    m_updating = false;
}

int GCodeSketchPanel::plane_axis() const
{
    switch (m_plane) {
    case SketchPlane::Front: return 1;
    case SketchPlane::Side: return 0;
    default: return 2;
    }
}

double GCodeSketchPanel::plane_min() const
{
    Vec2d min, max;
    bed_box(m_machine, min, max);
    const int axis = plane_axis();
    return axis == 2 ? 0. : min[axis];
}

double GCodeSketchPanel::plane_max() const
{
    Vec2d min, max;
    bed_box(m_machine, min, max);
    const int axis = plane_axis();
    return axis == 2 ? std::max(m_machine.max_height, 1.) : max[axis];
}

double GCodeSketchPanel::plane_step() const
{
    if (plane_axis() == 2)
        return z_step();
    const double grid = grid_step();
    return grid > 0. ? grid : 1.;
}

double GCodeSketchPanel::z_step() const
{
    return m_settings.layer_height > 0. ? m_settings.layer_height : 0.2;
}

double GCodeSketchPanel::grid_step() const
{
    const int selection = m_grid != nullptr ? m_grid->GetSelection() : DEFAULT_GRID;
    return selection >= 0 && size_t(selection) < sizeof(GRID_STEPS) / sizeof(GRID_STEPS[0]) ? GRID_STEPS[selection] : 0.;
}

bool GCodeSketchPanel::snap_to_points() const
{
    return m_snap_points == nullptr || m_snap_points->GetValue();
}

bool GCodeSketchPanel::snap_to_axes() const
{
    return m_snap_axes == nullptr || m_snap_axes->GetValue();
}

bool GCodeSketchPanel::constraints() const
{
    return m_constraints == nullptr || m_constraints->GetValue();
}

bool GCodeSketchPanel::has_anchor() const
{
    switch (m_tool) {
    case SketchTool::Select:
    case SketchTool::Point:
    case SketchTool::CircleTangent2:
    case SketchTool::CircleTangent3:
    case SketchTool::Text:
    case SketchTool::CircularPattern:
    case SketchTool::Offset:
    case SketchTool::Fillet:
    case SketchTool::ChamferEqual:
    case SketchTool::ChamferDistanceAngle:
    case SketchTool::ChamferTwoDistances:
    case SketchTool::Trim:
    case SketchTool::Extend:
    case SketchTool::Break: return false;
    // From the first point of the mirror line, the middle of the objects, the base point, the center.
    case SketchTool::Mirror: return m_objects_done && m_clicks.size() == 1;
    case SketchTool::RectangularPattern: return m_objects_done && !m_objects.empty();
    case SketchTool::Scale:
    case SketchTool::Move:
    case SketchTool::Copy:
    case SketchTool::Rotate: return m_objects_done && !m_clicks.empty();
    case SketchTool::Line: return m_chain;
    // The end of an arc is measured from its start; its third point lies on the arc.
    case SketchTool::Arc: return m_clicks.size() == 1;
    case SketchTool::SlotArc3Points: return !m_clicks.empty() && m_clicks.size() != 2;
    default: return !m_clicks.empty();
    }
}

Vec3d GCodeSketchPanel::anchor() const
{
    if (m_tool == SketchTool::Line)
        return m_chain_count > 0 ? m_drawing.last_point() : m_chain_start;
    // The copies are spaced from the middle of the objects.
    if (m_tool == SketchTool::RectangularPattern)
        return objects_center();
    if (m_clicks.empty())
        return m_drawing.last_point();
    switch (m_tool) {
    // Measured from the center.
    case SketchTool::RectangleCenter:
    case SketchTool::Circle:
    case SketchTool::ArcCenter:
    case SketchTool::PolygonCircumscribed:
    case SketchTool::PolygonInscribed:
    case SketchTool::Ellipse:
    case SketchTool::SlotCenter:
    case SketchTool::SlotArcCenter:
    // From the first point of the mirror line, the base point, the center of the rotation.
    case SketchTool::Mirror:
    case SketchTool::Scale:
    case SketchTool::Move:
    case SketchTool::Copy:
    case SketchTool::Rotate: return m_clicks.front();
    default: return m_clicks.back();
    }
}

size_t GCodeSketchPanel::clicks_needed() const
{
    switch (m_tool) {
    case SketchTool::Select:
    case SketchTool::Line:
    case SketchTool::Spline:
    // Lines picked, no point.
    case SketchTool::CircleTangent3:
    // Corners and curves picked.
    case SketchTool::Fillet:
    case SketchTool::ChamferEqual:
    case SketchTool::ChamferDistanceAngle:
    case SketchTool::ChamferTwoDistances:
    case SketchTool::Trim:
    case SketchTool::Extend:
    case SketchTool::Break: return 0;
    // Its center, once its lines are picked.
    case SketchTool::CircleTangent2:
    case SketchTool::Point:
    // Its start; the click placing the copies, the offset.
    case SketchTool::Text:
    case SketchTool::RectangularPattern:
    case SketchTool::Offset: return 1;
    case SketchTool::Rectangle:
    case SketchTool::RectangleCenter:
    case SketchTool::Circle:
    case SketchTool::Circle2Points:
    case SketchTool::ArcTangent:
    case SketchTool::PolygonCircumscribed:
    case SketchTool::PolygonInscribed:
    // The mirror line; the center then the click placing the copies; the base point then the factor or where it goes.
    case SketchTool::Mirror:
    case SketchTool::CircularPattern:
    case SketchTool::Scale:
    case SketchTool::Move:
    case SketchTool::Copy: return 2;
    case SketchTool::SlotArc3Points:
    case SketchTool::SlotArcCenter: return 4;
    default: return 3;
    }
}

bool GCodeSketchPanel::flat_tool() const
{
    switch (m_tool) {
    case SketchTool::Rectangle:
    case SketchTool::Rectangle3Points:
    case SketchTool::RectangleCenter:
    case SketchTool::Circle:
    case SketchTool::Circle2Points:
    case SketchTool::Circle3Points:
    case SketchTool::CircleTangent2:
    case SketchTool::CircleTangent3:
    case SketchTool::ArcCenter:
    case SketchTool::PolygonCircumscribed:
    case SketchTool::PolygonInscribed:
    case SketchTool::PolygonEdge:
    case SketchTool::Ellipse:
    case SketchTool::SlotCenters:
    case SketchTool::SlotOverall:
    case SketchTool::SlotCenter:
    case SketchTool::SlotArc3Points:
    case SketchTool::SlotArcCenter: return true;
    default: return false;
    }
}

bool GCodeSketchPanel::picking() const
{
    return (m_tool == SketchTool::CircleTangent2 && m_picks.size() < 2) || (m_tool == SketchTool::CircleTangent3 && m_picks.size() < 3);
}

SketchInput GCodeSketchPanel::input() const
{
    switch (m_tool) {
    case SketchTool::CircleTangent2:
    case SketchTool::CircleTangent3: return picking() ? SketchInput::Line : SketchInput::Point;
    case SketchTool::Mirror:
    case SketchTool::CircularPattern:
    case SketchTool::RectangularPattern:
    case SketchTool::Scale:
    case SketchTool::Move:
    case SketchTool::Copy:
    case SketchTool::Rotate:
        if (!m_objects_done)
            return SketchInput::Object;
        return m_tool == SketchTool::Mirror && m_clicks.empty() ? SketchInput::MirrorLine : SketchInput::Point;
    case SketchTool::Offset: return m_objects_done ? SketchInput::Point : SketchInput::Path;
    case SketchTool::Fillet:
    case SketchTool::ChamferEqual:
    case SketchTool::ChamferDistanceAngle:
    case SketchTool::ChamferTwoDistances: return SketchInput::Corner;
    case SketchTool::Trim:
    case SketchTool::Extend:
    case SketchTool::Break: return SketchInput::Curve;
    default: return SketchInput::Point;
    }
}

bool GCodeSketchPanel::takes_objects(SketchTool tool)
{
    switch (tool) {
    case SketchTool::Mirror:
    case SketchTool::CircularPattern:
    case SketchTool::RectangularPattern:
    case SketchTool::Scale:
    case SketchTool::Move:
    case SketchTool::Copy:
    case SketchTool::Rotate: return true;
    default: return false;
    }
}

bool GCodeSketchPanel::modify_tool(SketchTool tool)
{
    switch (tool) {
    case SketchTool::Text:
    case SketchTool::Fillet:
    case SketchTool::ChamferEqual:
    case SketchTool::ChamferDistanceAngle:
    case SketchTool::ChamferTwoDistances:
    case SketchTool::Offset:
    case SketchTool::Trim:
    case SketchTool::Extend:
    case SketchTool::Break: return true;
    default: return takes_objects(tool);
    }
}

std::vector<std::pair<Vec3d, Vec3d>> GCodeSketchPanel::picked_lines() const
{
    std::vector<std::pair<Vec3d, Vec3d>> lines;
    for (const Pick &picked : m_picks)
        lines.emplace_back(picked.a, picked.b);
    return lines;
}

bool GCodeSketchPanel::extrude_shown() const
{
    const GCodeSketch::Segment *segment = first_selected();
    return segment != nullptr ? segment->extrude : m_extrude;
}

void GCodeSketchPanel::set_polygon_sides(int sides)
{
    const int value = std::clamp(sides, 3, MAX_SIDES);
    if (value == m_polygon_sides)
        return;
    m_polygon_sides = value;
    // The hint shows it.
    update_hint();
}

void GCodeSketchPanel::set_conic_rho(double rho)
{
    const double value = std::clamp(rho, 0.01, 0.99);
    if (std::abs(value - m_conic_rho) < 1e-9)
        return;
    m_conic_rho = value;
    update_hint();
}

bool GCodeSketchPanel::sweeping() const
{
    return ((m_tool == SketchTool::ArcCenter || m_tool == SketchTool::SlotArcCenter) && m_clicks.size() == 2) ||
           (m_tool == SketchTool::Rotate && m_objects_done && m_clicks.size() == 2);
}

void GCodeSketchPanel::track_sweep(const Vec3d &cursor)
{
    if (!sweeping())
        return;
    Vec3d n, u, v;
    plane_frame(n, u, v);
    Vec3d from = m_clicks[1] - m_clicks[0];
    Vec3d to   = cursor - m_clicks[0];
    from -= from.dot(n) * n;
    to -= to.dot(n) * n;
    if (from.norm() < EPSILON || to.norm() < EPSILON)
        return;
    const double angle = std::atan2(from.cross(to).dot(n), from.dot(to));
    if (!m_sweep_valid) {
        m_sweep       = angle;
        m_sweep_valid = true;
        return;
    }
    // The turn closest to the previous angle: the mouse went on turning, more than half a turn if it did.
    const double turns = std::round((m_sweep - angle) / (2. * PI));
    m_sweep            = std::clamp(angle + 2. * PI * turns, -2. * PI, 2. * PI);
}

void GCodeSketchPanel::set_sweep(double angle)
{
    m_sweep       = std::clamp(angle, -2. * PI, 2. * PI);
    m_sweep_valid = true;
}

void GCodeSketchPanel::plane_frame(Vec3d &normal, Vec3d &u, Vec3d &v) const
{
    const int axis = plane_axis();
    u              = Vec3d::Unit(axis == 0 ? 1 : 0);
    v              = Vec3d::Unit(axis == 2 ? 1 : 2);
    // From u toward v, as the angles shown: counterclockwise around this normal.
    normal = u.cross(v);
}

bool GCodeSketchPanel::shape_path(const std::vector<Vec3d> &p, GCodeSketch::Path &path) const
{
    path = GCodeSketch::Path();
    if (p.size() < clicks_needed())
        return false;
    Vec3d n, u, v;
    plane_frame(n, u, v);
    // Distance from q to the line through a and b.
    auto line_distance = [](const Vec3d &q, const Vec3d &a, const Vec3d &b) {
        const Vec3d d = b - a;
        return d.norm() < EPSILON ? (q - a).norm() : (q - a).cross(d).norm() / d.norm();
    };
    switch (m_tool) {
    case SketchTool::Arc: {
        // Its start, its end, then a point of it, not on its ends.
        if ((p[2] - p[0]).norm() < EPSILON || (p[2] - p[1]).norm() < EPSILON)
            return false;
        GCodeSketch::Segment segment;
        segment.end = p[1];
        segment.mid = p[2];
        // 3 aligned points: a straight line.
        segment.type = GCodeSketch::arc_through(p[0], p[2], p[1]).valid ? GCodeSketch::SegmentType::Arc : GCodeSketch::SegmentType::Line;
        path.start   = p[0];
        path.segments.push_back(segment);
        break;
    }
    case SketchTool::Rectangle:
        path.start    = p[0];
        path.segments = GCodeSketch::rectangle_segments(p[0], p[1], u, v);
        break;
    case SketchTool::Rectangle3Points: path = GCodeSketch::rectangle_3_points(p[0], p[1], p[2], n); break;
    case SketchTool::RectangleCenter: path = GCodeSketch::center_rectangle(p[0], p[1], u, v); break;
    case SketchTool::Circle: path = GCodeSketch::circle(p[0], p[1], n); break;
    case SketchTool::Circle2Points: path = GCodeSketch::circle_2_points(p[0], p[1], n); break;
    case SketchTool::Circle3Points: path = GCodeSketch::circle_3_points(p[0], p[1], p[2]); break;
    case SketchTool::CircleTangent2: {
        Vec3d  center;
        double radius = 0.;
        if (m_picks.size() < 2 ||
            !GCodeSketch::circle_tangent_2(m_picks[0].a, m_picks[0].b, m_picks[1].a, m_picks[1].b, p[0], 0., n, center, radius))
            return false;
        path = GCodeSketch::circle(center, Vec3d(center + radius * u), n);
        break;
    }
    case SketchTool::CircleTangent3: {
        if (m_picks.size() < 3)
            return false;
        std::array<Vec3d, 3> a, b, at;
        for (size_t i = 0; i < 3; ++ i) {
            a[i]  = m_picks[i].a;
            b[i]  = m_picks[i].b;
            at[i] = m_picks[i].at;
        }
        Vec3d  center;
        double radius = 0.;
        if (!GCodeSketch::circle_tangent_3(a, b, at, n, center, radius))
            return false;
        path = GCodeSketch::circle(center, Vec3d(center + radius * u), n);
        break;
    }
    case SketchTool::ArcCenter: path = GCodeSketch::center_arc(p[0], p[1], m_sweep, n); break;
    case SketchTool::ArcTangent: {
        if ((p[1] - p[0]).norm() < EPSILON)
            return false;
        GCodeSketch::Segment segment;
        segment.end = p[1];
        Vec3d middle;
        // Along the tangent: a straight line.
        if (GCodeSketch::tangent_arc_middle(p[0], m_tangent, p[1], middle) && GCodeSketch::arc_through(p[0], middle, p[1]).valid) {
            segment.type = GCodeSketch::SegmentType::Arc;
            segment.mid  = middle;
        }
        path.start = p[0];
        path.segments.push_back(segment);
        break;
    }
    case SketchTool::PolygonCircumscribed:
    case SketchTool::PolygonInscribed:
        path = GCodeSketch::polygon(p[0], p[1], m_polygon_sides, m_tool == SketchTool::PolygonInscribed, n);
        break;
    case SketchTool::PolygonEdge: path = GCodeSketch::edge_polygon(p[0], p[1], p[2], m_polygon_sides, n); break;
    case SketchTool::Ellipse: path = GCodeSketch::ellipse(p[0], p[1], p[2], n, CURVE_TOLERANCE); break;
    case SketchTool::SlotCenters: path = GCodeSketch::slot(p[0], p[1], 2. * line_distance(p[2], p[0], p[1]), n); break;
    case SketchTool::SlotOverall: {
        // From its ends: the centers of its half circles are half its width inside.
        const Vec3d  axis   = p[1] - p[0];
        const double length = axis.norm();
        if (length < EPSILON)
            return false;
        const double width = 2. * line_distance(p[2], p[0], p[1]);
        const double inset = std::min(0.5 * width, 0.5 * length);
        path = GCodeSketch::slot(Vec3d(p[0] + (inset / length) * axis), Vec3d(p[1] - (inset / length) * axis), width, n);
        break;
    }
    case SketchTool::SlotCenter: path = GCodeSketch::slot(Vec3d(2. * p[0] - p[1]), p[1], 2. * line_distance(p[2], p[0], p[1]), n); break;
    case SketchTool::SlotArc3Points: {
        // The centers of its ends, a point of its arc, then its width: twice the distance from the arc.
        const GCodeSketch::Arc arc = GCodeSketch::arc_through(p[0], p[2], p[1]);
        if (!arc.valid)
            return false;
        path = GCodeSketch::arc_slot(arc, 2. * std::abs((p[3] - arc.center).norm() - arc.radius));
        break;
    }
    case SketchTool::SlotArcCenter: {
        const GCodeSketch::Arc arc = GCodeSketch::arc_around(p[0], p[1], m_sweep, n);
        if (!arc.valid)
            return false;
        path = GCodeSketch::arc_slot(arc, 2. * std::abs((p[3] - arc.center).norm() - arc.radius));
        break;
    }
    case SketchTool::Conic: path = GCodeSketch::conic(p[0], p[1], p[2], m_conic_rho, CURVE_TOLERANCE); break;
    default: return false;
    }
    return !path.empty();
}

void GCodeSketchPanel::shape_preview(const Vec3d &cursor, GCodeSketch::Path &path, std::vector<std::pair<Vec3d, Vec3d>> &guides) const
{
    path = GCodeSketch::Path();
    guides.clear();
    std::vector<Vec3d> points = m_clicks;
    points.push_back(cursor);
    Vec3d n, u, v;
    plane_frame(n, u, v);
    // Construction line along an arc.
    auto arc_guide = [&guides](const GCodeSketch::Arc &arc) {
        if (!arc.valid)
            return;
        Vec3d previous = arc.start;
        for (const Vec3d &point : GCodeSketch::discretize_arc(arc, 0.05, 256)) {
            guides.emplace_back(previous, point);
            previous = point;
        }
    };
    switch (m_tool) {
    case SketchTool::Select:
    case SketchTool::Line:
    case SketchTool::Point: return;
    case SketchTool::Spline:
        if (!m_clicks.empty())
            path = GCodeSketch::spline(points, false, CURVE_TOLERANCE);
        return;
    case SketchTool::CircleTangent2:
    case SketchTool::CircleTangent3:
        if (!picking())
            shape_path(points, path);
        return;
    default: break;
    }
    if (m_clicks.empty())
        return;
    const Vec3d &first = m_clicks.front();
    if (points.size() >= clicks_needed()) {
        // The shape, the cursor as its last point.
        shape_path(points, path);
        if (m_tool == SketchTool::ArcCenter && !path.empty()) {
            guides.emplace_back(first, path.start);
            guides.emplace_back(first, path.segments.back().end);
        } else if (m_tool == SketchTool::SlotArcCenter)
            arc_guide(GCodeSketch::arc_around(first, m_clicks[1], m_sweep, n));
        else if (m_tool == SketchTool::SlotArc3Points)
            arc_guide(GCodeSketch::arc_through(first, m_clicks[2], m_clicks[1]));
        return;
    }
    // Not complete yet: what is known of it.
    auto line = [&path](const Vec3d &a, const Vec3d &b) {
        path.start = a;
        GCodeSketch::Segment segment;
        segment.end = b;
        path.segments.push_back(segment);
    };
    switch (m_tool) {
    case SketchTool::Arc:
    case SketchTool::Rectangle3Points:
    case SketchTool::PolygonEdge:
    case SketchTool::Conic: line(first, cursor); break;
    case SketchTool::Circle3Points:
    case SketchTool::ArcCenter:
    case SketchTool::SlotCenters:
    case SketchTool::SlotOverall: guides.emplace_back(first, cursor); break;
    // Both halves of the axis.
    case SketchTool::Ellipse:
    case SketchTool::SlotCenter: guides.emplace_back(Vec3d(2. * first - cursor), cursor); break;
    case SketchTool::SlotArc3Points:
        if (m_clicks.size() == 1)
            guides.emplace_back(first, cursor);
        else
            arc_guide(GCodeSketch::arc_through(first, cursor, m_clicks[1]));
        break;
    case SketchTool::SlotArcCenter:
        if (m_clicks.size() == 1)
            guides.emplace_back(first, cursor);
        else {
            const GCodeSketch::Arc arc = GCodeSketch::arc_around(first, m_clicks[1], m_sweep, n);
            arc_guide(arc);
            guides.emplace_back(first, m_clicks[1]);
            if (arc.valid)
                guides.emplace_back(first, arc.end);
        }
        break;
    default: break;
    }
}

void GCodeSketchPanel::preview(const Vec3d *cursor, int hover, const Vec3d &at, SketchPreview &out) const
{
    out = SketchPreview();
    Vec3d n, u, v;
    plane_frame(n, u, v);
    if (!modify_tool(m_tool)) {
        // The shape of a create tool.
        if (cursor == nullptr)
            return;
        GCodeSketch::Path path;
        shape_preview(*cursor, path, out.guides);
        if (!path.empty())
            out.paths.push_back(std::move(path));
        return;
    }
    const bool   on_segment = hover >= 0 && size_t(hover) < m_drawing.segments.size();
    const size_t segment    = on_segment ? size_t(hover) : 0;
    // Corners and curves of the printed segments only.
    const bool   printed    = on_segment && m_drawing.segments[segment].extrude;
    switch (m_tool) {
    case SketchTool::Text:
        if (cursor != nullptr)
            out.paths = text_paths(*cursor);
        break;
    case SketchTool::Mirror: {
        if (!m_objects_done)
            break;
        Vec3d a     = Vec3d::Zero();
        Vec3d along = Vec3d::Zero();
        if (m_clicks.empty() && on_segment && m_drawing.segments[segment].type == GCodeSketch::SegmentType::Line) {
            // The line under the mouse.
            a     = m_drawing.segment_start(segment);
            along = m_drawing.segments[segment].end - a;
        } else if (!m_clicks.empty() && cursor != nullptr) {
            a     = m_clicks.front();
            along = *cursor - a;
        }
        along -= along.dot(n) * n;
        if (along.norm() < EPSILON)
            break;
        out.paths = object_copies({ GCodeSketch::mirror_transformation(a, along, n) });
        // The mirror line, across the bed.
        Vec2d min, max;
        bed_box(m_machine, min, max);
        const double reach = (max - min).norm();
        out.guides.emplace_back(Vec3d(a - reach * along.normalized()), Vec3d(a + reach * along.normalized()));
        break;
    }
    case SketchTool::CircularPattern:
        if (m_objects_done && (!m_clicks.empty() || cursor != nullptr)) {
            const Vec3d center = m_clicks.empty() ? *cursor : m_clicks.front();
            out.paths          = object_copies(circular_transformations(center));
        }
        break;
    case SketchTool::RectangularPattern:
        if (m_objects_done)
            out.paths = object_copies(rectangular_transformations());
        break;
    case SketchTool::Offset:
        if (m_objects_done && !m_objects.empty()) {
            GCodeSketch::Path path = GCodeSketch::offset_path(m_drawing, m_objects.front(), m_objects.back(), m_offset_distance, n);
            if (!path.empty())
                out.paths.push_back(std::move(path));
        }
        break;
    case SketchTool::Scale:
        if (m_objects_done && !m_clicks.empty() && cursor != nullptr) {
            out.paths = object_copies({ GCodeSketch::scale_in_plane(m_clicks.front(), m_scale_factor, n) });
            out.guides.emplace_back(m_clicks.front(), *cursor);
        }
        break;
    case SketchTool::Move:
    case SketchTool::Copy:
        if (m_objects_done && !m_clicks.empty() && cursor != nullptr) {
            Transform3d move = Transform3d::Identity();
            move.translate(*cursor - m_clicks.front());
            out.paths = object_copies({ move });
            out.guides.emplace_back(m_clicks.front(), *cursor);
        }
        break;
    case SketchTool::Rotate:
        if (!m_objects_done || m_clicks.empty() || cursor == nullptr)
            break;
        out.guides.emplace_back(m_clicks.front(), m_clicks.size() >= 2 ? m_clicks[1] : *cursor);
        if (m_clicks.size() >= 2) {
            out.guides.emplace_back(m_clicks.front(), *cursor);
            out.paths = object_copies({ GCodeSketch::rotation_around(m_clicks.front(), m_sweep, n) });
        }
        break;
    case SketchTool::Fillet:
    case SketchTool::ChamferEqual:
    case SketchTool::ChamferDistanceAngle:
    case SketchTool::ChamferTwoDistances: {
        GCodeSketch::Corner corner;
        GCodeSketch::Path   shape;
        if (!printed || !GCodeSketch::find_corner(m_drawing, segment, at, corner) || !corner_shape(corner, shape))
            break;
        // What it cuts off the two segments.
        GCodeSketch::Path removed;
        removed.start = shape.start;
        removed.segments.push_back(GCodeSketch::segment_part(m_drawing, corner.in, GCodeSketch::segment_fraction(m_drawing, corner.in, shape.start), 1.));
        removed.segments.push_back(
            GCodeSketch::segment_part(m_drawing, corner.out, 0., GCodeSketch::segment_fraction(m_drawing, corner.out, shape.segments.back().end)));
        out.removed.push_back(std::move(removed));
        out.paths.push_back(std::move(shape));
        break;
    }
    case SketchTool::Trim: {
        GCodeSketch::Path removed;
        if (printed && GCodeSketch::trim_part(m_drawing, segment, at, n, removed))
            out.removed.push_back(std::move(removed));
        break;
    }
    case SketchTool::Extend: {
        GCodeSketch::Path extension;
        if (printed && GCodeSketch::extend_part(m_drawing, segment, at, n, extension))
            out.paths.push_back(std::move(extension));
        break;
    }
    case SketchTool::Break:
        if (printed)
            GCodeSketch::break_points(m_drawing, segment, at, n, out.marks);
        break;
    default: break;
    }
}

void GCodeSketchPanel::add_path(const GCodeSketch::Path &path)
{
    if (path.empty())
        return;
    snapshot();
    // A travel move links the shape to the drawing.
    move_pen_to(path.start);
    for (GCodeSketch::Segment segment : path.segments) {
        segment.extrude = m_extrude;
        m_drawing.segments.push_back(segment);
    }
}

bool GCodeSketchPanel::add_spline(bool closed)
{
    if (m_tool != SketchTool::Spline || m_clicks.size() < 2)
        return false;
    const GCodeSketch::Path path = GCodeSketch::spline(m_clicks, closed, CURVE_TOLERANCE);
    m_clicks.clear();
    if (path.empty())
        return false;
    add_path(path);
    return true;
}

bool GCodeSketchPanel::tangent_at(const Vec3d &point, Vec3d &tangent) const
{
    const std::vector<GCodeSketch::Segment> &segments = m_drawing.segments;
    if (!m_drawing.has_start)
        return false;
    // A segment ending there: the arc goes on along it; the printed ones first, the last drawn first.
    for (int pass = 0; pass < 2; ++ pass)
        for (size_t i = segments.size(); i -- > 0;)
            if ((segments[i].extrude || pass == 1) && (segments[i].end - point).norm() < EPSILON) {
                tangent = GCodeSketch::end_tangent(m_drawing, i);
                if (tangent.norm() > 0.5)
                    return true;
            }
    // A segment starting there: the arc leaves it backward.
    for (int pass = 0; pass < 2; ++ pass)
        for (size_t i = 0; i < segments.size(); ++ i)
            if ((segments[i].extrude || pass == 1) && (m_drawing.segment_start(i) - point).norm() < EPSILON) {
                tangent = -GCodeSketch::start_tangent(m_drawing, i);
                if (tangent.norm() > 0.5)
                    return true;
            }
    return false;
}

Vec3d GCodeSketchPanel::chain_tangent() const
{
    if (!chain_has_segments() || m_drawing.segments.empty())
        return Vec3d::Zero();
    return GCodeSketch::end_tangent(m_drawing, m_drawing.segments.size() - 1);
}

void GCodeSketchPanel::run_at(size_t idx, size_t &first, size_t &last) const
{
    first = idx;
    last  = idx;
    if (idx < m_drawing.segments.size() && GCodeSketch::find_shape(m_drawing, idx, first, last) == GCodeSketch::ShapeType::None) {
        first = idx;
        last  = idx;
    }
}

std::vector<double> GCodeSketchPanel::plane_marks() const
{
    std::vector<double> marks;
    const int           axis = plane_axis();
    for (const GCodeSketch::PointRef &ref : GCodeSketch::list_points(m_drawing))
        if (const Vec3d *p = GCodeSketch::point_position(m_drawing, ref))
            marks.push_back((*p)[axis]);
    std::sort(marks.begin(), marks.end());
    marks.erase(std::unique(marks.begin(), marks.end(), [](double a, double b) { return std::abs(a - b) < EPSILON; }), marks.end());
    return marks;
}

void GCodeSketchPanel::snapshot()
{
    m_undo.push_back(m_drawing);
    m_redo.clear();
    trim_undo();
}

void GCodeSketchPanel::begin_change()
{
    m_undo.push_back(m_drawing);
    m_change_redo = std::move(m_redo);
    m_redo.clear();
}

void GCodeSketchPanel::end_change()
{
    if (!m_undo.empty() && m_undo.back() == m_drawing) {
        // Nothing changed: no undo step, and the redo steps come back.
        m_undo.pop_back();
        m_redo = std::move(m_change_redo);
    } else
        trim_undo();
    m_change_redo.clear();
}

void GCodeSketchPanel::trim_undo()
{
    const size_t max_undo = m_drawing.segments.size() > LARGE_DRAWING ? MAX_UNDO_LARGE : MAX_UNDO;
    if (m_undo.size() > max_undo)
        m_undo.erase(m_undo.begin(), m_undo.begin() + (m_undo.size() - max_undo));
}

void GCodeSketchPanel::move_pen_to(const Vec3d &point)
{
    if (!m_drawing.has_start) {
        m_drawing.has_start = true;
        m_drawing.start     = point;
    } else if ((point - m_drawing.last_point()).norm() >= EPSILON) {
        GCodeSketch::Segment travel;
        travel.extrude = false;
        travel.end     = point;
        m_drawing.segments.push_back(travel);
    }
}

void GCodeSketchPanel::follow(const Vec3d &point)
{
    set_plane_offset(point[plane_axis()]);
}

void GCodeSketchPanel::next_step()
{
    ++ m_step;
}

void GCodeSketchPanel::click(const Vec3d &point)
{
    if (modify_tool(m_tool)) {
        click_modify(point);
        return;
    }
    bool added = false;
    switch (m_tool) {
    // The tangent circles pick lines (pick()).
    case SketchTool::Select:
    case SketchTool::CircleTangent3: return;
    case SketchTool::Line: {
        if (!m_chain) {
            // Start of a line: nothing is drawn until its first segment.
            m_chain       = true;
            m_chain_start = point;
            m_chain_count = 0;
            break;
        }
        if ((point - anchor()).norm() < EPSILON)
            return;
        snapshot();
        // Its first segment: a travel move links the line to the drawing.
        if (m_chain_count == 0)
            move_pen_to(m_chain_start);
        GCodeSketch::Segment segment;
        segment.extrude = m_extrude;
        segment.end     = point;
        m_drawing.segments.push_back(segment);
        ++ m_chain_count;
        added = true;
        // Back on its start: the outline is closed, the next click starts another line.
        if (m_chain_count >= 2 && (point - m_chain_start).norm() < EPSILON)
            m_chain = false;
        break;
    }
    case SketchTool::Point:
        // The nozzle goes there: a travel move, or the start of the drawing.
        begin_change();
        move_pen_to(point);
        end_change();
        added = true;
        break;
    case SketchTool::Spline:
        if (!m_clicks.empty() && (point - m_clicks.back()).norm() < EPSILON)
            return;
        // Back on its first point: closed.
        if (m_clicks.size() >= 2 && (point - m_clicks.front()).norm() < EPSILON) {
            added = add_spline(true);
            break;
        }
        m_clicks.push_back(point);
        break;
    case SketchTool::ArcTangent:
        if (m_clicks.empty()) {
            // From the end of a line or of an arc, along it.
            if (!tangent_at(point, m_tangent)) {
                m_hint_text->SetLabel(_L("No line nor arc ends there: click the end of one, the arc goes on along it."));
                return;
            }
            m_clicks.push_back(point);
            break;
        }
        [[fallthrough]];
    default: {
        // The other shapes: their points, then the shape.
        if (picking())
            return;
        // The end of a center point arc gives the angle it turns: on its start, a full turn.
        if (!m_clicks.empty() && !sweeping() && (point - m_clicks.back()).norm() < EPSILON)
            return;
        m_clicks.push_back(point);
        // The angle of a center point arc follows the mouse from its start.
        if (m_clicks.size() == 2 && (m_tool == SketchTool::ArcCenter || m_tool == SketchTool::SlotArcCenter))
            m_sweep_valid = false;
        // The arc of an arc slot exists, less than a full turn, before its width is asked: otherwise another click.
        if (m_clicks.size() == 3 && (m_tool == SketchTool::SlotArc3Points || m_tool == SketchTool::SlotArcCenter)) {
            Vec3d n, u, v;
            plane_frame(n, u, v);
            const GCodeSketch::Arc arc = m_tool == SketchTool::SlotArc3Points ? GCodeSketch::arc_through(m_clicks[0], m_clicks[2], m_clicks[1]) :
                                                                                GCodeSketch::arc_around(m_clicks[0], m_clicks[1], m_sweep, n);
            if (!arc.valid || arc.angle > 2. * PI - 1e-6) {
                m_clicks.pop_back();
                return;
            }
        }
        if (m_clicks.size() < clicks_needed())
            break;
        GCodeSketch::Path path;
        if (!shape_path(m_clicks, path)) {
            // Flat, or points merged: wait for another click.
            m_clicks.pop_back();
            return;
        }
        add_path(path);
        m_clicks.clear();
        m_picks.clear();
        added = true;
        break;
    }
    }
    // The working plane follows the drawing.
    follow(added && m_drawing.has_start ? m_drawing.last_point() : point);
    next_step();
    if (added)
        on_drawing_changed();
    on_tool_changed();
}

void GCodeSketchPanel::click_modify(const Vec3d &point)
{
    Vec3d n, u, v;
    plane_frame(n, u, v);
    bool     changed = false;
    wxString refusal;
    switch (m_tool) {
    case SketchTool::Text:
        m_text_point = point;
        // After this event: the dialog would keep the mouse of the view.
        CallAfter([this]() { place_text(); });
        return;
    case SketchTool::Mirror:
        if (!m_objects_done)
            return;
        if (m_clicks.empty()) {
            m_clicks.push_back(point);
            break;
        }
        {
            // In the plane: a point above the first one gives no line.
            Vec3d along = point - m_clicks.front();
            along -= along.dot(n) * n;
            if (along.norm() < EPSILON)
                return;
            changed = add_copies({ GCodeSketch::mirror_transformation(m_clicks.front(), along, n) });
        }
        restart_objects();
        break;
    case SketchTool::CircularPattern:
        if (!m_objects_done)
            return;
        if (m_clicks.empty()) {
            m_clicks.push_back(point);
            break;
        }
        changed = add_copies(circular_transformations(m_clicks.front()));
        restart_objects();
        break;
    case SketchTool::RectangularPattern:
    case SketchTool::Offset:
        if (!m_objects_done)
            return;
        changed = apply_ready();
        if (!changed && m_tool == SketchTool::Offset)
            refusal = _L("No offset: the path lies out of the working plane (Alt: next plane), or this distance makes it vanish.");
        restart_objects();
        break;
    case SketchTool::Scale:
        if (!m_objects_done)
            return;
        if (m_clicks.empty()) {
            m_clicks.push_back(point);
            break;
        }
        changed = apply_ready();
        restart_objects();
        break;
    case SketchTool::Move:
    case SketchTool::Copy: {
        if (!m_objects_done)
            return;
        if (m_clicks.empty()) {
            m_clicks.push_back(point);
            break;
        }
        // Not moved: nothing to do.
        if ((point - m_clicks.front()).norm() < EPSILON)
            return;
        Transform3d move = Transform3d::Identity();
        move.translate(point - m_clicks.front());
        if (m_tool == SketchTool::Copy) {
            // More copies from the same base point, until Enter or Esc.
            changed = add_copies({ move });
            break;
        }
        changed = transform_objects(move);
        restart_objects();
        break;
    }
    case SketchTool::Rotate:
        if (!m_objects_done)
            return;
        if (m_clicks.size() < 2) {
            if (!m_clicks.empty() && (point - m_clicks.back()).norm() < EPSILON)
                return;
            m_clicks.push_back(point);
            // The angle follows the mouse from the point turned.
            if (m_clicks.size() == 2)
                m_sweep_valid = false;
            break;
        }
        changed = transform_objects(GCodeSketch::rotation_around(m_clicks.front(), m_sweep, n));
        restart_objects();
        break;
    default: return;
    }
    next_step();
    if (changed)
        on_drawing_changed();
    // The objects shown, or no longer.
    if (m_view != nullptr)
        m_view->on_selection_changed();
    on_tool_changed();
    // After the hint of the tool.
    if (!refusal.empty())
        m_hint_text->SetLabel(refusal);
}

void GCodeSketchPanel::pick(size_t idx, const Vec3d &at, bool double_click)
{
    if (idx >= m_drawing.segments.size())
        return;
    Vec3d n, u, v;
    plane_frame(n, u, v);
    bool     changed = false;
    wxString refusal;
    switch (input()) {
    case SketchInput::Line: pick_line(idx, at); return;
    case SketchInput::Object: toggle_object(idx, double_click); return;
    case SketchInput::Path: {
        if (!m_drawing.segments[idx].extrude) {
            refusal = _L("Click a printed path: the travel moves are not offset.");
            break;
        }
        // The closed outline holding it (a rectangle drawn after a line...), otherwise the whole path; then its distance.
        size_t first = idx;
        size_t last  = idx;
        if (!GCodeSketch::find_loop(m_drawing, idx, first, last))
            GCodeSketch::find_path(m_drawing, idx, first, last);
        m_objects.clear();
        for (size_t k = first; k <= last; ++ k)
            m_objects.push_back(k);
        m_objects_done = true;
        break;
    }
    case SketchInput::MirrorLine: {
        const Vec3d a = m_drawing.segment_start(idx);
        const Vec3d b = m_drawing.segments[idx].end;
        Vec3d       along = b - a;
        along -= along.dot(n) * n;
        if (m_drawing.segments[idx].type != GCodeSketch::SegmentType::Line || along.norm() < EPSILON) {
            refusal = _L("The mirror line is a straight line, not seen end on: click another one, or two points.");
            break;
        }
        changed = add_copies({ GCodeSketch::mirror_transformation(a, along, n) });
        restart_objects();
        break;
    }
    case SketchInput::Curve:
        if (!m_drawing.segments[idx].extrude) {
            refusal = _L("Click a printed segment: the travel moves are not modified.");
            break;
        }
        changed = change_drawing([this, idx, &at, &n](GCodeSketch::Drawing &drawing) {
            switch (m_tool) {
            case SketchTool::Trim: return GCodeSketch::trim(drawing, idx, at, n);
            case SketchTool::Extend: return GCodeSketch::extend(drawing, idx, at, n);
            default: return GCodeSketch::break_curve(drawing, idx, at, n);
            }
        });
        if (!changed)
            refusal = m_tool == SketchTool::Trim ?
                          _L("Nothing to trim there: click a line or an arc lying in the working plane (Alt: next plane).") :
                      m_tool == SketchTool::Extend ?
                          _L("Nothing to extend there: click near the free end of a line or of an arc of the working plane, toward a line it "
                             "meets.") :
                          _L("Nothing to break there: the curve is already split at these points.");
        break;
    case SketchInput::Corner: {
        if (!m_drawing.segments[idx].extrude) {
            refusal = _L("Click a printed segment: the travel moves are not modified.");
            break;
        }
        GCodeSketch::Corner corner;
        changed = GCodeSketch::find_corner(m_drawing, idx, at, corner) &&
                  change_drawing([this, &corner](GCodeSketch::Drawing &drawing) { return change_corner(drawing, corner); });
        if (!changed)
            refusal = m_tool == SketchTool::Fillet ?
                          _L("No fillet there: click near a corner of a path in the working plane; the radius may be too large.") :
                          _L("No chamfer there: click near a corner between two lines of a path in the working plane; the distances may be "
                             "too long.");
        break;
    }
    default: return;
    }
    next_step();
    if (changed)
        on_drawing_changed();
    if (m_view != nullptr)
        m_view->on_selection_changed();
    on_tool_changed();
    // After the hint of the tool.
    if (!refusal.empty())
        m_hint_text->SetLabel(refusal);
}

void GCodeSketchPanel::pick_line(size_t idx, const Vec3d &at)
{
    if (!picking() || idx >= m_drawing.segments.size())
        return;
    const GCodeSketch::Segment &segment = m_drawing.segments[idx];
    const Vec3d                 a       = m_drawing.segment_start(idx);
    // Straight lines only, each one once.
    if (segment.type != GCodeSketch::SegmentType::Line || (segment.end - a).norm() < EPSILON)
        return;
    for (const Pick &picked : m_picks)
        if (picked.segment == idx)
            return;
    // Seen end on from the working plane, or on the straight line of the first one (2-tangent circle): no circle
    // touches it.
    Vec3d n, u, v;
    plane_frame(n, u, v);
    Vec3d direction = segment.end - a;
    direction -= direction.dot(n) * n;
    if (direction.norm() < EPSILON)
        return;
    if (m_tool == SketchTool::CircleTangent2 && m_picks.size() == 1) {
        Vec3d first  = m_picks[0].b - m_picks[0].a;
        Vec3d offset = a - m_picks[0].a;
        first -= first.dot(n) * n;
        offset -= offset.dot(n) * n;
        const Vec3d along = first.normalized();
        if (along.cross(direction.normalized()).norm() < 1e-6 && along.cross(offset).norm() < EPSILON)
            return;
    }
    m_picks.push_back({ idx, a, segment.end, at });
    // The circle is drawn in the plane of its lines.
    follow(at);
    if (m_tool == SketchTool::CircleTangent3 && m_picks.size() == 3) {
        GCodeSketch::Path path;
        if (shape_path({}, path)) {
            add_path(path);
            m_picks.clear();
            follow(m_drawing.last_point());
            next_step();
            on_drawing_changed();
            on_tool_changed();
            return;
        }
        // No circle touches them: another third line.
        m_picks.pop_back();
    }
    next_step();
    on_tool_changed();
}

void GCodeSketchPanel::toggle_object(size_t idx, bool whole_path)
{
    if (idx >= m_drawing.segments.size())
        return;
    size_t first = idx;
    size_t last  = idx;
    if (whole_path)
        GCodeSketch::find_path(m_drawing, idx, first, last);
    else
        run_at(idx, first, last);
    bool all = true;
    for (size_t k = first; k <= last && all; ++ k)
        all = std::binary_search(m_objects.begin(), m_objects.end(), k);
    // A click on a chosen shape removes it; a double click adds the whole path.
    if (all && !whole_path)
        m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(), [first, last](size_t k) { return k >= first && k <= last; }),
                        m_objects.end());
    else {
        for (size_t k = first; k <= last; ++ k)
            m_objects.push_back(k);
        std::sort(m_objects.begin(), m_objects.end());
        m_objects.erase(std::unique(m_objects.begin(), m_objects.end()), m_objects.end());
    }
    if (m_view != nullptr)
        m_view->on_selection_changed();
    on_tool_changed();
}

void GCodeSketchPanel::restart_objects()
{
    m_objects.clear();
    m_objects_done = false;
    m_clicks.clear();
    m_sweep_valid = false;
}

std::vector<std::pair<size_t, size_t>> GCodeSketchPanel::object_ranges() const
{
    std::vector<std::pair<size_t, size_t>> ranges;
    for (const size_t idx : m_objects) {
        if (idx >= m_drawing.segments.size())
            continue;
        if (!ranges.empty() && ranges.back().second + 1 == idx)
            ranges.back().second = idx;
        else
            ranges.emplace_back(idx, idx);
    }
    return ranges;
}

Vec3d GCodeSketchPanel::objects_center() const
{
    Vec3d min = Vec3d::Constant(std::numeric_limits<double>::max());
    Vec3d max = -min;
    auto  add = [&min, &max](const Vec3d &p) {
        min = min.cwiseMin(p);
        max = max.cwiseMax(p);
    };
    for (const auto &[first, last] : object_ranges()) {
        add(m_drawing.segment_start(first));
        for (size_t idx = first; idx <= last; ++ idx) {
            const GCodeSketch::Segment &segment = m_drawing.segments[idx];
            add(segment.end);
            if (segment.type == GCodeSketch::SegmentType::Arc)
                add(segment.mid);
        }
    }
    return min.x() <= max.x() ? Vec3d(0.5 * (min + max)) : m_drawing.last_point();
}

double GCodeSketchPanel::objects_reach(const Vec3d &base) const
{
    Vec3d n, u, v;
    plane_frame(n, u, v);
    double reach = 0.;
    auto   add   = [&](const Vec3d &p) {
        Vec3d d = p - base;
        d -= d.dot(n) * n;
        reach = std::max(reach, d.norm());
    };
    for (const auto &[first, last] : object_ranges()) {
        add(m_drawing.segment_start(first));
        for (size_t idx = first; idx <= last; ++ idx) {
            const GCodeSketch::Segment &segment = m_drawing.segments[idx];
            add(segment.end);
            if (segment.type == GCodeSketch::SegmentType::Arc)
                add(segment.mid);
        }
    }
    return reach;
}

bool GCodeSketchPanel::offset_distance_at(const Vec3d &p, double &distance) const
{
    if (m_tool != SketchTool::Offset || m_objects.empty())
        return false;
    Vec3d n, u, v;
    plane_frame(n, u, v);
    return GCodeSketch::distance_to_path(m_drawing, m_objects.front(), m_objects.back(), p, n, distance);
}

void GCodeSketchPanel::set_pattern_count(int count)
{
    const int value = std::clamp(count, 2, MAX_COPIES);
    if (value == m_pattern_count)
        return;
    m_pattern_count = value;
    update_hint();
}

void GCodeSketchPanel::set_pattern_angle(double degrees)
{
    if (std::abs(degrees) < 0.1)
        return;
    const double value = std::clamp(degrees, -360., 360.);
    if (std::abs(value - m_pattern_angle) < 1e-9)
        return;
    m_pattern_angle = value;
    update_hint();
}

void GCodeSketchPanel::set_grid_count(int axis, int count)
{
    const int value = std::clamp(count, 1, MAX_COPIES);
    int      &field = m_grid_counts[axis == 0 ? 0 : 1];
    if (value == field)
        return;
    field = value;
    update_hint();
}

void GCodeSketchPanel::set_grid_spacing(double along_u, double along_v)
{
    m_grid_spacing[0] = along_u;
    m_grid_spacing[1] = along_v;
}

void GCodeSketchPanel::set_fillet_radius(double radius)
{
    if (radius < 0.01 || std::abs(radius - m_fillet_radius) < 1e-9)
        return;
    m_fillet_radius = radius;
    update_hint();
}

void GCodeSketchPanel::set_chamfer_distance(int which, double distance)
{
    double &field = m_chamfer[which == 0 ? 0 : 1];
    if (distance < 0.01 || std::abs(distance - field) < 1e-9)
        return;
    field = distance;
    update_hint();
}

void GCodeSketchPanel::set_chamfer_angle(double degrees)
{
    const double value = std::clamp(degrees, 1., 179.);
    if (std::abs(value - m_chamfer_angle) < 1e-9)
        return;
    m_chamfer_angle = value;
    update_hint();
}

void GCodeSketchPanel::set_offset_distance(double distance)
{
    m_offset_distance = distance;
}

void GCodeSketchPanel::set_scale_factor(double factor)
{
    if (factor < 1e-3 || std::abs(factor - m_scale_factor) < 1e-9)
        return;
    m_scale_factor = factor;
    update_hint();
}

std::vector<GCodeSketch::Path> GCodeSketchPanel::object_copies(const std::vector<Transform3d> &transformations) const
{
    std::vector<GCodeSketch::Path>               copies;
    const std::vector<std::pair<size_t, size_t>> ranges = object_ranges();
    size_t                                       count  = 0;
    for (const auto &[first, last] : ranges)
        count += last - first + 1;
    if (count == 0 || count * transformations.size() > MAX_COPY_SEGMENTS)
        return copies;
    copies.reserve(ranges.size() * transformations.size());
    for (const Transform3d &transformation : transformations)
        for (const auto &[first, last] : ranges)
            copies.push_back(GCodeSketch::transformed_path(m_drawing, first, last, transformation));
    return copies;
}

std::vector<Transform3d> GCodeSketchPanel::circular_transformations(const Vec3d &center) const
{
    Vec3d n, u, v;
    plane_frame(n, u, v);
    std::vector<Transform3d> transformations;
    const int    count = std::max(m_pattern_count, 2);
    // All around: the copies evenly apart; otherwise the last one at the angle.
    const double step  = std::abs(m_pattern_angle) >= 360. - 1e-6 ? 2. * PI / count : m_pattern_angle * PI / 180. / (count - 1);
    for (int k = 1; k < count; ++ k)
        transformations.push_back(GCodeSketch::rotation_around(center, k * step, n));
    return transformations;
}

std::vector<Transform3d> GCodeSketchPanel::rectangular_transformations() const
{
    Vec3d n, u, v;
    plane_frame(n, u, v);
    std::vector<Transform3d> transformations;
    // Too many segments: none, before building them.
    if (size_t(m_grid_counts[0]) * size_t(m_grid_counts[1]) * std::max<size_t>(m_objects.size(), 1) > MAX_COPY_SEGMENTS)
        return transformations;
    for (int i = 0; i < m_grid_counts[0]; ++ i)
        for (int j = 0; j < m_grid_counts[1]; ++ j) {
            if (i == 0 && j == 0)
                continue;
            Transform3d move = Transform3d::Identity();
            move.translate(Vec3d(double(i) * m_grid_spacing[0] * u + double(j) * m_grid_spacing[1] * v));
            transformations.push_back(move);
        }
    return transformations;
}

bool GCodeSketchPanel::add_copies(const std::vector<Transform3d> &transformations)
{
    const std::vector<GCodeSketch::Path> copies = object_copies(transformations);
    if (copies.empty())
        return false;
    return change_drawing([&copies](GCodeSketch::Drawing &drawing) {
        for (const GCodeSketch::Path &copy : copies)
            GCodeSketch::append_path(drawing, copy);
        // No travel move at the end of the drawing.
        while (!drawing.segments.empty() && !drawing.segments.back().extrude)
            drawing.segments.pop_back();
        if (drawing.segments.empty())
            drawing.clear();
        return true;
    });
}

bool GCodeSketchPanel::transform_objects(const Transform3d &transformation)
{
    const std::vector<std::pair<size_t, size_t>> ranges = object_ranges();
    if (ranges.empty())
        return false;
    return change_drawing([&ranges, &transformation](GCodeSketch::Drawing &drawing) {
        for (const auto &[first, last] : ranges)
            GCodeSketch::transform_segments(drawing, first, last, transformation);
        return true;
    });
}

bool GCodeSketchPanel::apply_ready()
{
    if (!m_objects_done || m_objects.empty())
        return false;
    Vec3d n, u, v;
    plane_frame(n, u, v);
    switch (m_tool) {
    case SketchTool::CircularPattern: return !m_clicks.empty() && add_copies(circular_transformations(m_clicks.front()));
    case SketchTool::RectangularPattern: return add_copies(rectangular_transformations());
    case SketchTool::Offset: {
        const GCodeSketch::Path path = GCodeSketch::offset_path(m_drawing, m_objects.front(), m_objects.back(), m_offset_distance, n);
        if (path.empty())
            return false;
        snapshot();
        GCodeSketch::append_path(m_drawing, path);
        return true;
    }
    case SketchTool::Scale: return !m_clicks.empty() && transform_objects(GCodeSketch::scale_in_plane(m_clicks.front(), m_scale_factor, n));
    default: return false;
    }
}

bool GCodeSketchPanel::change_drawing(const std::function<bool(GCodeSketch::Drawing &)> &change)
{
    GCodeSketch::Drawing changed = m_drawing;
    if (!change(changed) || changed == m_drawing)
        return false;
    snapshot();
    m_drawing = std::move(changed);
    return true;
}

bool GCodeSketchPanel::corner_shape(const GCodeSketch::Corner &corner, GCodeSketch::Path &shape) const
{
    Vec3d n, u, v;
    plane_frame(n, u, v);
    switch (m_tool) {
    case SketchTool::Fillet: return GCodeSketch::fillet_shape(m_drawing, corner, m_fillet_radius, n, shape);
    case SketchTool::ChamferEqual: return GCodeSketch::chamfer_shape(m_drawing, corner, m_chamfer[0], m_chamfer[0], 0., n, shape);
    case SketchTool::ChamferDistanceAngle:
        return GCodeSketch::chamfer_shape(m_drawing, corner, m_chamfer[0], 0., m_chamfer_angle * PI / 180., n, shape);
    case SketchTool::ChamferTwoDistances: return GCodeSketch::chamfer_shape(m_drawing, corner, m_chamfer[0], m_chamfer[1], 0., n, shape);
    default: return false;
    }
}

bool GCodeSketchPanel::change_corner(GCodeSketch::Drawing &drawing, const GCodeSketch::Corner &corner) const
{
    Vec3d n, u, v;
    plane_frame(n, u, v);
    switch (m_tool) {
    case SketchTool::Fillet: return GCodeSketch::fillet(drawing, corner, m_fillet_radius, n);
    case SketchTool::ChamferEqual: return GCodeSketch::chamfer(drawing, corner, m_chamfer[0], m_chamfer[0], 0., n);
    case SketchTool::ChamferDistanceAngle: return GCodeSketch::chamfer(drawing, corner, m_chamfer[0], 0., m_chamfer_angle * PI / 180., n);
    case SketchTool::ChamferTwoDistances: return GCodeSketch::chamfer(drawing, corner, m_chamfer[0], m_chamfer[1], 0., n);
    default: return false;
    }
}

bool GCodeSketchPanel::text_outlines(const TextSettings &settings, std::vector<std::vector<Vec2d>> &outlines, wxString &error) const
{
    outlines.clear();
    const wxString face = settings.font.empty() ? wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).GetFaceName() : settings.font;
    const wxFont   font(wxFontInfo(48).FaceName(face).Bold(settings.bold).Italic(settings.italic));
    std::unique_ptr<::Slic3r::Emboss::FontFile> file;
    if (font.IsOk())
        file = WxFontUtils::create_font_file(font);
    if (file == nullptr || file->infos.empty()) {
        error = wxString::Format(_L("The font %s cannot be read: choose another one."), face);
        return false;
    }
    const ::Slic3r::Emboss::FontFile::Info &info = file->infos.front();
    // Height of the capitals in units of the font: the one of the H, otherwise most of the ascent.
    double capital = 0.7 * double(std::max(info.ascent, 1));
    const std::optional<::Slic3r::Emboss::Glyph> h = ::Slic3r::Emboss::letter2glyph(*file, 0, int('H'), float(std::max(info.unit_per_em, 64)) / 64.f);
    if (h.has_value() && !h->shape.empty()) {
        const BoundingBox box = get_extents(h->shape);
        if (box.max.y() > 0)
            capital = double(box.max.y()) * GLYPH_UNIT;
    }
    const double scale = std::max(settings.height, 0.01) / capital;
    // Their curves made of straight lines within about 0.02 mm.
    const float  flatness    = float(std::max(0.02 / scale, 0.5));
    const double line_height = double(info.ascent - info.descent + info.linegap) * scale;
    const double cos_a       = std::cos(settings.angle * PI / 180.);
    const double sin_a       = std::sin(settings.angle * PI / 180.);
    double       pen_x       = 0.;
    double       pen_y       = 0.;
    auto add = [&](const Polygon &polygon) {
        if (polygon.points.size() < 3)
            return;
        std::vector<Vec2d> outline;
        outline.reserve(polygon.points.size());
        for (const Point &p : polygon.points) {
            const double x = double(p.x()) * GLYPH_UNIT * scale + pen_x;
            const double y = double(p.y()) * GLYPH_UNIT * scale + pen_y;
            outline.emplace_back(x * cos_a - y * sin_a, x * sin_a + y * cos_a);
        }
        outlines.push_back(std::move(outline));
    };
    const std::wstring text = settings.text.ToStdWstring();
    for (size_t i = 0; i < text.size(); ++ i) {
        int letter = int(text[i]);
        // A character beyond the first plane of Unicode, in two halves (UTF-16 of Windows).
        if (letter >= 0xD800 && letter <= 0xDBFF && i + 1 < text.size() && int(text[i + 1]) >= 0xDC00 && int(text[i + 1]) <= 0xDFFF) {
            letter = 0x10000 + ((letter - 0xD800) << 10) + (int(text[i + 1]) - 0xDC00);
            ++ i;
        }
        if (letter == int(L'\r'))
            continue;
        if (letter == int(L'\n')) {
            pen_x = 0.;
            pen_y -= line_height;
            continue;
        }
        const std::optional<::Slic3r::Emboss::Glyph> glyph = ::Slic3r::Emboss::letter2glyph(*file, 0, letter, flatness);
        if (!glyph.has_value()) {
            // Not in the font: a space.
            pen_x += 0.5 * settings.height + settings.spacing;
            continue;
        }
        for (const ExPolygon &expolygon : glyph->shape) {
            add(expolygon.contour);
            for (const Polygon &hole : expolygon.holes)
                add(hole);
        }
        pen_x += double(glyph->advance_width) * scale + settings.spacing;
        if (outlines.size() > MAX_TEXT_OUTLINES)
            break;
    }
    if (outlines.empty()) {
        error = _L("This text has no letter in this font.");
        return false;
    }
    return true;
}

std::vector<GCodeSketch::Path> GCodeSketchPanel::text_paths(const Vec3d &point) const
{
    Vec3d n, u, v;
    plane_frame(n, u, v);
    std::vector<GCodeSketch::Path> paths;
    paths.reserve(m_text_outlines.size());
    for (const std::vector<Vec2d> &outline : m_text_outlines) {
        GCodeSketch::Path path;
        path.start = point + outline.front().x() * u + outline.front().y() * v;
        // Around it, back to its first point.
        for (size_t i = 1; i <= outline.size(); ++ i) {
            const Vec2d               &p = outline[i % outline.size()];
            GCodeSketch::Segment       segment;
            segment.extrude = m_extrude;
            segment.end     = point + p.x() * u + p.y() * v;
            const Vec3d &from = path.segments.empty() ? path.start : path.segments.back().end;
            if (i < outline.size() && (segment.end - from).norm() < EPSILON)
                continue;
            path.segments.push_back(segment);
        }
        if (path.segments.size() >= 2)
            paths.push_back(std::move(path));
    }
    return paths;
}

void GCodeSketchPanel::place_text()
{
    if (m_tool != SketchTool::Text)
        return;
    const TextSettings &last = m_text_settings;
    TextDialog dialog(this, last.text, last.font, last.bold, last.italic, last.height, last.angle, last.spacing);
    if (dialog.ShowModal() != wxID_OK) {
        focus_view();
        return;
    }
    TextSettings settings;
    settings.text    = dialog.text();
    settings.font    = dialog.font();
    settings.bold    = dialog.bold();
    settings.italic  = dialog.italic();
    settings.height  = dialog.height();
    settings.angle   = dialog.angle();
    settings.spacing = dialog.spacing();
    std::vector<std::vector<Vec2d>> outlines;
    wxString                        error;
    if (!text_outlines(settings, outlines, error)) {
        show_error(this, error);
        focus_view();
        return;
    }
    m_text_settings = settings;
    m_text_outlines = std::move(outlines);
    const std::vector<GCodeSketch::Path> paths = text_paths(m_text_point);
    if (!paths.empty()) {
        // One undo step for the whole text, each outline after a travel move.
        snapshot();
        for (const GCodeSketch::Path &path : paths)
            GCodeSketch::append_path(m_drawing, path);
    }
    next_step();
    if (!paths.empty())
        on_drawing_changed();
    on_tool_changed();
    focus_view();
}

void GCodeSketchPanel::add_tangent_arc(const Vec3d &end)
{
    if (m_tool != SketchTool::Line || !chain_has_segments())
        return;
    const Vec3d start = m_drawing.last_point();
    if ((end - start).norm() < EPSILON)
        return;
    GCodeSketch::Segment segment;
    segment.extrude = m_extrude;
    segment.end     = end;
    Vec3d middle;
    if (GCodeSketch::tangent_arc_middle(start, chain_tangent(), end, middle) && GCodeSketch::arc_through(start, middle, end).valid) {
        segment.type = GCodeSketch::SegmentType::Arc;
        segment.mid  = middle;
    }
    snapshot();
    m_drawing.segments.push_back(segment);
    ++ m_chain_count;
    if ((end - m_chain_start).norm() < EPSILON)
        m_chain = false;
    follow(end);
    next_step();
    on_drawing_changed();
    on_tool_changed();
}

void GCodeSketchPanel::end_chain()
{
    // The spline being drawn is made; the tool stays.
    const bool added = add_spline(false);
    m_chain = false;
    m_clicks.clear();
    m_picks.clear();
    if (added)
        follow(m_drawing.last_point());
    next_step();
    if (added)
        on_drawing_changed();
    on_tool_changed();
}

void GCodeSketchPanel::finish()
{
    // The objects chosen: the tool goes on, as the selection of Fusion 360 goes to the next field of its dialog.
    if (input() == SketchInput::Object && !m_objects.empty()) {
        m_objects_done = true;
        next_step();
        on_tool_changed();
        return;
    }
    bool added = add_spline(false);
    // The pattern, the offset or the scale shown, as OK.
    if (!added)
        added = apply_ready();
    m_chain = false;
    m_clicks.clear();
    m_picks.clear();
    restart_objects();
    m_tool = SketchTool::Select;
    next_step();
    if (added)
        on_drawing_changed();
    if (m_view != nullptr)
        m_view->on_selection_changed();
    on_tool_changed();
}

void GCodeSketchPanel::escape()
{
    if (m_tool == SketchTool::Select) {
        clear_selection();
        on_tool_changed();
        return;
    }
    // The shape in progress is dropped, the spline too, nothing is applied: back to Select.
    m_chain = false;
    m_clicks.clear();
    m_picks.clear();
    restart_objects();
    m_tool = SketchTool::Select;
    next_step();
    if (m_view != nullptr)
        m_view->on_selection_changed();
    on_tool_changed();
}

void GCodeSketchPanel::set_tool(SketchTool tool)
{
    // The segments selected are the objects of the tools working on objects, the path of the offset, as in Fusion 360.
    std::vector<size_t> selected;
    if ((takes_objects(tool) || tool == SketchTool::Offset) && m_selection.kind == SketchSelection::Kind::Segments && selection_valid())
        for (size_t idx = m_selection.first; idx <= m_selection.last; ++ idx)
            selected.push_back(idx);
    // Another tool, or the same one again: the line or the shape in progress is dropped, as in Fusion 360.
    m_chain = false;
    m_clicks.clear();
    m_picks.clear();
    restart_objects();
    m_tool = tool;
    if (!selected.empty()) {
        m_objects      = std::move(selected);
        m_objects_done = true;
    }
    // The text shown at the cursor before the first one is typed.
    if (tool == SketchTool::Text && m_text_outlines.empty()) {
        wxString error;
        text_outlines(m_text_settings, m_text_outlines, error);
    }
    // The values on the right are the ones of the Select tool.
    if (tool != SketchTool::Select)
        clear_selection();
    next_step();
    if (m_view != nullptr)
        m_view->on_selection_changed();
    on_tool_changed();
}

void GCodeSketchPanel::set_plane(SketchPlane plane)
{
    if (plane != m_plane) {
        // A flat shape lies in one plane: it goes on from its first point in the new one. The tangent circles pick
        // their lines again.
        if (flat_tool() && m_clicks.size() > 1)
            m_clicks.resize(1);
        m_picks.clear();
        m_sweep_valid = false;
    }
    m_plane = plane;
    // The new plane goes through the point being measured from, or the last point, otherwise through the first layer
    // or the middle of the bed.
    if (has_anchor())
        m_plane_offset = anchor()[plane_axis()];
    else if (!m_clicks.empty())
        m_plane_offset = m_clicks.front()[plane_axis()];
    else if (m_drawing.has_start)
        m_plane_offset = m_drawing.last_point()[plane_axis()];
    else
        m_plane_offset = plane_axis() == 2 ? z_step() : 0.5 * (plane_min() + plane_max());
    set_plane_offset(m_plane_offset);
    next_step();
    on_tool_changed();
}

void GCodeSketchPanel::next_plane()
{
    set_plane(m_plane == SketchPlane::Horizontal ? SketchPlane::Front : m_plane == SketchPlane::Front ? SketchPlane::Side : SketchPlane::Horizontal);
}

void GCodeSketchPanel::set_plane_offset(double offset)
{
    const double low  = plane_min();
    const double high = std::max(plane_max(), low);
    m_plane_offset    = std::round(std::clamp(offset, low, high) * 10000.) / 10000.;
    // The flat shape being drawn lies on the working plane: it moves with it.
    if (flat_tool()) {
        const int axis = plane_axis();
        for (Vec3d &point : m_clicks)
            point[axis] = m_plane_offset;
    }
    if (m_plane_bar != nullptr)
        m_plane_bar->Refresh();
    if (m_view != nullptr) {
        // The cursor and the shape being drawn on the plane where it is now.
        m_view->update_cursor_here();
        m_view->refresh();
    }
}

void GCodeSketchPanel::step_plane(int steps)
{
    set_plane_offset(m_plane_offset + steps * plane_step());
}

void GCodeSketchPanel::toggle_extrude()
{
    // A selection: its segments extrude or travel, as the construction button of Fusion 360 changes the selected
    // lines. Otherwise the new segments.
    if (m_selection.kind == SketchSelection::Kind::Segments && selection_valid()) {
        const bool value = !m_drawing.segments[m_selection.first].extrude;
        begin_change();
        for_selected([value](GCodeSketch::Segment &segment) { segment.extrude = value; });
        end_change();
        on_drawing_changed();
    } else
        m_extrude = !m_extrude;
    on_tool_changed();
}

void GCodeSketchPanel::on_tool_changed()
{
    if (m_toolbox != nullptr)
        m_toolbox->Refresh();
    if (m_plane_bar != nullptr)
        m_plane_bar->Refresh();
    update_hint();
    if (m_view != nullptr) {
        m_view->update_cursor_here();
        m_view->refresh();
    }
}

void GCodeSketchPanel::on_drawing_changed()
{
    // Objects no longer in the drawing.
    m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(), [this](size_t idx) { return idx >= m_drawing.segments.size(); }),
                    m_objects.end());
    if (m_objects.empty() && m_objects_done)
        restart_objects();
    if (!selection_valid()) {
        m_selection = SketchSelection();
        if (m_view != nullptr)
            m_view->on_selection_changed();
    }
    m_stacked.clear();
    // The other layers are shown in the view, unless there are too many of them.
    if (m_drawing.has_start && (m_settings.layers > 1 || m_settings.spiral) &&
        GCodeSketch::stacked_segments(m_drawing, m_settings) <= MAX_STACKED)
        m_stacked = GCodeSketch::stack_layers(m_drawing, m_settings);
    m_issues = GCodeSketch::check(m_drawing, m_machine, GCodeSketch::layers_height(m_settings));
    // The fields of the selection: built again when it changed, otherwise their values.
    if (selection_signature() != m_selection_signature)
        request_selection_ui();
    else
        update_selection_values();
    update_status();
    if (m_plane_bar != nullptr)
        m_plane_bar->Refresh();
    if (m_view != nullptr)
        m_view->on_drawing_changed();
}

void GCodeSketchPanel::update_status()
{
    const GCodeSketch::Stats stats = GCodeSketch::compute_stats(m_drawing, m_settings);
    wxString text = wxString::Format(_L("%d points, %.1f mm printed, %.1f mm of travel, %.2f m of filament (%.1f g), about %s"),
                                     int(GCodeSketch::list_points(m_drawing).size()), stats.extrude_length, stats.travel_length,
                                     stats.filament_length / 1000., stats.filament_weight, from_u8(short_time(get_time_dhms(float(stats.time)))));
    if (m_settings.layers > 1)
        text += ", " + wxString::Format(_L("on %d layers"), m_settings.layers);
    if (!m_issues.empty())
        text = wxString::Format(_L("%d problem(s). %s"), int(m_issues.size()), issue_text(m_issues.front())) + "   " + text;
    m_status_text->SetLabel(text);
    m_status_text->SetToolTip(text);
    m_status_text->SetForegroundColour(m_issues.empty() ? wxGetApp().get_label_clr_default() : wxColour(226, 75, 74));
    update_hint();
}

void GCodeSketchPanel::update_hint()
{
    // Step of the tool: the points clicked.
    const size_t step = m_clicks.size();
    // What the step asks, by step.
    auto by_step = [step](std::initializer_list<wxString> texts) {
        const size_t index = std::min(step, texts.size() - 1);
        return *(texts.begin() + index);
    };
    wxString hint;
    switch (m_tool) {
    case SketchTool::Select:
        hint = _L("Click a segment, a rectangle or a circle to edit it on the right; double click: the whole path. Drag: move. "
                  "Delete: delete.");
        break;
    case SketchTool::Line:
        if (!m_chain)
            hint = _L("Click the start of the line.");
        else if (m_chain_count == 0)
            hint = _L("Click the next point, or type its length (Tab: angle) and Enter.");
        else
            hint = _L("Next point, or type its length (Tab: angle). Drag from the last point: tangent arc. Double click: end the line.");
        break;
    case SketchTool::Arc:
        hint = by_step({ _L("Click the start of the arc."), _L("Click the end of the arc, or type the length of its chord."),
                         _L("Click a point of the arc, or type its radius.") });
        break;
    case SketchTool::Rectangle:
        hint = by_step({ _L("Click a corner of the rectangle."), _L("Click the opposite corner, or type its sides (Tab: next side) and Enter.") });
        break;
    case SketchTool::Rectangle3Points:
        hint = by_step({ _L("Click the start of a side of the rectangle."), _L("Click the end of that side, or type its length (Tab: angle)."),
                         _L("Click the other side, or type its length.") });
        break;
    case SketchTool::RectangleCenter:
        hint = by_step({ _L("Click the center of the rectangle."), _L("Click a corner, or type its sides (Tab: next side) and Enter.") });
        break;
    case SketchTool::Circle:
        hint = by_step({ _L("Click the center of the circle."), _L("Click a point of the circle, or type its diameter and Enter.") });
        break;
    case SketchTool::Circle2Points:
        hint = by_step({ _L("Click an end of a diameter of the circle."), _L("Click the other end, or type the diameter (Tab: angle).") });
        break;
    case SketchTool::Circle3Points:
        hint = by_step({ _L("Click a first point of the circle."), _L("Click a second point of the circle."),
                         _L("Click a third point of the circle.") });
        break;
    case SketchTool::CircleTangent2:
        hint = m_picks.empty()       ? _L("Click a first line the circle touches.") :
               m_picks.size() == 1   ? _L("Click a second line the circle touches.") :
                                       _L("Click its center, between the lines, or type its radius.");
        break;
    case SketchTool::CircleTangent3:
        hint = m_picks.empty()       ? _L("Click a first line the circle touches.") :
               m_picks.size() == 1   ? _L("Click a second line the circle touches.") :
                                       _L("Click a third line the circle touches, near where it touches it.");
        break;
    case SketchTool::ArcCenter:
        hint = by_step({ _L("Click the center of the arc."), _L("Click the start of the arc, or type its radius (Tab: angle)."),
                         _L("Turn around the center and click the end of the arc, or type its angle.") });
        break;
    case SketchTool::ArcTangent:
        hint = by_step({ _L("Click the end of a line or of an arc: the arc goes on along it."),
                         _L("Click the end of the arc, or type its radius (Tab: angle).") });
        break;
    case SketchTool::PolygonCircumscribed:
        hint = by_step({ _L("Click the center of the polygon."),
                         _L("Click the middle of a side, or type the diameter of the circle inside (Tab: sides).") });
        break;
    case SketchTool::PolygonInscribed:
        hint = by_step({ _L("Click the center of the polygon."),
                         _L("Click a vertex, or type the diameter of the circle through the vertices (Tab: sides).") });
        break;
    case SketchTool::PolygonEdge:
        hint = by_step({ _L("Click the start of a side of the polygon."), _L("Click the end of that side, or type its length (Tab: angle, sides)."),
                         _L("Click the side where the polygon is (Tab: sides).") });
        break;
    case SketchTool::Ellipse:
        hint = by_step({ _L("Click the center of the ellipse."), _L("Click the end of an axis, or type half its length (Tab: angle)."),
                         _L("Click the other axis, or type half its length.") });
        break;
    case SketchTool::SlotCenters:
        hint = by_step({ _L("Click the center of an end of the slot."), _L("Click the center of the other end, or type the distance (Tab: angle)."),
                         _L("Click the width of the slot, or type it.") });
        break;
    case SketchTool::SlotOverall:
        hint = by_step({ _L("Click an end of the slot."), _L("Click the other end, or type the length (Tab: angle)."),
                         _L("Click the width of the slot, or type it.") });
        break;
    case SketchTool::SlotCenter:
        hint = by_step({ _L("Click the center of the slot."), _L("Click the center of an end, or type the distance (Tab: angle)."),
                         _L("Click the width of the slot, or type it.") });
        break;
    case SketchTool::SlotArc3Points:
        hint = by_step({ _L("Click the center of an end of the slot."), _L("Click the center of the other end, or type the distance (Tab: angle)."),
                         _L("Click a point of its arc, or type its radius."), _L("Click the width of the slot, or type it.") });
        break;
    case SketchTool::SlotArcCenter:
        hint = by_step({ _L("Click the center of the arc of the slot."), _L("Click the center of an end, or type the radius (Tab: angle)."),
                         _L("Turn around the center and click the center of the other end, or type the angle."),
                         _L("Click the width of the slot, or type it.") });
        break;
    case SketchTool::Spline:
        hint = step == 0 ? _L("Click the first point of the spline.") :
                           _L("Click its next point. Double click or Enter: end it; click its first point: close it.");
        break;
    case SketchTool::Conic:
        hint = by_step({ _L("Click the start of the conic."), _L("Click its end, or type the length (Tab: angle)."),
                         wxString::Format(_L("Click its apex, where its tangents meet. Rho %.2f: type it (0.5: parabola)."), m_conic_rho) });
        break;
    case SketchTool::Point: hint = _L("Click a point: the nozzle goes there without extruding."); break;
    case SketchTool::Text: hint = _L("Click where the text starts, at the left of its baseline: its text, its font and its size are asked then."); break;
    case SketchTool::Mirror:
        hint = !m_objects_done ? _L("Click the shapes to mirror (double click: a whole path), then Enter.") :
               m_clicks.empty() ? _L("Click the mirror line: a line of the drawing, or a first point.") :
                                  _L("Click the second point of the mirror line, or type its angle.");
        break;
    case SketchTool::CircularPattern:
        hint = !m_objects_done  ? _L("Click the shapes to copy around a center (double click: a whole path), then Enter.") :
               m_clicks.empty() ? _L("Click the center of the pattern.") :
                                  wxString::Format(_L("%d copies over %.0f degrees: type their number (Tab: the angle), then click or Enter."),
                                                   m_pattern_count, m_pattern_angle);
        break;
    case SketchTool::RectangularPattern:
        hint = !m_objects_done ? _L("Click the shapes to copy in rows (double click: a whole path), then Enter.") :
                                 wxString::Format(_L("%d x %d copies: move the mouse to space them, or type their numbers and spacings (Tab), "
                                                     "then click or Enter."),
                                                  m_grid_counts[0], m_grid_counts[1]);
        break;
    case SketchTool::Fillet: hint = wxString::Format(_L("Click a corner to round it with a radius of %.2f mm, or type another radius."), m_fillet_radius); break;
    case SketchTool::ChamferEqual:
        hint = wxString::Format(_L("Click a corner between two lines to cut it %.2f mm from it on each one, or type another distance."), m_chamfer[0]);
        break;
    case SketchTool::ChamferDistanceAngle:
        hint = wxString::Format(_L("Click a corner between two lines to cut it %.2f mm from it on the first one, at %.1f degrees from it, or type "
                                   "others (Tab)."),
                                m_chamfer[0], m_chamfer_angle);
        break;
    case SketchTool::ChamferTwoDistances:
        hint = wxString::Format(_L("Click a corner between two lines to cut it %.2f mm from it on the first one and %.2f mm on the second one, or "
                                   "type others (Tab)."),
                                m_chamfer[0], m_chamfer[1]);
        break;
    case SketchTool::Offset:
        hint = !m_objects_done ? _L("Click a path to offset.") :
                                 _L("Move the mouse to the side and the distance of the offset, or type the distance, then click or Enter.");
        break;
    case SketchTool::Trim: hint = _L("Click the part of a curve to remove, between the lines crossing it (the whole curve if none does)."); break;
    case SketchTool::Extend: hint = _L("Click near the free end of a line or of an arc: it goes on up to the first line it meets."); break;
    case SketchTool::Break: hint = _L("Click a curve to split it where the lines cross it, or at the point clicked if none does."); break;
    case SketchTool::Scale:
        hint = !m_objects_done  ? _L("Click the shapes to scale (double click: a whole path), then Enter.") :
               m_clicks.empty() ? _L("Click the base point of the scale.") :
                                  wxString::Format(_L("Factor %.3f: move the mouse, or type the factor, then click or Enter."), m_scale_factor);
        break;
    case SketchTool::Move:
        hint = !m_objects_done  ? _L("Click the shapes to move (double click: a whole path), then Enter.") :
               m_clicks.empty() ? _L("Click the base point of the move.") :
                                  _L("Click where the base point goes, or type the distance (Tab: the angle).");
        break;
    case SketchTool::Copy:
        hint = !m_objects_done  ? _L("Click the shapes to copy (double click: a whole path), then Enter.") :
               m_clicks.empty() ? _L("Click the base point of the copies.") :
                                  _L("Click where each copy goes, or type the distance (Tab: the angle). Enter: done.");
        break;
    case SketchTool::Rotate:
        hint = !m_objects_done      ? _L("Click the shapes to rotate (double click: a whole path), then Enter.") :
               m_clicks.empty()     ? _L("Click the center of the rotation.") :
               m_clicks.size() == 1 ? _L("Click a point to turn.") :
                                      _L("Turn around the center and click where the point goes, or type the angle.");
        break;
    }
    // The objects chosen so far.
    if (input() == SketchInput::Object && !m_objects.empty())
        hint = wxString::Format(_L("%d segment(s) chosen."), int(m_objects.size())) + " " + hint;
    // Nothing is picked under the mouse in a very large drawing (imported G-code).
    const SketchInput picks = input();
    if (m_drawing.segments.size() > MAX_PICKED &&
        (picks == SketchInput::Object || picks == SketchInput::Path || picks == SketchInput::Curve || picks == SketchInput::Corner))
        hint = wxString::Format(_L("More than %d segments: the modify tools pick nothing in this drawing."), int(MAX_PICKED));
    // The polygons show their number of sides.
    if (m_tool == SketchTool::PolygonCircumscribed || m_tool == SketchTool::PolygonInscribed || m_tool == SketchTool::PolygonEdge)
        hint = wxString::Format(_L("%d sides."), m_polygon_sides) + " " + hint;
    if (m_tool != SketchTool::Select) {
        hint += "   " + _L("Esc: done. Ctrl: no constraint.");
        if (!m_extrude)
            hint += " " + _L("(travel moves, without extrusion)");
    }
    if (m_hint_text != nullptr && m_hint_text->GetLabel() != hint) {
        m_hint_text->SetLabel(hint);
        m_hint_text->SetToolTip(hint);
    }
}

void GCodeSketchPanel::update_export_tooltip()
{
    if (m_export_button != nullptr)
        m_export_button->SetToolTip(wxString::Format(_L("Save the G-code of the drawing as a .gcode file (Ctrl + E): %s. The file opens again in "
                                                        "this tab."),
                                                     m_export.standalone ? _L("program run alone by the printer") :
                                                                           _L("block to insert into G-code")));
}

void GCodeSketchPanel::show_cursor(const Vec3d *position, const wxString &snap)
{
    wxString text;
    if (position != nullptr) {
        text = format_point(*position);
        if (!snap.empty())
            text += "   (" + snap + ")";
    }
    if (m_cursor_text->GetLabel() != text)
        m_cursor_text->SetLabel(text);
}

Vec3d &GCodeSketchPanel::start_of(size_t idx)
{
    return idx == 0 || idx > m_drawing.segments.size() ? m_drawing.start : m_drawing.segments[idx - 1].end;
}

GCodeSketch::Arc GCodeSketchPanel::arc_of(size_t idx) const
{
    if (idx >= m_drawing.segments.size())
        return GCodeSketch::Arc();
    const GCodeSketch::Segment &segment = m_drawing.segments[idx];
    return GCodeSketch::arc_through(m_drawing.segment_start(idx), segment.mid, segment.end);
}

bool GCodeSketchPanel::selection_valid() const
{
    switch (m_selection.kind) {
    case SketchSelection::Kind::Point: return GCodeSketch::point_position(m_drawing, m_selection.point) != nullptr;
    case SketchSelection::Kind::Segments:
        return m_drawing.has_start && m_selection.first <= m_selection.last && m_selection.last < m_drawing.segments.size();
    default: return true;
    }
}

std::string GCodeSketchPanel::selection_signature() const
{
    std::string signature = std::to_string(int(m_selection.kind));
    if (!selection_valid())
        return signature + " ?";
    if (m_selection.kind == SketchSelection::Kind::Point)
        signature += " " + std::to_string(m_selection.point.segment) + (m_selection.point.mid ? "m" : "e");
    else if (m_selection.kind == SketchSelection::Kind::Segments) {
        // The fields depend on the shape, on the type of a segment alone and on what it does.
        const GCodeSketch::Segment &segment = m_drawing.segments[m_selection.first];
        signature += " " + std::to_string(m_selection.first) + "-" + std::to_string(m_selection.last) + " " +
                     std::to_string(int(GCodeSketch::shape_type(m_drawing, m_selection.first, m_selection.last))) +
                     (segment.type == GCodeSketch::SegmentType::Arc ? "a" : "l") + (segment.extrude ? "x" : "t");
    }
    return signature;
}

const GCodeSketch::Segment *GCodeSketchPanel::first_selected() const
{
    return m_selection.kind == SketchSelection::Kind::Segments && selection_valid() ? &m_drawing.segments[m_selection.first] : nullptr;
}

void GCodeSketchPanel::for_selected(const std::function<void(GCodeSketch::Segment &)> &change)
{
    if (m_selection.kind != SketchSelection::Kind::Segments || !selection_valid())
        return;
    for (size_t idx = m_selection.first; idx <= m_selection.last; ++ idx)
        change(m_drawing.segments[idx]);
}

void GCodeSketchPanel::edit(const std::function<void()> &change)
{
    // Fields of a selection which changed and whose fields aren't built again yet.
    if (m_selection_ui_pending || selection_signature() != m_selection_signature)
        return;
    // Nothing changed: no undo step.
    begin_change();
    change();
    end_change();
    on_drawing_changed();
}

void GCodeSketchPanel::translate_segments(size_t first, size_t last, const Vec3d &offset)
{
    if (first > last || last >= m_drawing.segments.size())
        return;
    start_of(first) += offset;
    for (size_t idx = first; idx <= last; ++ idx) {
        GCodeSketch::Segment &segment = m_drawing.segments[idx];
        segment.end += offset;
        if (segment.type == GCodeSketch::SegmentType::Arc)
            segment.mid += offset;
    }
}

Vec3d GCodeSketchPanel::rectangle_side(size_t first, int side) const
{
    if (first + 1 >= m_drawing.segments.size())
        return Vec3d::Zero();
    if (side == 0)
        return m_drawing.segments[first].end - m_drawing.segment_start(first);
    return m_drawing.segments[first + 1].end - m_drawing.segments[first].end;
}

void GCodeSketchPanel::resize_rectangle(size_t first, int side, double length)
{
    if (first + 3 >= m_drawing.segments.size() || length < EPSILON)
        return;
    const Vec3d corner = m_drawing.segment_start(first);
    Vec3d       a      = rectangle_side(first, 0);
    Vec3d       b      = rectangle_side(first, 1);
    if (a.norm() < EPSILON || b.norm() < EPSILON)
        return;
    if (side == 0)
        a = a.normalized() * length;
    else
        b = b.normalized() * length;
    m_drawing.segments[first].end     = corner + a;
    m_drawing.segments[first + 1].end = corner + a + b;
    m_drawing.segments[first + 2].end = corner + b;
    m_drawing.segments[first + 3].end = corner;
}

void GCodeSketchPanel::set_circle_diameter(size_t first, size_t last, double diameter)
{
    const GCodeSketch::Arc arc = arc_of(first);
    if (!arc.valid || last >= m_drawing.segments.size() || diameter < 2. * EPSILON)
        return;
    // Same start direction from the center, same plane, same way round.
    Vec3d radial = m_drawing.segment_start(first) - arc.center;
    radial -= radial.dot(arc.normal) * arc.normal;
    if (radial.norm() < EPSILON)
        return;
    const Vec3d                       start  = arc.center + 0.5 * diameter * radial.normalized();
    std::vector<GCodeSketch::Segment> halves = GCodeSketch::circle_segments(arc.center, start, arc.normal);
    if (halves.size() != 2)
        return;
    const GCodeSketch::Segment model = m_drawing.segments[first];
    for (GCodeSketch::Segment &half : halves) {
        half.extrude = model.extrude;
        half.width   = model.width;
        half.height  = model.height;
        half.speed   = model.speed;
    }
    start_of(first) = start;
    m_drawing.segments.erase(m_drawing.segments.begin() + first, m_drawing.segments.begin() + last + 1);
    m_drawing.segments.insert(m_drawing.segments.begin() + first, halves.begin(), halves.end());
    // A circle in one arc is now in two.
    if (m_selection.kind == SketchSelection::Kind::Segments && m_selection.first == first)
        m_selection.last = first + 1;
}

void GCodeSketchPanel::request_selection_ui()
{
    if (m_selection_ui_pending)
        return;
    m_selection_ui_pending = true;
    // Not now: the event being processed may come from one of the fields.
    CallAfter([this]() {
        if (m_selection_ui_pending)
            build_selection_ui();
    });
}

void GCodeSketchPanel::build_selection_ui()
{
    m_selection_ui_pending = false;
    if (m_selection_box == nullptr || m_side == nullptr)
        return;
    const int em = wxGetApp().em_unit();
    // The whole column, not only the box: the sections under it move, nothing of the old fields may stay drawn.
    m_side->Freeze();
    m_number_fields.clear();
    m_check_fields.clear();
    m_text_fields.clear();
    m_selection_wrapped.clear();
    wxSizer *sizer = m_selection_box->GetSizer();
    sizer->Clear(true);
    wxWindow *parent = m_selection_box;
    if (!selection_valid())
        m_selection = SketchSelection();
    // Width of the texts in the box.
    const int wrap = std::max(8 * em, m_side->GetClientSize().GetWidth() - 3 * em);

    auto heading = [&](const wxString &text) {
        auto *label = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        label->SetFont(wxGetApp().bold_font());
        label->SetForegroundColour(wxColour(8, 119, 232));
        label->SetMinSize(wxSize(em, -1));
        sizer->Add(label, 0, wxEXPAND | wxTOP, em / 2);
    };
    // Label cut if the column is narrow: it never widens it.
    auto label = [&](const wxString &text) {
        auto *control = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        control->SetMinSize(wxSize(em, -1));
        control->SetToolTip(text);
        return control;
    };
    // Text wrapped at the width of the column.
    auto note = [&](const wxString &text, bool in_small_font) {
        auto *control = new wxStaticText(parent, wxID_ANY, text);
        if (in_small_font)
            control->SetFont(wxGetApp().small_font());
        control->Wrap(wrap);
        m_selection_wrapped.emplace_back(control, text);
        sizer->Add(control, 0, wxTOP, em / 2);
    };
    // Field of a number, shown by get and changed by set.
    auto number = [&](wxSizer *line, double min, double max, double increment, unsigned int digits, std::function<double()> get,
                      std::function<void(double)> set, int width, int proportion) {
        auto *control = new wxSpinCtrlDouble(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(width * em, -1),
                                             wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER, min, max, min, increment);
        control->SetDigits(digits);
        control->Bind(wxEVT_SPINCTRLDOUBLE, [this, control, set](wxSpinDoubleEvent &) {
            if (!m_updating)
                set(control->GetValue());
        });
        m_number_fields.push_back({ control, std::move(get) });
        line->Add(control, proportion, wxALIGN_CENTER_VERTICAL);
        return control;
    };
    auto number_row = [&](const wxString &text, double min, double max, double increment, unsigned int digits, std::function<double()> get,
                          std::function<void(double)> set) {
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        line->Add(label(text), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
        number(line, min, max, increment, digits, std::move(get), std::move(set), 8, 0);
        sizer->Add(line, 0, wxEXPAND | wxTOP, em / 2);
    };
    // The X, Y and Z of a point, side by side, as wide as the column lets them.
    auto point_row = [&](const wxString &text, std::function<Vec3d()> get, std::function<void(const Vec3d &)> set) {
        sizer->Add(label(text), 0, wxEXPAND | wxTOP, em / 2);
        auto              *line    = new wxBoxSizer(wxHORIZONTAL);
        static const char *names[] = { "X", "Y", "Z" };
        for (int k = 0; k < 3; ++ k) {
            line->Add(new wxStaticText(parent, wxID_ANY, names[k]), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 4);
            number(line, k == 2 ? -10. : -1000., 1000., k == 2 ? z_step() : 1., 3, [get, k]() { return get()[k]; },
                   [this, get, set, k](double value) {
                       // The fields of another selection, not built again yet: get() would read a changed drawing.
                       if (m_selection_ui_pending || selection_signature() != m_selection_signature)
                           return;
                       Vec3d p = get();
                       p[k]    = value;
                       set(p);
                   },
                   5, 1);
            if (k < 2)
                line->AddSpacer(em / 2);
        }
        sizer->Add(line, 0, wxEXPAND | wxTOP, em / 4);
    };
    // A label and its value, wrapped (update_selection_values()).
    auto text_row = [&](const wxString &text, std::function<wxString()> get) {
        auto *value = new wxStaticText(parent, wxID_ANY, wxEmptyString);
        m_text_fields.push_back({ value, text, std::move(get), wxString() });
        sizer->Add(value, 0, wxEXPAND | wxTOP, em / 2);
    };
    // The start of the selected segments: changing it moves them all.
    auto start_row = [&](const wxString &text, size_t first, size_t last) {
        point_row(text, [this, first]() { return Vec3d(m_drawing.segment_start(first)); },
                  [this, first, last](const Vec3d &p) {
                      edit([this, first, last, p]() { translate_segments(first, last, p - m_drawing.segment_start(first)); });
                  });
    };
    // Extrusion of the selected segments, 0 for the settings of the drawing.
    auto setting_row = [&](const wxString &text, double max, double increment, unsigned int digits, double GCodeSketch::Segment::*value) {
        number_row(text, 0., max, increment, digits,
                   [this, value]() {
                       const GCodeSketch::Segment *segment = first_selected();
                       return segment != nullptr ? segment->*value : 0.;
                   },
                   [this, value](double number) {
                       edit([this, value, number]() { for_selected([value, number](GCodeSketch::Segment &segment) { segment.*value = number; }); });
                   });
    };

    if (m_selection.kind == SketchSelection::Kind::None) {
        note(_L("Select a path in the view with the Select tool (V) to edit its values here: a click selects a segment, a rectangle "
                "or a circle, a double click the whole path."),
             false);
    } else if (m_selection.kind == SketchSelection::Kind::Point) {
        const GCodeSketch::PointRef ref = m_selection.point;
        heading(ref.segment < 0 ? _L("Start of the drawing") : ref.mid ? _L("Point an arc passes through") : _L("Point"));
        point_row(_L("Position (mm)"),
                  [this, ref]() {
                      const Vec3d *p = GCodeSketch::point_position(m_drawing, ref);
                      return p != nullptr ? *p : Vec3d(Vec3d::Zero());
                  },
                  [this, ref](const Vec3d &p) {
                      edit([this, ref, p]() {
                          if (Vec3d *q = GCodeSketch::point_position(m_drawing, ref))
                              *q = p;
                      });
                  });
    } else {
        const size_t                 first = m_selection.first;
        const size_t                 last  = m_selection.last;
        const GCodeSketch::Segment  &model = m_drawing.segments[first];
        const GCodeSketch::ShapeType shape = GCodeSketch::shape_type(m_drawing, first, last);
        if (shape == GCodeSketch::ShapeType::Rectangle) {
            heading(_L("Rectangle"));
            start_row(_L("Corner (mm)"), first, last);
            static const char *names[] = { "X", "Y", "Z" };
            for (int side = 0; side < 2; ++ side) {
                // Named after its axis when it is along one.
                const Vec3d d    = rectangle_side(first, side).normalized();
                wxString    name = wxString::Format(_L("Side %d (mm)"), side + 1);
                for (int k = 0; k < 3; ++ k)
                    if (std::abs(std::abs(d[k]) - 1.) < 1e-6)
                        name = wxString::Format(_L("Side along %s (mm)"), names[k]);
                number_row(name, 0.001, 2000., 1., 3, [this, first, side]() { return rectangle_side(first, side).norm(); },
                           [this, first, side](double length) { edit([this, first, side, length]() { resize_rectangle(first, side, length); }); });
            }
        } else if (shape == GCodeSketch::ShapeType::Circle) {
            heading(_L("Circle"));
            point_row(_L("Center (mm)"), [this, first]() { return arc_of(first).center; },
                      [this, first, last](const Vec3d &p) {
                          edit([this, first, last, p]() {
                              const GCodeSketch::Arc arc = arc_of(first);
                              if (arc.valid)
                                  translate_segments(first, last, p - arc.center);
                          });
                      });
            number_row(_L("Diameter (mm)"), 0.002, 2000., 1., 3, [this, first]() { return 2. * arc_of(first).radius; },
                       [this, first, last](double diameter) {
                           edit([this, first, last, diameter]() { set_circle_diameter(first, last, diameter); });
                       });
        } else if (first == last && model.type == GCodeSketch::SegmentType::Arc) {
            heading(model.extrude ? _L("Arc") : _L("Travel arc"));
            start_row(_L("Start (mm)"), first, first);
            point_row(_L("End (mm)"), [this, first]() { return m_drawing.segments[first].end; },
                      [this, first](const Vec3d &p) { edit([this, first, p]() { m_drawing.segments[first].end = p; }); });
            point_row(_L("Point it passes through (mm)"), [this, first]() { return m_drawing.segments[first].mid; },
                      [this, first](const Vec3d &p) { edit([this, first, p]() { m_drawing.segments[first].mid = p; }); });
            number_row(_L("Radius (mm)"), 0.001, 100000., 1., 3,
                       [this, first]() {
                           const GCodeSketch::Arc arc = arc_of(first);
                           return arc.valid ? arc.radius : 0.;
                       },
                       [this, first](double radius) {
                           edit([this, first, radius]() {
                               GCodeSketch::Segment &segment = m_drawing.segments[first];
                               Vec3d                 middle;
                               // Same ends, on the same side.
                               if (GCodeSketch::arc_middle_with_radius(m_drawing.segment_start(first), segment.end, segment.mid, radius, middle))
                                   segment.mid = middle;
                           });
                       });
            text_row(_L("Center"), [this, first]() {
                const GCodeSketch::Arc arc = arc_of(first);
                return arc.valid ? format_point(arc.center) : wxString("-");
            });
            text_row(_L("Angle"), [this, first]() {
                const GCodeSketch::Arc arc = arc_of(first);
                return arc.valid ? wxString::Format("%.1f", arc.angle * 180. / PI) + wxString::FromUTF8("\xC2\xB0") : wxString("-");
            });
            text_row(_L("Length"), [this, first]() {
                const GCodeSketch::Arc arc = arc_of(first);
                return arc.valid ? wxString::Format("%.2f mm", arc.length()) : wxString("-");
            });
        } else if (first == last) {
            heading(model.extrude ? _L("Line") : _L("Travel move"));
            start_row(_L("Start (mm)"), first, first);
            point_row(_L("End (mm)"), [this, first]() { return m_drawing.segments[first].end; },
                      [this, first](const Vec3d &p) { edit([this, first, p]() { m_drawing.segments[first].end = p; }); });
            number_row(_L("Length (mm)"), 0.001, 2000., 1., 3,
                       [this, first]() { return (m_drawing.segments[first].end - m_drawing.segment_start(first)).norm(); },
                       [this, first](double length) {
                           edit([this, first, length]() {
                               // Same start and direction.
                               const Vec3d start     = m_drawing.segment_start(first);
                               Vec3d       direction = m_drawing.segments[first].end - start;
                               direction             = direction.norm() > EPSILON ? Vec3d(direction.normalized()) : Vec3d(Vec3d::UnitX());
                               m_drawing.segments[first].end = start + length * direction;
                           });
                       });
        } else {
            heading(wxString::Format(model.extrude ? _L("Path of %d segments") : _L("Travel moves: %d segments"), int(last - first + 1)));
            start_row(_L("Start (mm)"), first, last);
            text_row(_L("Length"), [this, first, last]() {
                double length = 0.;
                for (size_t idx = first; idx <= last && idx < m_drawing.segments.size(); ++ idx) {
                    // A straight segment keeps a point an arc would pass through: only the arcs count as arcs.
                    const bool             is_arc = m_drawing.segments[idx].type == GCodeSketch::SegmentType::Arc;
                    const GCodeSketch::Arc arc    = is_arc ? arc_of(idx) : GCodeSketch::Arc();
                    length += arc.valid ? arc.length() : (m_drawing.segments[idx].end - m_drawing.segment_start(idx)).norm();
                }
                return wxString::Format("%.2f mm", length);
            });
        }

        // What the segments do: the Extrusion button of the toolbox changes it. Their own settings.
        note(model.extrude ? _L("It extrudes. To make it travel: the Extrusion button of the toolbox (E).") :
                             _L("It travels without extruding. To make it extrude: the Travel button of the toolbox (E)."),
             true);
        setting_row(_L("Line width (mm)"), 5., 0.01, 2, &GCodeSketch::Segment::width);
        setting_row(_L("Layer height (mm)"), 5., 0.01, 2, &GCodeSketch::Segment::height);
        setting_row(_L("Speed (mm/s)"), 1000., 1., 0, &GCodeSketch::Segment::speed);
        note(_L("0: the setting of the drawing (Extrusion below)."), true);
    }

    if (m_selection.kind != SketchSelection::Kind::None) {
        ::Button *remove = make_button(parent, _L("Delete"), [this]() { delete_selection(); });
        remove->SetToolTip(_L("Delete the selection (Del). The rest of the drawing stays in place, linked by a travel move."));
        sizer->Add(remove, 0, wxTOP, em / 2);
    }

    m_selection_signature = selection_signature();
    update_selection_values();
    wxGetApp().UpdateDarkUIWin(m_selection_box);
    m_selection_box->Layout();
    m_side->Layout();
    m_side->FitInside();
    m_side->Thaw();
    // Everything under the selection moved.
    m_side->Refresh();
}

void GCodeSketchPanel::update_selection_values()
{
    if (m_selection_ui_pending || selection_signature() != m_selection_signature)
        return;
    m_updating = true;
    for (const NumberField &field : m_number_fields)
        field.control->SetValue(field.get());
    for (const CheckField &field : m_check_fields)
        field.check->SetValue(field.get());
    const int em      = wxGetApp().em_unit();
    const int wrap    = m_side != nullptr ? std::max(8 * em, m_side->GetClientSize().GetWidth() - 3 * em) : 30 * em;
    bool      changed = false;
    for (TextField &field : m_text_fields) {
        const wxString shown = wxString::Format(_L("%s: %s"), field.label, field.get());
        if (shown == field.shown)
            continue;
        field.shown = shown;
        field.text->SetLabel(shown);
        field.text->Wrap(wrap);
        changed = true;
    }
    m_updating = false;
    // A value on more lines, or on fewer.
    if (changed && m_side != nullptr && m_selection_box != nullptr) {
        m_selection_box->Layout();
        m_side->Layout();
        m_side->FitInside();
    }
}

void GCodeSketchPanel::on_selection_changed()
{
    request_selection_ui();
    // The Extrusion button shows what the selection does.
    if (m_toolbox != nullptr)
        m_toolbox->Refresh();
    if (m_view != nullptr)
        m_view->on_selection_changed();
}

void GCodeSketchPanel::select_point(const GCodeSketch::PointRef &ref)
{
    SketchSelection selection;
    selection.kind  = SketchSelection::Kind::Point;
    selection.point = ref;
    if (selection == m_selection)
        return;
    m_selection = selection;
    on_selection_changed();
}

void GCodeSketchPanel::select_segments(size_t first, size_t last)
{
    SketchSelection selection;
    selection.kind  = SketchSelection::Kind::Segments;
    selection.first = std::min(first, last);
    selection.last  = std::max(first, last);
    if (selection == m_selection)
        return;
    m_selection = selection;
    on_selection_changed();
}

void GCodeSketchPanel::select_at(size_t idx, bool path)
{
    if (idx >= m_drawing.segments.size()) {
        clear_selection();
        return;
    }
    size_t first = idx;
    size_t last  = idx;
    if (path)
        GCodeSketch::find_path(m_drawing, idx, first, last);
    else
        run_at(idx, first, last);
    select_segments(first, last);
}

void GCodeSketchPanel::clear_selection()
{
    if (m_selection.kind == SketchSelection::Kind::None)
        return;
    m_selection = SketchSelection();
    on_selection_changed();
}

void GCodeSketchPanel::delete_point(const GCodeSketch::PointRef &ref)
{
    if (ref.segment < 0) {
        // The end of the first segment becomes the start.
        if (m_drawing.segments.empty())
            m_drawing.clear();
        else {
            m_drawing.start = m_drawing.segments.front().end;
            m_drawing.segments.erase(m_drawing.segments.begin());
        }
    } else if (size_t(ref.segment) < m_drawing.segments.size()) {
        if (ref.mid)
            // Without the point it passes through, the arc becomes a straight line.
            m_drawing.segments[size_t(ref.segment)].type = GCodeSketch::SegmentType::Line;
        else
            m_drawing.segments.erase(m_drawing.segments.begin() + ref.segment);
    }
    // A point alone draws nothing.
    if (m_drawing.segments.empty())
        m_drawing.clear();
}

void GCodeSketchPanel::delete_selection()
{
    if (m_selection.kind == SketchSelection::Kind::None || !selection_valid())
        return;
    snapshot();
    if (m_selection.kind == SketchSelection::Kind::Point)
        delete_point(m_selection.point);
    else
        GCodeSketch::remove_segments(m_drawing, m_selection.first, m_selection.last);
    m_selection = SketchSelection();
    // The line being drawn may have lost its segments, the tangent circles their lines, the tools their objects.
    m_chain = false;
    m_clicks.clear();
    m_picks.clear();
    restart_objects();
    next_step();
    on_selection_changed();
    on_drawing_changed();
    on_tool_changed();
}

void GCodeSketchPanel::begin_move(const GCodeSketch::PointRef &ref)
{
    begin_change();
    m_moving = true;
    m_move_origin.clear();
    select_point(ref);
}

void GCodeSketchPanel::move_point(const GCodeSketch::PointRef &ref, const Vec3d &position)
{
    Vec3d *point = GCodeSketch::point_position(m_drawing, ref);
    if (point == nullptr || *point == position)
        return;
    *point = position;
    on_drawing_changed();
}

void GCodeSketchPanel::begin_move_segments(size_t first, size_t last)
{
    if (first > last || last >= m_drawing.segments.size())
        return;
    begin_change();
    m_moving     = true;
    m_move_first = first;
    m_move_last  = last;
    m_move_origin.clear();
    m_move_origin.push_back(m_drawing.segment_start(first));
    for (size_t idx = first; idx <= last; ++ idx) {
        m_move_origin.push_back(m_drawing.segments[idx].mid);
        m_move_origin.push_back(m_drawing.segments[idx].end);
    }
}

void GCodeSketchPanel::move_segments(const Vec3d &offset)
{
    if (!m_moving || m_move_last >= m_drawing.segments.size() || m_move_origin.size() != 1 + 2 * (m_move_last - m_move_first + 1))
        return;
    start_of(m_move_first) = m_move_origin[0] + offset;
    for (size_t idx = m_move_first, k = 1; idx <= m_move_last; ++ idx, k += 2) {
        GCodeSketch::Segment &segment = m_drawing.segments[idx];
        segment.mid = m_move_origin[k] + offset;
        segment.end = m_move_origin[k + 1] + offset;
    }
    on_drawing_changed();
}

void GCodeSketchPanel::end_move()
{
    // Dragged back to where it was: no undo step.
    if (m_moving)
        end_change();
    m_moving = false;
    m_move_origin.clear();
}

void GCodeSketchPanel::cancel_move()
{
    if (!m_moving || m_undo.empty())
        return;
    // The drawing before the move: end_change() drops its undo step.
    m_drawing = m_undo.back();
    end_change();
    m_moving = false;
    m_move_origin.clear();
    on_drawing_changed();
}

void GCodeSketchPanel::undo()
{
    if (m_undo.empty())
        return;
    m_redo.push_back(std::move(m_drawing));
    m_drawing = std::move(m_undo.back());
    m_undo.pop_back();
    // The line or the shape in progress is dropped: a click on the last point goes on with the drawing.
    m_chain = false;
    m_clicks.clear();
    m_picks.clear();
    restart_objects();
    m_selection = SketchSelection();
    next_step();
    on_selection_changed();
    on_drawing_changed();
    on_tool_changed();
}

void GCodeSketchPanel::redo()
{
    if (m_redo.empty())
        return;
    m_undo.push_back(std::move(m_drawing));
    m_drawing = std::move(m_redo.back());
    m_redo.pop_back();
    m_chain = false;
    m_clicks.clear();
    m_picks.clear();
    restart_objects();
    m_selection = SketchSelection();
    next_step();
    on_selection_changed();
    on_drawing_changed();
    on_tool_changed();
}

void GCodeSketchPanel::new_drawing()
{
    if (!m_drawing.has_start) {
        // Nothing drawn: only the line or the shape in progress goes.
        m_chain = false;
        m_clicks.clear();
        m_picks.clear();
        restart_objects();
        next_step();
        on_tool_changed();
        return;
    }
    // Ctrl + Z brings it back.
    snapshot();
    m_drawing.clear();
    m_selection = SketchSelection();
    m_chain     = false;
    m_clicks.clear();
    m_picks.clear();
    restart_objects();
    m_last_file.clear();
    next_step();
    set_plane_offset(m_plane == SketchPlane::Horizontal ? z_step() : 0.5 * (plane_min() + plane_max()));
    on_selection_changed();
    on_drawing_changed();
    on_tool_changed();
}

void GCodeSketchPanel::open_drawing()
{
    wxFileDialog dialog(this, _L("Open a G-code drawing"), m_last_dir, wxEmptyString, file_wildcard(), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;
    const wxString path = dialog.GetPath();
    std::string    gcode;
    {
        boost::nowide::ifstream file(into_u8(path), std::ios::binary);
        if (!file) {
            show_error(this, _L("The file cannot be read."));
            return;
        }
        std::stringstream content;
        content << file.rdbuf();
        gcode = content.str();
    }

    GCodeSketch::Drawing  drawing;
    GCodeSketch::Settings settings = m_settings;
    // A drawing saved by this tab, otherwise the moves of any G-code, printed as they are: on one layer.
    if (!GCodeSketch::deserialize(gcode, drawing, settings)) {
        if (!GCodeSketch::import_moves(gcode, drawing)) {
            show_error(this, _L("No move was found in this file."));
            return;
        }
        settings.layers = 1;
        settings.spiral = false;
    }
    snapshot();
    m_drawing   = std::move(drawing);
    m_settings  = settings;
    m_selection = SketchSelection();
    m_chain     = false;
    m_clicks.clear();
    m_picks.clear();
    restart_objects();
    m_last_dir  = wxPathOnly(path);
    m_last_file = wxFileNameFromPath(path);
    next_step();
    write_settings();
    if (m_drawing.has_start)
        follow(m_drawing.last_point());
    on_selection_changed();
    on_drawing_changed();
    on_tool_changed();
    m_view->zoom_to_bed();
}

std::vector<std::string> GCodeSketchPanel::export_header() const
{
    return { "G-code Drawing, " SLIC3R_APP_FULL_NAME, "printer: " + m_printer_name, "filament: " + m_filament_name };
}

void GCodeSketchPanel::export_gcode()
{
    if (!m_drawing.has_start || m_drawing.segments.empty()) {
        show_info(this, _L("The drawing is empty: draw it in the view first, with the Line tool (L) for instance."), _L("G-code Drawing"));
        return;
    }
    m_export.header = export_header();
    // Size of the G-code: the one of a layer times the layers, as in the G-code dialog.
    GCodeSketch::Settings one_layer = m_settings;
    one_layer.layers                = 1;
    const double megabytes = double(GCodeSketch::generate(m_drawing, one_layer, m_machine, m_export).size()) *
                             double(std::max(m_settings.layers, 1)) / (1024. * 1024.);
    if (megabytes > MAX_GCODE_MB) {
        show_error(this, wxString::Format(_L("The G-code of the drawing would be about %d MB, at most %d MB: reduce the number of layers."),
                                          int(std::min(megabytes, 1e9)), int(MAX_GCODE_MB)));
        return;
    }
    if (!m_issues.empty()) {
        MessageDialog warning(this, wxString::Format(_L("The drawing has %d problem(s). %s\nExport it anyway?"), int(m_issues.size()),
                                                     issue_text(m_issues.front())),
                              _L("G-code Drawing"), wxYES_NO | wxICON_WARNING);
        if (warning.ShowModal() != wxID_YES)
            return;
    }
    wxFileDialog dialog(this, _L("Export G-code"), m_last_dir, m_last_file.empty() ? wxString("drawing.gcode") : m_last_file,
                        _L("G-code files") + " (*.gcode)|*.gcode", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK)
        return;
    const wxString    path  = dialog.GetPath();
    const std::string gcode = GCodeSketch::generate(m_drawing, m_settings, m_machine, m_export);
    bool              written = false;
    {
        boost::nowide::ofstream file(into_u8(path), std::ios::binary | std::ios::trunc);
        if (file) {
            file << gcode;
            // The drawing, so that the file opens again for editing in the tab.
            file << GCodeSketch::serialize(m_drawing, m_settings);
            file.close();
            written = !file.fail();
        }
    }
    if (!written) {
        show_error(this, _L("The file cannot be written."));
        return;
    }
    m_last_dir  = wxPathOnly(path);
    m_last_file = wxFileNameFromPath(path);
    const wxString done = wxString::Format(_L("Saved to %s"), path);
    m_hint_text->SetLabel(done);
    m_hint_text->SetToolTip(done);
}

void GCodeSketchPanel::show_gcode()
{
    if (!m_drawing.has_start) {
        show_info(this, _L("The drawing is empty: draw it in the view first, with the Line tool (L) for instance."), _L("G-code Drawing"));
        return;
    }
    m_export.header = export_header();
    wxString problems;
    for (size_t i = 0; i < m_issues.size() && i < 5; ++ i)
        problems += issue_text(m_issues[i]) + "\n";
    if (m_issues.size() > 5)
        problems += wxString::Format(_L("And %d more."), int(m_issues.size() - 5));
    GCodeSketchDialog dialog(this, m_drawing, m_settings, m_machine, m_export, problems, m_last_dir,
                             m_last_file.empty() ? wxString("drawing.gcode") : m_last_file);
    dialog.ShowModal();
    if (!dialog.directory().empty())
        m_last_dir = dialog.directory();
    if (!dialog.file_name().empty())
        m_last_file = dialog.file_name();
    // The options chosen there are the ones of the export.
    update_export_tooltip();
    // Sent through the G-code console, once this modal dialog is closed: the G-code is checked there before sending.
    if (dialog.send_requested())
        GCodeConsoleDialog::show_console_with_text(nullptr, from_u8(dialog.gcode()));
}

} // namespace GUI
} // namespace Slic3r
