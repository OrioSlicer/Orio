#include "GCodeToMeshPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "Widgets/StateColor.hpp"
#include "slic3r/Utils/FixModelByWin10.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <wx/base64.h>
#include <wx/dcbuffer.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/log.h>
#include <wx/mstream.h>
#include <wx/numformatter.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {

// Roles of the texts, for their colors (TEXT is a macro of Windows).
constexpr int PLAIN     = 0;
constexpr int SECONDARY = 1;
constexpr int ACCENT    = 2;

// The colors of these line types in the Preview (BaseRenderer.cpp), which never change.
const wxColour OUTER_WALL(255, 125, 56);
const wxColour INNER_WALL(255, 230, 77);
const wxColour SPARSE_INFILL(176, 48, 41);
// A mesh: top, left and right faces.
const wxColour MESH[3] = { wxColour(159, 208, 255), wxColour(79, 163, 240), wxColour(47, 134, 219) };
// The kinds of surfaces of a STEP conversion, in the order of MeshToStep::Kind: plane, cylinder, cone, sphere, torus,
// facet (a small plane of a surface which is not recognized), extrusion, revolution, helix, free surface.
const wxColour SURFACE[10] = { wxColour(204, 214, 230), wxColour(242, 153, 64), wxColour(89, 191, 89),  wxColour(166, 115, 217),
                               wxColour(230, 77, 89),   wxColour(140, 140, 140), wxColour(46, 178, 160), wxColour(235, 200, 60),
                               wxColour(220, 100, 180), wxColour(70, 150, 235) };
// The "Beta" badges of the tabs (Button::renderBadge): lemon yellow, dark text.
const wxColour BETA_YELLOW(250, 208, 0);
const wxColour BETA_TEXT(45, 45, 48);
// The choices of card 2 of a STEP conversion: the deviation allowed between the mesh and the surfaces found, mm. The
// smallest gives the truest surfaces.
constexpr double TOLERANCES[4] = { 0.01, 0.02, 0.05, 0.1 };

wxString number(double value, int precision = 2) { return wxNumberFormatter::ToString(value, precision, wxNumberFormatter::Style_NoTrailingZeroes); }

wxString mm(double value) { return number(value) + " mm"; }

// "0.42 mm", or "0.42 - 0.5 mm".
wxString mm_range(double min, double max)
{
    if (max - min < 0.005)
        return mm(max);
    return number(min) + wxString::FromUTF8(" \xE2\x80\x93 ") + mm(max);
}

const wxString &separator()
{
    static const wxString dot = wxString::FromUTF8(" \xC2\xB7 ");
    return dot;
}

::Button *make_button(wxWindow *parent, const wxString &label, bool main)
{
    auto *button = new ::Button(parent, label);
    if (main) {
        // Orio: white on deep blue, in both themes.
        StateColor blue(std::pair<wxColour, int>(wxColour("#C4C8CE"), StateColor::Disabled),
                        std::pair<wxColour, int>(wxColour("#0560C2"), StateColor::Pressed),
                        std::pair<wxColour, int>(wxColour("#2B8CF2"), StateColor::Hovered),
                        std::pair<wxColour, int>(wxColour("#0874E0"), StateColor::Normal));
        button->SetBackgroundColor(blue);
        button->SetBorderWidth(0);
        button->SetTextColor(wxColour("#FEFEFE"));
        button->SetFont(Label::Head_14);
    } else {
        // Light colours, which StateColor turns into their dark mode ones.
        StateColor background(std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Pressed),
                              std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Hovered),
                              std::pair<wxColour, int>(wxColour("#FFFFFF"), StateColor::Normal));
        button->SetBackgroundColor(background);
        button->SetBorderColor(StateColor(wxColour("#ACACAC")));
        button->SetTextColor(StateColor(wxColour("#262E30")));
        button->SetFont(Label::Body_14);
    }
    button->SetCornerRadius(parent->FromDIP(6));
    button->SetMinSize(wxSize(parent->FromDIP(100), parent->FromDIP(36)));
    return button;
}

// "6h 13m 2s", "1d 2h 3m", "45m 10s" -> "6 h 13 min", "26 h 3 min", "45 min"; empty if no time is found.
wxString print_time(const std::string &text)
{
    long long seconds = 0;
    bool      found   = false;
    for (size_t i = 0; i < text.size();) {
        if (!std::isdigit((unsigned char) text[i])) {
            ++i;
            continue;
        }
        long long value = 0;
        while (i < text.size() && std::isdigit((unsigned char) text[i]))
            value = value * 10 + (text[i++] - '0');
        while (i < text.size() && text[i] == ' ')
            ++i;
        const char unit = i < text.size() ? text[i] : 0;
        if (unit == 'd')
            seconds += value * 86400;
        else if (unit == 'h')
            seconds += value * 3600;
        else if (unit == 'm')
            seconds += value * 60;
        else if (unit == 's')
            seconds += value;
        else
            continue;
        found = true;
    }
    if (!found)
        return wxString();
    const int minutes = int((seconds + 30) / 60);
    return minutes >= 60 ? wxString::Format("%d h %d min", minutes / 60, minutes % 60) : wxString::Format("%d min", minutes);
}

// The G-codes opened lately, the latest first, in the settings of the application.
constexpr int RECENT_COUNT = 5;

std::vector<wxString> recent_gcodes()
{
    std::vector<wxString> out;
    for (int i = 0; i < RECENT_COUNT; ++i) {
        const std::string path = wxGetApp().app_config->get("gcode_to_mesh", "recent_" + std::to_string(i));
        if (!path.empty())
            out.emplace_back(from_u8(path));
    }
    return out;
}

void remember_gcode(const wxString &path)
{
    std::vector<wxString> list = recent_gcodes();
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    AppConfig *config = wxGetApp().app_config;
    for (int i = 0; i < RECENT_COUNT; ++i)
        config->set("gcode_to_mesh", "recent_" + std::to_string(i), i < int(list.size()) ? into_u8(list[i]) : std::string());
    config->save();
}

// Thrown to stop building meshes which are no longer wanted.
struct Canceled
{};

// A mesh to convert into STEP: an STL, an OBJ or a 3MF (not a G-code in a 3MF).
bool is_mesh_file(const wxString &path)
{
    const wxString lower = path.Lower();
    return (lower.EndsWith(".stl") || lower.EndsWith(".obj") || lower.EndsWith(".3mf")) && !lower.EndsWith(".gcode.3mf");
}

// The parts of an object (not its modifiers nor its negative parts), placed by one of its instances or by each of them
// (-1), facing outward, as exported into STL.
TriangleMesh object_mesh(const ModelObject &object, int instance)
{
    TriangleMesh parts;
    for (const ModelVolume *volume : object.volumes)
        if (volume->is_model_part()) {
            TriangleMesh mesh(volume->mesh());
            mesh.transform(volume->get_matrix(), true);
            parts.merge(mesh);
        }
    TriangleMesh out;
    for (size_t i = 0; i < object.instances.size(); ++i)
        if (instance < 0 || int(i) == instance) {
            TriangleMesh mesh = parts;
            mesh.transform(object.instances[i]->get_matrix(), true);
            out.merge(mesh);
        }
    return out;
}

// The meshes seen by the viewer: turned by yaw around Z, then tilted by pitch so that their top comes toward the viewer,
// fitted in w x h whichever way they are turned. Flat shaded, white (gray in the light theme), lit from the upper left,
// on the background: a z-buffer over all the triangles, both of their sides lit (no hole where a face is reversed).
// kinds: the kind of surface of each face (index in SURFACE), counted over all the meshes, to color them.
// ponytail: drawn by the processor, no OpenGL canvas to set up; OpenGL if large prints turn too slowly.
wxImage render_meshes(const indexed_triangle_set *meshes, size_t count, int w, int h, double yaw, double pitch, bool dark, const wxColour &background,
                      const std::vector<unsigned char> &kinds = {})
{
    wxImage        image(w, h, false);
    unsigned char *pixels = image.GetData();
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        pixels[3 * i]     = background.Red();
        pixels[3 * i + 1] = background.Green();
        pixels[3 * i + 2] = background.Blue();
    }
    Vec3f low  = Vec3f::Constant(std::numeric_limits<float>::max());
    Vec3f high = -low;
    for (size_t m = 0; m < count; ++m)
        for (const stl_vertex &v : meshes[m].vertices) {
            low  = low.cwiseMin(v);
            high = high.cwiseMax(v);
        }
    if (!(low.x() <= high.x()) || (high - low).norm() <= 0.f)
        return image;
    const Vec3f center    = 0.5f * (low + high);
    const float scale     = 0.92f * float(std::min(w, h)) / (high - low).norm();
    const float cos_yaw   = float(std::cos(yaw));
    const float sin_yaw   = float(std::sin(yaw));
    const float cos_pitch = float(std::cos(pitch));
    const float sin_pitch = float(std::sin(pitch));
    // The viewer looks along +Y, the light comes from behind them, above, on the left.
    const Vec3f light  = Vec3f(-0.4f, -0.75f, 0.55f).normalized();
    const float dim    = dark ? 95.f : 70.f;
    const float bright = dark ? 250.f : 175.f;
    std::vector<float> depth(size_t(w) * size_t(h), std::numeric_limits<float>::max());
    std::vector<Vec3f> view;
    size_t             index = 0;
    for (size_t m = 0; m < count; ++m) {
        const indexed_triangle_set &its = meshes[m];
        view.clear();
        view.reserve(its.vertices.size());
        for (const stl_vertex &v : its.vertices) {
            const Vec3f q = v - center;
            const float x = q.x() * cos_yaw - q.y() * sin_yaw;
            const float y = q.x() * sin_yaw + q.y() * cos_yaw;
            view.emplace_back(x, y * cos_pitch - q.z() * sin_pitch, y * sin_pitch + q.z() * cos_pitch);
        }
        for (size_t f = 0; f < its.indices.size(); ++f, ++index) {
            const stl_triangle_vertex_indices &face   = its.indices[f];
            const Vec3f                       &a      = view[face[0]];
            const Vec3f                       &b      = view[face[1]];
            const Vec3f                       &c      = view[face[2]];
            const Vec3f                        normal = (b - a).cross(c - a);
            const float                        length = normal.norm();
            if (length <= 0.f)
                continue;
            const float   lit = std::abs(normal.dot(light)) / length;
            unsigned char shade[3];
            if (index < kinds.size()) {
                // The color of the kind of surface, darker away from the light.
                const wxColour &colour = SURFACE[std::min<size_t>(kinds[index], 9)];
                const float     factor = 0.45f + 0.55f * lit;
                shade[0]               = (unsigned char) (colour.Red() * factor);
                shade[1]               = (unsigned char) (colour.Green() * factor);
                shade[2]               = (unsigned char) (colour.Blue() * factor);
            } else
                shade[0] = shade[1] = shade[2] = (unsigned char) (dim + (bright - dim) * lit);
            // On the screen: x to the right, y down.
            const float sx[3] = { 0.5f * w + scale * a.x(), 0.5f * w + scale * b.x(), 0.5f * w + scale * c.x() };
            const float sy[3] = { 0.5f * h - scale * a.z(), 0.5f * h - scale * b.z(), 0.5f * h - scale * c.z() };
            const float area  = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
            if (area == 0.f)
                continue;
            const float sign = area > 0.f ? 1.f : -1.f;
            const int   x0   = std::max(0, int(std::floor(std::min({ sx[0], sx[1], sx[2] }))));
            const int   x1   = std::min(w - 1, int(std::ceil(std::max({ sx[0], sx[1], sx[2] }))));
            const int   y0   = std::max(0, int(std::floor(std::min({ sy[0], sy[1], sy[2] }))));
            const int   y1   = std::min(h - 1, int(std::ceil(std::max({ sy[0], sy[1], sy[2] }))));
            for (int py = y0; py <= y1; ++py)
                for (int px = x0; px <= x1; ++px) {
                    const float fx = px + 0.5f;
                    const float fy = py + 0.5f;
                    // Weights of a, b and c: twice the areas of the triangles facing them.
                    const float wa = sign * ((sx[2] - sx[1]) * (fy - sy[1]) - (sy[2] - sy[1]) * (fx - sx[1]));
                    const float wb = sign * ((sx[0] - sx[2]) * (fy - sy[2]) - (sy[0] - sy[2]) * (fx - sx[2]));
                    const float wc = sign * ((sx[1] - sx[0]) * (fy - sy[0]) - (sy[1] - sy[0]) * (fx - sx[0]));
                    if (wa < 0.f || wb < 0.f || wc < 0.f)
                        continue;
                    // Depth along the view: the nearer, the smaller.
                    const float z       = (wa * a.y() + wb * b.y() + wc * c.y()) / (sign * area);
                    float      &nearest = depth[size_t(py) * w + px];
                    if (z >= nearest)
                        continue;
                    nearest              = z;
                    unsigned char *pixel = pixels + 3 * (size_t(py) * w + px);
                    pixel[0]             = shade[0];
                    pixel[1]             = shade[1];
                    pixel[2]             = shade[2];
                }
        }
    }
    return image;
}

// A picture kept in cache, drawn again when forgotten or resized; finer when fine (drawn twice as large, then reduced).
void draw_cached(wxGraphicsContext &gc, const wxRect2DDouble &area, wxBitmap &cache, bool fine, const std::function<wxImage(int, int)> &render)
{
    const wxSize size(int(area.m_width), int(area.m_height));
    if (!cache.IsOk() || cache.GetSize() != size) {
        const int quality = fine ? 2 : 1;
        wxImage   image   = render(size.x * quality, size.y * quality);
        if (quality > 1)
            image.Rescale(size.x, size.y, wxIMAGE_QUALITY_BOX_AVERAGE);
        cache = wxBitmap(image);
    }
    gc.DrawBitmap(cache, area.m_x, area.m_y, size.x, size.y);
}

// Files dropped on the tab: loaded once the drop is over, as PlaterDropTarget does. A mesh alone is converted into
// STEP in this tab; the G-codes come back to this tab, the other files go to the Prepare tab.
class DropTarget : public wxFileDropTarget
{
public:
    explicit DropTarget(GCodeToMeshPanel *panel) : m_panel(panel) { SetDefaultAction(wxDragCopy); }

    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString &files) override
    {
        wxGetApp().CallAfter([files, panel = m_panel]() {
            wxGetApp().mainframe->Raise();
            if (files.size() == 1 && is_mesh_file(files[0]))
                panel->open_mesh(files[0]);
            else
                wxGetApp().plater()->load_files(files);
        });
        return true;
    }

private:
    GCodeToMeshPanel *m_panel;
};

// Drawing.

using Points2 = std::vector<wxPoint2DDouble>;

wxColour with_alpha(const wxColour &c, unsigned char alpha) { return wxColour(c.Red(), c.Green(), c.Blue(), alpha); }

// Fits a drawing of w x h into the area, centered.
void fit(wxGraphicsContext &gc, const wxRect2DDouble &area, double w, double h)
{
    const double scale = std::min(area.m_width / w, area.m_height / h);
    gc.Translate(area.m_x + (area.m_width - w * scale) / 2., area.m_y + (area.m_height - h * scale) / 2.);
    gc.Scale(scale, scale);
}

wxGraphicsPath make_path(wxGraphicsContext &gc, const Points2 &pts, bool closed)
{
    wxGraphicsPath path = gc.CreatePath();
    path.MoveToPoint(pts.front());
    for (size_t i = 1; i < pts.size(); ++i)
        path.AddLineToPoint(pts[i]);
    if (closed)
        path.CloseSubpath();
    return path;
}

void fill_path(wxGraphicsContext &gc, const Points2 &pts, const wxColour &fill)
{
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(fill));
    gc.FillPath(make_path(gc, pts, true));
}

void stroke_path(wxGraphicsContext &gc, const Points2 &pts, const wxColour &colour, double width, bool closed, bool dashed = false)
{
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour, width, dashed ? wxPENSTYLE_SHORT_DASH : wxPENSTYLE_SOLID).Join(wxJOIN_ROUND).Cap(wxCAP_ROUND)));
    gc.StrokePath(make_path(gc, pts, closed));
}

void fill_round_rect(wxGraphicsContext &gc, double x, double y, double w, double h, double r, const wxColour &fill)
{
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(fill));
    gc.DrawRoundedRectangle(x, y, w, h, r);
}

void stroke_round_rect(wxGraphicsContext &gc, double x, double y, double w, double h, double r, const wxColour &colour, double width, bool dashed = false)
{
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour, width, dashed ? wxPENSTYLE_SHORT_DASH : wxPENSTYLE_SOLID)));
    gc.DrawRoundedRectangle(x, y, w, h, r);
}

void fill_circle(wxGraphicsContext &gc, double x, double y, double r, const wxColour &fill)
{
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(fill));
    gc.DrawEllipse(x - r, y - r, 2. * r, 2. * r);
}

void stroke_circle(wxGraphicsContext &gc, double x, double y, double r, const wxColour &colour, double width, bool dashed = false)
{
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(colour, width, dashed ? wxPENSTYLE_SHORT_DASH : wxPENSTYLE_SOLID)));
    gc.DrawEllipse(x - r, y - r, 2. * r, 2. * r);
}

// Text centered on y, left aligned (align < 0) or centered (align == 0) on x. Returns its width.
double draw_text(wxGraphicsContext &gc, const wxString &text, double x, double y, const wxColour &colour, const wxFont &font, int align)
{
    gc.SetFont(font, colour);
    double w = 0., h = 0.;
    gc.GetTextExtent(text, &w, &h);
    gc.DrawText(text, align < 0 ? x : x - w / 2., y - h / 2.);
    return w;
}

void draw_bitmap(wxGraphicsContext &gc, const wxRect2DDouble &area, const wxBitmap &bitmap)
{
    const double w = bitmap.GetWidth();
    const double h = bitmap.GetHeight();
    if (w <= 0. || h <= 0.)
        return;
    const double scale = std::min(area.m_width / w, area.m_height / h);
    gc.DrawBitmap(bitmap, area.m_x + (area.m_width - w * scale) / 2., area.m_y + (area.m_height - h * scale) / 2., w * scale, h * scale);
}

// Isometric view: x to the lower right, y to the lower left, z up.
struct Iso
{
    double cx;
    double cy;
    double s;

    wxPoint2DDouble operator()(double x, double y, double z) const { return wxPoint2DDouble(cx + (x - y) * 0.866 * s, cy + (x + y) * 0.5 * s - z * s); }
};

// A box seen from above, its top, left and right faces in the colors c[0], c[1], c[2].
void iso_box(wxGraphicsContext &gc, const Iso &iso, double x, double y, double z, double w, double d, double h, const wxColour *c)
{
    fill_path(gc, { iso(x, y, z + h), iso(x + w, y, z + h), iso(x + w, y + d, z + h), iso(x, y + d, z + h) }, c[0]);
    fill_path(gc, { iso(x, y + d, z), iso(x + w, y + d, z), iso(x + w, y + d, z + h), iso(x, y + d, z + h) }, c[1]);
    fill_path(gc, { iso(x + w, y, z), iso(x + w, y + d, z), iso(x + w, y + d, z + h), iso(x + w, y, z + h) }, c[2]);
}

// Layers of beads seen from the side, in the colors of the Preview: outer wall, inner wall, sparse infill (200 x 120).
void draw_beads(wxGraphicsContext &gc)
{
    for (int i = 0; i < 8; ++i) {
        const double y = 100. - i * 11.;
        const double l = 30. + i * 3.;
        const double r = 170. - i * 3.;
        fill_round_rect(gc, l, y, 15., 9., 4.5, OUTER_WALL);
        fill_round_rect(gc, l + 15., y, 13., 9., 4.5, INNER_WALL);
        for (double x = l + 30.; x <= r - 40.; x += 12.)
            fill_round_rect(gc, x, y + 2., 10., 5., 2.5, SPARSE_INFILL);
        fill_round_rect(gc, r - 28., y, 13., 9., 4.5, INNER_WALL);
        fill_round_rect(gc, r - 15., y, 15., 9., 4.5, OUTER_WALL);
    }
}

// A block of facets in the accent color (200 x 120).
void draw_mesh_block(wxGraphicsContext &gc, const wxColour &accent)
{
    auto face = [&gc, &accent](const Points2 &pts, unsigned char alpha) {
        fill_path(gc, pts, with_alpha(accent, alpha));
        stroke_path(gc, pts, accent, 1., true);
    };
    face({ { 100., 16. }, { 160., 41. }, { 100., 66. }, { 40., 41. } }, 38);
    face({ { 40., 41. }, { 100., 66. }, { 100., 104. }, { 40., 79. } }, 82);
    face({ { 100., 66. }, { 160., 41. }, { 160., 79. }, { 100., 104. } }, 128);
    const wxColour edge = with_alpha(accent, 190);
    stroke_path(gc, { { 40., 41. }, { 160., 41. } }, edge, 0.7, false);
    stroke_path(gc, { { 100., 16. }, { 100., 66. } }, edge, 0.7, false);
    stroke_path(gc, { { 40., 79. }, { 100., 66. } }, edge, 0.7, false);
    stroke_path(gc, { { 100., 104. }, { 160., 41. } }, edge, 0.7, false);
}

// A G-code file: a sheet with lines in the colors of the Preview (22 x 26).
void paint_file_icon(wxGraphicsContext &gc, const wxRect2DDouble &area, const wxColour &accent)
{
    fit(gc, area, 22., 26.);
    stroke_round_rect(gc, 2., 1., 18., 24., 3., accent, 1.5);
    const double   widths[]  = { 11., 8., 10. };
    const wxColour colours[] = { OUTER_WALL, INNER_WALL, SPARSE_INFILL };
    for (int i = 0; i < 3; ++i)
        fill_round_rect(gc, 5.5, 7. + i * 5., widths[i], 2.4, 1.2, colours[i]);
}

wxColour mix(const wxColour &a, const wxColour &b, double t)
{
    return wxColour((unsigned char) (a.Red() + (b.Red() - a.Red()) * t), (unsigned char) (a.Green() + (b.Green() - a.Green()) * t),
                    (unsigned char) (a.Blue() + (b.Blue() - a.Blue()) * t));
}

// A cylinder seen from above (200 x 120), as a true cylinder and its plane, in the colors of their kinds of surfaces.
void paint_cylinder(wxGraphicsContext &gc, const wxRect2DDouble &area)
{
    fit(gc, area, 200., 120.);
    const Iso    iso{ 100., 82., 1. };
    const double R = 40.;
    const double H = 46.;
    const int    n = 72;
    Points2      top;
    for (int i = 0; i < n; ++i) {
        const double a0 = 2. * PI * i / n;
        const double a1 = 2. * PI * (i + 1) / n;
        top.push_back(iso(R * std::cos(a0), R * std::sin(a0), H));
        // The sides facing the viewer (toward +x +y), lit from the left (+y).
        const double am = 0.5 * (a0 + a1);
        if (std::cos(am) + std::sin(am) <= 0.)
            continue;
        const double   lit  = 0.5 + 0.5 * std::sin(am);
        const Points2  side = { iso(R * std::cos(a0), R * std::sin(a0), 0.), iso(R * std::cos(a1), R * std::sin(a1), 0.),
                                iso(R * std::cos(a1), R * std::sin(a1), H), iso(R * std::cos(a0), R * std::sin(a0), H) };
        const wxColour fill = mix(mix(*wxBLACK, SURFACE[1], 0.55), SURFACE[1], lit);
        fill_path(gc, side, fill);
        // No seam between the thin sides.
        stroke_path(gc, side, fill, 0.6, true);
    }
    fill_path(gc, top, SURFACE[0]);
}

} // namespace

// Colors of the page, in the light and dark themes of Orio.
struct GCodeToMeshPanel::Palette
{
    wxColour page;
    wxColour card;
    wxColour well;
    wxColour border;
    wxColour strong;
    wxColour text;
    wxColour text2;
    wxColour accent;
    wxColour pale;
    wxColour plate[3];
};

GCodeToMeshPanel::Palette GCodeToMeshPanel::palette()
{
    const bool dark = wxGetApp().dark_mode();
    auto       c    = [dark](const char *light, const char *dark_colour) { return wxColour(dark ? dark_colour : light); };
    Palette    pal;
    pal.page     = c("#F2F3F5", "#18181B");
    pal.card     = c("#FFFFFF", "#29292E");
    pal.well     = c("#F7F8FA", "#202024");
    pal.border   = c("#E1E3E6", "#34343B");
    pal.strong   = c("#C4C8CE", "#4A4A52");
    pal.text     = c("#262E30", "#EDEDEF");
    pal.text2    = c("#6B6B6B", "#A5A5AB");
    pal.accent   = c("#0877E8", "#00C2FF");
    pal.pale     = c("#EAF3FD", "#243D49");
    pal.plate[0] = c("#C9CDD2", "#5B5B66");
    pal.plate[1] = c("#B3B8BF", "#4B4B54");
    pal.plate[2] = c("#A2A7AF", "#40404A");
    return pal;
}

// A drawing, on a rounded well of the card (or bare), repainted on demand.
class GCodeToMeshPanel::Canvas : public wxPanel
{
public:
    using Paint = std::function<void(wxGraphicsContext &, const Palette &, const wxRect2DDouble &)>;

    Canvas(wxWindow *parent, const wxSize &size, Paint paint, bool well = true)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size), m_paint(std::move(paint)), m_well(well)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(size);
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc)
                return;
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            gc->SetInterpolationQuality(wxINTERPOLATION_GOOD);
            const Palette  pal  = palette();
            const wxSize   size = GetClientSize();
            wxRect2DDouble area(0., 0., size.x, size.y);
            if (m_well) {
                fill_round_rect(*gc, 0., 0., size.x, size.y, FromDIP(8), pal.well);
                const double pad = FromDIP(6);
                area             = wxRect2DDouble(pad, pad, size.x - 2. * pad, size.y - 2. * pad);
            }
            if (area.m_width > 1. && area.m_height > 1.)
                m_paint(*gc, pal, area);
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
            Refresh();
            event.Skip();
        });
    }

private:
    Paint m_paint;
    bool  m_well;
};

// A rounded card, its border in the accent color while the mouse is over it or over anything in it.
class GCodeToMeshPanel::Card : public wxPanel
{
public:
    Card(wxWindow *parent, std::function<void(bool)> on_hover) : wxPanel(parent, wxID_ANY), m_on_hover(std::move(on_hover))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(this);
            const Palette         pal = palette();
            dc.SetBackground(wxBrush(pal.page));
            dc.Clear();
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc)
                return;
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            const wxSize size = GetClientSize();
            gc->SetBrush(wxBrush(pal.card));
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(m_hovered ? pal.accent : pal.border, FromDIP(1))));
            gc->DrawRoundedRectangle(0.5, 0.5, size.x - 1., size.y - 1., FromDIP(14));
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
            Refresh();
            event.Skip();
        });
    }

    void set_hovered(bool hovered)
    {
        if (m_hovered != hovered) {
            m_hovered = hovered;
            Refresh();
        }
    }

    // The mouse entering the window or leaving it, for the window and everything in it.
    void watch(wxWindow *window)
    {
        window->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &event) {
            m_on_hover(true);
            event.Skip();
        });
        window->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &event) {
            event.Skip();
            // Leaving a window of the card for another one of the card is not leaving the card.
            CallAfter([this]() {
                if (!GetScreenRect().Contains(wxGetMousePosition()))
                    m_on_hover(false);
            });
        });
        for (wxWindow *child : window->GetChildren())
            watch(child);
    }

private:
    std::function<void(bool)> m_on_hover;
    bool                      m_hovered{ false };
};

// A frame around the cards of a row still in beta: a line in the yellow of the "Beta" badges of the tabs, that badge on
// its top left, the cards inside the line and under the badge.
class GCodeToMeshPanel::BetaFrame : public wxPanel
{
public:
    // Between the line and the cards, on the sides and at the bottom.
    static constexpr int MARGIN = 12;

    explicit BetaFrame(wxWindow *parent) : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(palette().page));
            dc.Clear();
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc)
                return;
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            const wxSize size  = GetClientSize();
            const double line  = 1.5 * FromDIP(100) / 100.;
            const double badge = badge_height();
            stroke_round_rect(*gc, line / 2., badge / 2., size.x - line, size.y - badge / 2. - line / 2., FromDIP(20), BETA_YELLOW, line);
            gc->SetFont(Label::Head_10, BETA_TEXT);
            double w = 0., h = 0.;
            gc->GetTextExtent(_L("Beta"), &w, &h);
            w += FromDIP(10);
            fill_round_rect(*gc, FromDIP(20), 0., w, badge, badge / 2., BETA_YELLOW);
            draw_text(*gc, _L("Beta"), FromDIP(20) + w / 2., badge / 2., BETA_TEXT, Label::Head_10, 0);
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
            Refresh();
            event.Skip();
        });
    }

    void hold(wxSizer *cards)
    {
        auto *sizer = new wxBoxSizer(wxVERTICAL);
        sizer->AddSpacer(badge_height() + FromDIP(8));
        sizer->Add(cards, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(MARGIN));
        SetSizer(sizer);
    }

private:
    // As high as the badges of the tabs (Button::badgeSize).
    int badge_height() const
    {
        int w = 0, h = 0;
        GetTextExtent(_L("Beta"), &w, &h, nullptr, nullptr, &Label::Head_10);
        return h + FromDIP(2);
    }
};

// The three steps above the cards: a dot and a label each, linked. The dot of a done step is filled and checked, the one
// of the current step is a ring in the accent color, the one of the card under the mouse too.
class GCodeToMeshPanel::Rail : public wxPanel
{
public:
    explicit Rail(wxWindow *parent) : wxPanel(parent, wxID_ANY)
    {
        m_labels = { _L("Open"), _L("Read and adjust"), _L("Transform") };
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, FromDIP(40)));
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
            Refresh();
            event.Skip();
        });
    }

    void set(bool opened, int hovered)
    {
        if (opened != m_opened || hovered != m_hovered) {
            m_opened  = opened;
            m_hovered = hovered;
            Refresh();
        }
    }

    // The steps of a G-code, or of a mesh to convert into STEP.
    void set_labels(std::vector<wxString> labels)
    {
        if (labels != m_labels) {
            m_labels = std::move(labels);
            Refresh();
        }
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        const Palette         pal = palette();
        dc.SetBackground(wxBrush(pal.page));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        const double dip = FromDIP(100) / 100.;
        const double r   = 12. * dip;
        const double gap = 7. * dip;
        const double cy  = GetClientSize().y / 2.;
        double       widths[3];
        double       total = 0.;
        gc->SetFont(Label::Body_14, pal.text);
        for (int i = 0; i < 3; ++i) {
            double h = 0.;
            gc->GetTextExtent(m_labels[i], &widths[i], &h);
            total += 2. * r + gap + widths[i];
        }
        const double link = std::max(12. * dip, (GetClientSize().x - total - 4. * gap) / 2.);
        // 0 to come, 1 current, 2 done.
        const int state[3] = { m_opened ? 2 : 1, m_opened ? 1 : 0, 0 };
        double    x        = 0.;
        for (int i = 0; i < 3; ++i) {
            const bool   hovered = m_hovered == i + 1;
            const double cx      = x + r;
            if (state[i] == 2) {
                fill_circle(*gc, cx, cy, r, pal.accent);
                stroke_path(*gc, { { cx - 0.42 * r, cy + 0.02 * r }, { cx - 0.1 * r, cy + 0.34 * r }, { cx + 0.45 * r, cy - 0.3 * r } }, pal.card, 2. * dip, false);
            } else {
                const bool lit = state[i] == 1 || hovered;
                stroke_circle(*gc, cx, cy, r - 0.75 * dip, lit ? pal.accent : pal.strong, 1.5 * dip);
                draw_text(*gc, wxString::Format("%d", i + 1), cx, cy, lit ? pal.accent : pal.text2, Label::Body_13, 0);
            }
            x += 2. * r + gap;
            draw_text(*gc, m_labels[i], x, cy, hovered || state[i] != 0 ? pal.text : pal.text2, Label::Body_14, -1);
            x += widths[i] + gap;
            if (i < 2) {
                gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(i == 0 && m_opened ? pal.accent : pal.strong, 1.5 * dip)));
                gc->StrokeLine(x, cy, x + link, cy);
                x += link + gap;
            }
        }
    }

    std::vector<wxString> m_labels;
    bool                  m_opened{ false };
    int                   m_hovered{ 0 };
};

// Small pills to pick one of a few labels.
class GCodeToMeshPanel::Chips : public wxPanel
{
public:
    Chips(wxWindow *parent, std::vector<wxString> labels, std::function<void(int)> on_select)
        : wxPanel(parent, wxID_ANY), m_labels(std::move(labels)), m_enabled(m_labels.size(), true), m_on_select(std::move(on_select))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, FromDIP(28)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
            for (size_t i = 0; i < m_boxes.size(); ++i)
                if (m_enabled[i] && m_boxes[i].Contains(event.GetPosition())) {
                    set_selected(int(i));
                    m_on_select(int(i));
                    break;
                }
        });
    }

    void set_selected(int index)
    {
        m_selected = index;
        Refresh();
    }

    void set_enabled(int index, bool enabled)
    {
        m_enabled[index] = enabled;
        Refresh();
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        const Palette         pal = palette();
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        const double        dip = FromDIP(100) / 100.;
        std::vector<double> widths;
        double              total = 0.;
        gc->SetFont(Label::Body_12, pal.text2);
        for (const wxString &label : m_labels) {
            double w = 0., h = 0.;
            gc->GetTextExtent(label, &w, &h);
            widths.emplace_back(w + 16. * dip);
            total += widths.back() + 4. * dip;
        }
        const wxSize size = GetClientSize();
        const double h    = 22. * dip;
        const double y    = (size.y - h) / 2.;
        double       x    = std::max(0., (size.x - total + 4. * dip) / 2.);
        m_boxes.clear();
        for (size_t i = 0; i < m_labels.size(); ++i) {
            const bool     on     = int(i) == m_selected;
            const wxColour colour = !m_enabled[i] ? pal.border : on ? pal.accent : pal.strong;
            stroke_round_rect(*gc, x + 0.5, y + 0.5, widths[i] - 1., h - 1., h / 2., colour, dip);
            draw_text(*gc, m_labels[i], x + widths[i] / 2., y + h / 2., !m_enabled[i] ? pal.border : on ? pal.accent : pal.text2, Label::Body_12, 0);
            m_boxes.emplace_back(int(x), int(y), int(widths[i]), int(h));
            x += widths[i] + 4. * dip;
        }
    }

    std::vector<wxString>    m_labels;
    std::vector<bool>        m_enabled;
    std::function<void(int)> m_on_select;
    std::vector<wxRect>      m_boxes;
    int                      m_selected{ 0 };
};

// A setting: a check box, its title and a line under it. The whole row can be clicked; it is highlighted while its
// illustration is shown.
class GCodeToMeshPanel::Option : public wxPanel
{
public:
    Option(wxWindow *parent, const wxString &title, std::function<void()> on_hover, std::function<void()> on_toggle)
        : wxPanel(parent, wxID_ANY), m_on_hover(std::move(on_hover)), m_on_toggle(std::move(on_toggle))
    {
        m_check = new ::CheckBox(this);
        m_check->SetValue(true);
        m_title = new wxStaticText(this, wxID_ANY, title);
        m_title->SetFont(Label::Body_14);
        m_subtitle = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        m_subtitle->SetFont(Label::Body_12);
        m_subtitle->SetMinSize(wxSize(FromDIP(20), -1));
        auto *texts = new wxBoxSizer(wxVERTICAL);
        texts->Add(m_title, 0);
        texts->Add(m_subtitle, 0, wxEXPAND | wxTOP, FromDIP(1));
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(m_check, 0, wxTOP, FromDIP(1));
        row->Add(texts, 1, wxLEFT, FromDIP(8));
        auto *outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(row, 0, wxEXPAND | wxALL, FromDIP(6));
        SetSizer(outer);

        // Skipped: the check box draws its new state in its own handler.
        m_check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &event) {
            event.Skip();
            m_on_toggle();
        });
        for (wxWindow *window : std::initializer_list<wxWindow *>{ this, m_title, m_subtitle }) {
            window->SetCursor(wxCursor(wxCURSOR_HAND));
            window->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
                if (m_check->IsEnabled()) {
                    m_check->SetValue(!m_check->GetValue());
                    m_on_toggle();
                }
            });
        }
        for (wxWindow *window : std::initializer_list<wxWindow *>{ this, m_check, m_title, m_subtitle })
            window->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &event) {
                m_on_hover();
                event.Skip();
            });
    }

    bool value() const { return m_check->GetValue(); }
    void set_subtitle(const wxString &text)
    {
        m_subtitle->SetLabelText(text);
        m_subtitle->SetToolTip(text);
        // The label resized it to its whole width: back to the width of the row, ellipsized.
        Layout();
    }
    // The pictures of the check box, in the colors of the theme.
    void rescale() { m_check->Rescale(); }
    void set_tooltip(const wxString &text)
    {
        for (wxWindow *window : std::initializer_list<wxWindow *>{ this, m_check, m_title })
            window->SetToolTip(text);
    }
    void enable(bool enabled)
    {
        m_check->Enable(enabled);
        apply();
    }
    void set_selected(bool selected)
    {
        if (m_selected != selected) {
            m_selected = selected;
            apply();
        }
    }
    void apply()
    {
        const Palette  pal        = palette();
        const wxColour background = m_selected ? pal.pale : pal.card;
        for (wxWindow *window : std::initializer_list<wxWindow *>{ this, m_check, m_title, m_subtitle })
            window->SetBackgroundColour(background);
        m_title->SetForegroundColour(m_check->IsEnabled() ? pal.text : pal.text2);
        m_subtitle->SetForegroundColour(pal.text2);
        Refresh();
    }

private:
    std::function<void()> m_on_hover;
    std::function<void()> m_on_toggle;
    ::CheckBox           *m_check{ nullptr };
    wxStaticText         *m_title{ nullptr };
    wxStaticText         *m_subtitle{ nullptr };
    bool                  m_selected{ false };
};

GCodeToMeshPanel::GCodeToMeshPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
{
    build_ui();
    // The animations of the empty cards, only while they are seen.
    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
        if (!IsShownOnScreen())
            return;
        if (m_path.IsEmpty()) {
            m_drop->Refresh(false);
            m_scan->Refresh(false);
        }
        if (!m_mesh_its)
            m_mesh_drop->Refresh(false);
    });
    update();
}

GCodeToMeshPanel::~GCodeToMeshPanel()
{
    m_timer.Stop();
    // The jobs under way are stopped, their threads waited for.
    m_gcode_worker.quit();
    m_step_worker.quit();
}

void GCodeToMeshPanel::build_ui()
{
    const int em         = wxGetApp().em_unit();
    auto     *main_sizer = new wxBoxSizer(wxVERTICAL);
    m_page               = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxHSCROLL);
    m_page->SetScrollRate(em, em);
    m_page->SetDropTarget(new DropTarget(this));
    m_column = new wxBoxSizer(wxVERTICAL);
    // The illustrations of the cards all have the same height, set by rewrap() from the size of the page.
    m_well_height = FromDIP(150);

    // A row per conversion, one under the other: its name, the file open or what it is for, its three steps, a card for
    // each one, its notes.
    for (int r = 0; r < 2; ++r) {
        Row &row = m_rows[r];
        if (r > 0)
            m_column->AddSpacer(FromDIP(30));
        row.label = add_text(m_page, r == ROW_GCODE ? _L("G-code to mesh") : _L("Mesh to STEP"), Label::Head_14, ACCENT, false, false);
        m_column->Add(row.label, 0, wxBOTTOM, FromDIP(8));
        auto *head = new wxBoxSizer(wxHORIZONTAL);
        row.icon   = new Canvas(
            m_page, FromDIP(wxSize(26, 31)),
            [r](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) {
                if (r == ROW_GCODE)
                    paint_file_icon(gc, area, pal.accent);
                else {
                    fit(gc, area, 200., 120.);
                    draw_mesh_block(gc, pal.accent);
                }
            },
            false);
        head->Add(row.icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        auto *titles = new wxBoxSizer(wxVERTICAL);
        row.title    = add_text(m_page, wxEmptyString, Label::Head_18, PLAIN, false, false, wxST_ELLIPSIZE_MIDDLE);
        row.subtitle = add_text(m_page, wxEmptyString, Label::Body_13, SECONDARY, false, false, wxST_ELLIPSIZE_END);
        titles->Add(row.title, 0, wxEXPAND);
        titles->Add(row.subtitle, 0, wxEXPAND | wxTOP, FromDIP(2));
        head->Add(titles, 1, wxALIGN_CENTER_VERTICAL);
        m_column->Add(head, 0, wxEXPAND);
        row.rail = new Rail(m_page);
        if (r == ROW_STEP)
            row.rail->set_labels({ _L("Open"), _L("Surfaces"), _L("STEP") });
        m_column->Add(row.rail, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));
        // The cards of the conversion still in beta, in a yellow frame.
        BetaFrame *beta  = r == ROW_STEP ? new BetaFrame(m_page) : nullptr;
        auto      *cards = new wxGridSizer(1, 3, 0, FromDIP(12));
        for (int i = 0; i < 3; ++i) {
            row.cards[i]  = new Card(beta ? static_cast<wxWindow *>(beta) : m_page, [this, r, i](bool hovered) { hover_card(r, i, hovered); });
            row.arrive[i] = new wxPanel(row.cards[i]);
            row.opened[i] = new wxPanel(row.cards[i]);
            auto *sizer   = new wxBoxSizer(wxVERTICAL);
            sizer->Add(row.arrive[i], 1, wxEXPAND | wxALL, FromDIP(14));
            sizer->Add(row.opened[i], 1, wxEXPAND | wxALL, FromDIP(14));
            row.cards[i]->SetSizer(sizer);
            cards->Add(row.cards[i], 1, wxEXPAND);
        }
        if (beta) {
            beta->hold(cards);
            m_column->Add(beta, 0, wxEXPAND);
        } else
            m_column->Add(cards, 0, wxEXPAND);
        row.notes = add_text(m_page, wxEmptyString, Label::Body_13, SECONDARY, false, true);
        m_column->Add(row.notes, 0, wxEXPAND | wxTOP, FromDIP(12));
    }
    build_open_card();
    build_read_card();
    build_transform_card();
    build_mesh_arrive_cards();
    build_convert_cards();
    for (const Row &row : m_rows)
        for (Card *card : row.cards)
            card->watch(card);

    // The column in the middle of the page, both ways.
    auto *page_sizer = new wxBoxSizer(wxVERTICAL);
    page_sizer->AddStretchSpacer(1);
    page_sizer->Add(m_column, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 2 * em);
    page_sizer->AddStretchSpacer(1);
    m_page->SetSizer(page_sizer);
    main_sizer->Add(m_page, 1, wxEXPAND);
    SetSizer(main_sizer);

    m_page->Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
        event.Skip();
        rewrap(false);
    });
}

void GCodeToMeshPanel::build_open_card()
{
    const int gap = FromDIP(8);

    // Before: where to drop the G-code, the button, the slicers read, the G-codes opened lately.
    wxWindow *arrive = m_rows[ROW_GCODE].arrive[0];
    auto     *a      = new wxBoxSizer(wxVERTICAL);
    a->Add(make_title(arrive, 1, _L("Open a G-code")), 0, wxEXPAND);
    m_drop = new Canvas(arrive, wxSize(-1, m_well_height), [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) {
        paint_scene(gc, pal, area, Scene::Drop);
    });
    a->Add(m_drop, 0, wxEXPAND | wxTOP, gap);
    a->Add(add_text(arrive, _L("Drop a G-code here"), Label::Body_13, PLAIN, true, false), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, gap);
    m_open_button = make_button(arrive, _L("Open a G-code..."), true);
    m_open_button->SetToolTip(_L("Open a .gcode file. It can also be dropped into the window."));
    m_open_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_file(); });
    a->Add(m_open_button, 0, wxEXPAND | wxTOP, gap);
    a->Add(add_text(arrive, wxString::FromUTF8("Bambu Studio \xC2\xB7 Orca Slicer \xC2\xB7 PrusaSlicer \xC2\xB7 Orio"), Label::Body_12, SECONDARY, true, true,
                    wxALIGN_CENTRE_HORIZONTAL),
           0, wxEXPAND | wxTOP, FromDIP(6));
    m_recent_title = add_text(arrive, _L("Recent"), Label::Body_13, SECONDARY, true, false);
    a->Add(m_recent_title, 0, wxTOP, gap);
    m_recent = new wxPanel(arrive);
    m_recent->SetSizer(new wxBoxSizer(wxVERTICAL));
    a->Add(m_recent, 0, wxEXPAND | wxTOP, FromDIP(2));
    arrive->SetSizer(a);

    // Then: the picture of the print, what the G-code says, another G-code.
    wxWindow *opened = m_rows[ROW_GCODE].opened[0];
    auto     *o      = new wxBoxSizer(wxVERTICAL);
    o->Add(make_title(opened, 1, _L("The G-code")), 0, wxEXPAND);
    m_picture = new Canvas(opened, wxSize(-1, m_well_height), [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) {
        paint_scene(gc, pal, area, m_view);
    });
    o->Add(m_picture, 0, wxEXPAND | wxTOP, gap);
    m_views = new Chips(opened, { _L("Thumbnail"), _L("Top view"), _L("Side view") }, [this](int index) {
        m_view = index == 0 ? Scene::Thumbnail : index == 1 ? Scene::Top : Scene::Profile;
        m_picture->Refresh();
    });
    o->Add(m_views, 0, wxEXPAND | wxTOP, FromDIP(6));
    auto *grid = new wxFlexGridSizer(2, FromDIP(4), FromDIP(10));
    grid->AddGrowableCol(1, 1);
    const wxString labels[] = { _L("Software"), _L("Printer"), _L("Nozzle"), _L("Outer wall width"), _L("Layers"), _L("Height"), _L("Objects") };
    for (int i = 0; i < 7; ++i) {
        grid->Add(add_text(opened, labels[i], Label::Body_13, SECONDARY, true, false), 0);
        m_info[i] = add_text(opened, wxEmptyString, Label::Body_13, PLAIN, true, false, wxST_ELLIPSIZE_END);
        grid->Add(m_info[i], 1, wxEXPAND);
    }
    o->Add(grid, 0, wxEXPAND | wxTOP, gap);
    o->AddStretchSpacer(1);
    m_change_button = make_button(opened, _L("Change G-code..."), false);
    m_change_button->SetToolTip(_L("Open a .gcode file. It can also be dropped into the window."));
    m_change_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_file(); });
    o->Add(m_change_button, 0, wxEXPAND | wxTOP, gap);
    opened->SetSizer(o);
}

void GCodeToMeshPanel::build_read_card()
{
    const int gap = FromDIP(8);

    // Before: the layers being read.
    wxWindow *arrive = m_rows[ROW_GCODE].arrive[1];
    auto     *a      = new wxBoxSizer(wxVERTICAL);
    a->Add(make_title(arrive, 2, _L("Read and adjust")), 0, wxEXPAND);
    m_scan = new Canvas(arrive, wxSize(-1, m_well_height), [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) {
        paint_scene(gc, pal, area, Scene::Scan);
    });
    a->Add(m_scan, 0, wxEXPAND | wxTOP, gap);
    a->Add(add_text(arrive, _L("Orio reads in the G-code the outer wall of each layer, its line width, the nozzle and the compensations of the slicer."),
                    Label::Body_13, SECONDARY, true, true),
           0, wxEXPAND | wxTOP, gap);
    arrive->SetSizer(a);

    // Then: the setting under the mouse, illustrated as it is set, and the settings.
    wxWindow *opened = m_rows[ROW_GCODE].opened[1];
    auto     *o      = new wxBoxSizer(wxVERTICAL);
    o->Add(make_title(opened, 2, _L("Adjust")), 0, wxEXPAND);
    m_setting = new Canvas(opened, wxSize(-1, m_well_height), [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) {
        paint_scene(gc, pal, area, m_focus);
    });
    o->Add(m_setting, 0, wxEXPAND | wxTOP, gap);
    m_setting_caption    = add_text(opened, wxEmptyString, Label::Body_13, SECONDARY, true, true);
    m_texts.back().lines = 2;
    o->Add(m_setting_caption, 0, wxEXPAND | wxTOP, FromDIP(6));
    // The settings at the bottom of the card, level with the buttons of the other cards.
    o->AddStretchSpacer(1);
    m_undo = new Option(
        opened, _L("Undo the compensations"), [this]() { show_setting(Scene::Compensation); },
        [this]() {
            show_setting(Scene::Compensation);
            request_meshes();
        });
    m_undo->set_tooltip(_L("The slicer grew or shrank the outlines (X-Y contour and hole compensations) and the first layer "
                           "(elephant foot compensation). Undone, the parts get their sizes back."));
    o->Add(m_undo, 0, wxEXPAND | wxTOP, FromDIP(4));
    m_smooth = new Option(
        opened, _L("Smooth between the layers"), [this]() { show_setting(Scene::Smooth); },
        [this]() {
            show_setting(Scene::Smooth);
            request_meshes();
        });
    m_smooth->set_tooltip(_L("Slopes and curves go from the middle of a layer to the middle of the next one, instead of keeping "
                             "the steps of the layers. Slopes flatter than 20° keep their steps."));
    o->Add(m_smooth, 0, wxEXPAND | wxTOP, FromDIP(2));
    opened->SetSizer(o);
}

void GCodeToMeshPanel::build_transform_card()
{
    const int gap = FromDIP(8);

    // Before: what the layers become.
    wxWindow *arrive = m_rows[ROW_GCODE].arrive[2];
    auto     *a      = new wxBoxSizer(wxVERTICAL);
    a->Add(make_title(arrive, 3, _L("Transform")), 0, wxEXPAND);
    m_mesh = new Canvas(arrive, wxSize(-1, m_well_height),
                        [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) { paint_scene(gc, pal, area, Scene::Mesh); });
    a->Add(m_mesh, 0, wxEXPAND | wxTOP, gap);
    a->Add(add_text(arrive, _L("The layers become a faithful mesh, placed in the Prepare tab: to slice again or to export as STL."), Label::Body_13,
                    SECONDARY, true, true),
           0, wxEXPAND | wxTOP, gap);
    arrive->SetSizer(a);

    // Then: the result, or what the button under the mouse does; the buttons.
    wxWindow *opened = m_rows[ROW_GCODE].opened[2];
    auto     *o      = new wxBoxSizer(wxVERTICAL);
    o->Add(make_title(opened, 3, _L("Transform")), 0, wxEXPAND);
    m_result_canvas = new Canvas(opened, wxSize(-1, m_well_height), [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) {
        paint_scene(gc, pal, area, m_result);
    });
    o->Add(m_result_canvas, 0, wxEXPAND | wxTOP, gap);
    turnable(m_result_canvas, [this]() { return m_result == Scene::Result && !m_meshes.empty(); });
    m_result_caption     = add_text(opened, wxEmptyString, Label::Body_13, SECONDARY, true, true);
    m_texts.back().lines = 2;
    o->Add(m_result_caption, 0, wxEXPAND | wxTOP, FromDIP(6));
    // The size of the mesh.
    auto *size_row = new wxBoxSizer(wxHORIZONTAL);
    size_row->Add(add_text(opened, _L("Size"), Label::Body_13, SECONDARY, true, false), 0, wxRIGHT, FromDIP(10));
    m_size = add_text(opened, wxEmptyString, Label::Body_13, PLAIN, true, false, wxST_ELLIPSIZE_END);
    size_row->Add(m_size, 1);
    o->Add(size_row, 0, wxEXPAND | wxTOP, FromDIP(6));
    o->AddStretchSpacer(1);
    m_transform_button = make_button(opened, _L("Transform into mesh"), true);
    m_transform_button->SetToolTip(_L("Add the objects printed by the G-code to the Prepare tab, at their places on the current plate."));
    m_transform_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { transform(); });
    o->Add(m_transform_button, 0, wxEXPAND | wxTOP, gap);
    m_view_button = make_button(opened, _L("View the G-code"), false);
    m_view_button->SetToolTip(_L("Show the G-code in the Preview, alone: the current project is closed."));
    m_view_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { view(); });
    o->Add(m_view_button, 0, wxEXPAND | wxTOP, FromDIP(6));
    m_convert_button = make_button(opened, _L("Convert into STEP..."), false);
    m_convert_button->SetToolTip(_L("Make the mesh a STEP solid, in this tab."));
    m_convert_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { convert_gcode(); });
    o->Add(m_convert_button, 0, wxEXPAND | wxTOP, FromDIP(6));
    // The button under the mouse shows what it does.
    const std::pair<::Button *, Scene> hovers[] = { { m_transform_button, Scene::Transform }, { m_view_button, Scene::View }, { m_convert_button, Scene::Mesh } };
    for (const auto &hover : hovers) {
        const Scene scene = hover.second;
        hover.first->Bind(wxEVT_ENTER_WINDOW, [this, scene](wxMouseEvent &event) {
            show_result(scene);
            event.Skip();
        });
        hover.first->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &event) {
            show_result(Scene::Result);
            event.Skip();
        });
    }
    opened->SetSizer(o);
}

void GCodeToMeshPanel::build_mesh_arrive_cards()
{
    const int gap = FromDIP(8);

    // 1: where to drop the mesh, the button, the other way in.
    wxWindow *drop = m_rows[ROW_STEP].arrive[0];
    auto     *o    = new wxBoxSizer(wxVERTICAL);
    o->Add(make_title(drop, 1, _L("Open a mesh")), 0, wxEXPAND);
    m_mesh_drop = new Canvas(drop, wxSize(-1, m_well_height), [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) {
        paint_scene(gc, pal, area, Scene::DropMesh);
    });
    o->Add(m_mesh_drop, 0, wxEXPAND | wxTOP, gap);
    o->Add(add_text(drop, _L("Drop an STL, OBJ or 3MF file here"), Label::Body_13, PLAIN, true, false), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, gap);
    m_open_mesh_button = make_button(drop, _L("Open a mesh..."), true);
    m_open_mesh_button->SetToolTip(_L("Open an STL, OBJ or 3MF file to make it a STEP solid. It can also be dropped on this tab."));
    m_open_mesh_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_mesh(); });
    o->Add(m_open_mesh_button, 0, wxEXPAND | wxTOP, gap);
    o->Add(add_text(drop, _L("Or, in the Prepare tab: right click on an object, Export as STEP."), Label::Body_12, SECONDARY, true, true,
                    wxALIGN_CENTRE_HORIZONTAL),
           0, wxEXPAND | wxTOP, FromDIP(6));
    drop->SetSizer(o);

    // 2: what Orio finds in the mesh.
    wxWindow *found = m_rows[ROW_STEP].arrive[1];
    auto     *f    = new wxBoxSizer(wxVERTICAL);
    f->Add(make_title(found, 2, _L("Surfaces")), 0, wxEXPAND);
    m_surfaces = new Canvas(found, wxSize(-1, m_well_height), [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) {
        paint_scene(gc, pal, area, Scene::Surfaces);
    });
    f->Add(m_surfaces, 0, wxEXPAND | wxTOP, gap);
    f->Add(add_text(found, _L("Orio finds the planes, cylinders, cones, spheres and tori of the mesh, and makes each of them a true surface."),
                    Label::Body_13, SECONDARY, true, true),
           0, wxEXPAND | wxTOP, gap);
    found->SetSizer(f);

    // 3: the STEP file.
    wxWindow *file = m_rows[ROW_STEP].arrive[2];
    auto     *v    = new wxBoxSizer(wxVERTICAL);
    v->Add(make_title(file, 3, _L("STEP")), 0, wxEXPAND);
    m_step_file = new Canvas(file, wxSize(-1, m_well_height), [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) {
        paint_scene(gc, pal, area, Scene::StepFile);
    });
    v->Add(m_step_file, 0, wxEXPAND | wxTOP, gap);
    v->Add(add_text(file, _L("The solid is saved as a STEP file, for CAD software: FreeCAD, Fusion 360, SolidWorks, Onshape..."), Label::Body_13,
                    SECONDARY, true, true),
           0, wxEXPAND | wxTOP, gap);
    file->SetSizer(v);
}

void GCodeToMeshPanel::build_convert_cards()
{
    const int gap = FromDIP(8);

    // 1: the mesh, turned with the mouse, what it measures; repaired, another one.
    wxWindow *part = m_rows[ROW_STEP].opened[0];
    auto     *p    = new wxBoxSizer(wxVERTICAL);
    p->Add(make_title(part, 1, _L("The mesh")), 0, wxEXPAND);
    m_part = new Canvas(part, wxSize(-1, m_well_height),
                        [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) { paint_part(gc, pal, area); });
    turnable(m_part, [this]() { return m_mesh_its != nullptr; });
    p->Add(m_part, 0, wxEXPAND | wxTOP, gap);
    auto *grid = new wxFlexGridSizer(2, FromDIP(4), FromDIP(10));
    grid->AddGrowableCol(1, 1);
    const wxString labels[] = { _L("Triangles"), _L("Size"), _L("Volume"), _L("Closed") };
    for (int i = 0; i < 4; ++i) {
        grid->Add(add_text(part, labels[i], Label::Body_13, SECONDARY, true, false), 0);
        m_part_info[i] = add_text(part, wxEmptyString, Label::Body_13, PLAIN, true, false, wxST_ELLIPSIZE_END);
        grid->Add(m_part_info[i], 1, wxEXPAND);
    }
    p->Add(grid, 0, wxEXPAND | wxTOP, gap);
    p->AddStretchSpacer(1);
    m_repair_button = make_button(part, _L("Repair the mesh"), false);
    m_repair_button->SetToolTip(_L("Close the holes of the mesh and fix its faces, with the repair of Windows."));
    m_repair_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { repair_mesh(); });
    p->Add(m_repair_button, 0, wxEXPAND | wxTOP, gap);
    m_other_mesh_button = make_button(part, _L("Open another mesh..."), false);
    m_other_mesh_button->SetToolTip(_L("Open an STL, OBJ or 3MF file to make it a STEP solid. It can also be dropped on this tab."));
    m_other_mesh_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_mesh(); });
    p->Add(m_other_mesh_button, 0, wxEXPAND | wxTOP, FromDIP(6));
    part->SetSizer(p);

    // 2: the deviation allowed between the mesh and the surfaces found, illustrated as it is chosen.
    wxWindow *surfaces = m_rows[ROW_STEP].opened[1];
    auto     *s        = new wxBoxSizer(wxVERTICAL);
    s->Add(make_title(surfaces, 2, _L("Surfaces")), 0, wxEXPAND);
    m_deviation = new Canvas(surfaces, wxSize(-1, m_well_height),
                             [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) { paint_deviation(gc, pal, area); });
    s->Add(m_deviation, 0, wxEXPAND | wxTOP, gap);
    s->Add(add_text(surfaces,
                    _L("Each point of the mesh must lie within this distance of the surface found (shaded band). 0.01 mm gives "
                       "the truest surfaces; larger, a surface may stand for another (a nearly flat area for a large sphere)."),
                    Label::Body_13, SECONDARY, true, true),
           0, wxEXPAND | wxTOP, FromDIP(6));
    s->AddStretchSpacer(1);
    s->Add(add_text(surfaces, _L("Deviation allowed"), Label::Body_13, SECONDARY, true, false), 0, wxTOP, gap);
    std::vector<wxString> tolerances;
    for (double tolerance : TOLERANCES)
        tolerances.emplace_back(number(tolerance, 3) + " mm");
    auto *chips = new Chips(surfaces, tolerances, [this](int index) {
        m_tolerance = index;
        m_deviation->Refresh();
        recognize();
    });
    chips->set_selected(m_tolerance);
    s->Add(chips, 0, wxEXPAND | wxTOP, FromDIP(2));
    surfaces->SetSizer(s);

    // 3: the surfaces found, a color per kind, turned with the mouse; the save, its progress and its report.
    wxWindow *step = m_rows[ROW_STEP].opened[2];
    auto     *t    = new wxBoxSizer(wxVERTICAL);
    t->Add(make_title(step, 3, _L("STEP")), 0, wxEXPAND);
    m_regions = new Canvas(step, wxSize(-1, m_well_height),
                           [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) { paint_regions(gc, pal, area); });
    turnable(m_regions, [this]() { return m_mesh_its != nullptr; });
    t->Add(m_regions, 0, wxEXPAND | wxTOP, gap);
    m_legend = new Canvas(
        step, wxSize(-1, FromDIP(58)), [this](wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) { paint_legend(gc, pal, area); },
        false);
    t->Add(m_legend, 0, wxEXPAND | wxTOP, FromDIP(4));
    m_step_caption       = add_text(step, wxEmptyString, Label::Body_13, SECONDARY, true, true);
    m_texts.back().lines = 3;
    t->Add(m_step_caption, 0, wxEXPAND | wxTOP, FromDIP(2));
    t->AddStretchSpacer(1);
    m_save_button = make_button(step, _L("Save as STEP..."), true);
    m_save_button->SetToolTip(_L("Write the solid into a STEP file, in millimeters, for CAD software."));
    m_save_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { save_step(); });
    t->Add(m_save_button, 0, wxEXPAND | wxTOP, gap);
    step->SetSizer(t);
}

void GCodeToMeshPanel::turnable(Canvas *canvas, std::function<bool()> ready)
{
    canvas->SetCursor(wxCursor(wxCURSOR_HAND));
    canvas->SetToolTip(_L("Drag to turn the part"));
    const auto grab = [this, canvas, ready](wxMouseEvent &event) {
        if (!ready())
            return;
        m_dragging   = true;
        m_drag_start = event.GetPosition();
        canvas->CaptureMouse();
    };
    canvas->Bind(wxEVT_LEFT_DOWN, grab);
    canvas->Bind(wxEVT_LEFT_DCLICK, grab);
    canvas->Bind(wxEVT_MOTION, [this, canvas](wxMouseEvent &event) {
        if (!m_dragging)
            return;
        const wxPoint move = event.GetPosition() - m_drag_start;
        m_drag_start       = event.GetPosition();
        // Radians per pixel, whatever the scale of the screen.
        const double turn = 1. / FromDIP(100);
        m_yaw += turn * move.x;
        m_pitch = std::clamp(m_pitch + turn * move.y, -1.5, 1.5);
        forget_renders();
        canvas->Refresh(false);
    });
    // Let go: drawn again, finer, in every well.
    const auto release = [this]() {
        if (!m_dragging)
            return;
        m_dragging = false;
        forget_renders();
        for (Canvas *well : { m_result_canvas, m_part, m_regions })
            if (well != nullptr)
                well->Refresh(false);
    };
    canvas->Bind(wxEVT_LEFT_UP, [canvas, release](wxMouseEvent &) {
        if (canvas->HasCapture())
            canvas->ReleaseMouse();
        release();
    });
    canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [release](wxMouseCaptureLostEvent &) { release(); });
}

void GCodeToMeshPanel::forget_renders()
{
    m_render         = wxBitmap();
    m_part_render    = wxBitmap();
    m_regions_render = wxBitmap();
}

wxSizer *GCodeToMeshPanel::make_title(wxWindow *parent, int step, const wxString &title)
{
    auto *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(add_text(parent, wxString::Format("%d", step), Label::Head_14, ACCENT, true, false), 0);
    sizer->Add(add_text(parent, separator() + title, Label::Head_14, PLAIN, true, false), 0);
    return sizer;
}

wxStaticText *GCodeToMeshPanel::add_text(wxWindow *parent, const wxString &label, const wxFont &font, int role, bool on_card, bool wrapped, long style)
{
    auto *text = new wxStaticText(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, style);
    text->SetLabelText(label);
    text->SetFont(font);
    // An ellipsized or wrapped text takes the width it is given, not the width of its label.
    if (wrapped || (style & (wxST_ELLIPSIZE_START | wxST_ELLIPSIZE_MIDDLE | wxST_ELLIPSIZE_END)) != 0)
        text->SetMinSize(wxSize(FromDIP(20), -1));
    m_texts.push_back({ text, role, on_card, wrapped, label });
    return text;
}

void GCodeToMeshPanel::set_text(wxStaticText *text, const wxString &label)
{
    text->SetLabelText(label);
    for (Text &entry : m_texts)
        if (entry.text == text) {
            entry.label = label;
            if (entry.wrapped && m_wrap_width > 0 && wrap(entry))
                relayout();
            break;
        }
}

void GCodeToMeshPanel::rewrap(bool force)
{
    const int    em     = wxGetApp().em_unit();
    const wxSize client = m_page->GetClientSize();
    const int    width  = std::max(70 * em, std::min(140 * em, client.x - 4 * em));
    if (width == m_wrap_width && client.y == m_page_height && !force)
        return;
    m_wrap_width  = width;
    m_page_height = client.y;
    // Inside a card: a third of the column less the gaps between the cards, less the margins of the card; those of the
    // frame of the row in beta too, its cards are the narrowest.
    m_card_width = (width - 2 * FromDIP(BetaFrame::MARGIN) - 2 * FromDIP(12)) / 3 - 2 * FromDIP(14);
    m_column->SetMinSize(wxSize(width, -1));
    for (const Text &entry : m_texts)
        if (entry.wrapped) {
            if (entry.lines > 0)
                entry.text->SetMinSize(wxSize(FromDIP(20), entry.lines * entry.text->GetCharHeight()));
            wrap(entry);
        }
    // The illustrations (one per card, two rows) take the height left in the page (the margins and one more line kept
    // free, so that no scroll bar comes and goes), from half to three quarters of their width: a low page scrolls, what
    // is left of a high one centers the column.
    const int rest = m_column->GetMinSize().y - 2 * m_well_height;
    const int low  = std::max(FromDIP(150), m_card_width / 2);
    const int high = std::max(low, m_card_width * 3 / 4);
    m_well_height  = std::clamp((client.y - 5 * em - rest) / 2, low, high);
    for (Canvas *well : { m_drop, m_scan, m_mesh, m_picture, m_setting, m_result_canvas, m_mesh_drop, m_surfaces, m_step_file, m_part, m_deviation, m_regions })
        well->SetMinSize(wxSize(-1, m_well_height));
    relayout();
}

bool GCodeToMeshPanel::wrap(const Text &entry)
{
    entry.text->SetLabelText(entry.label);
    entry.text->Wrap(entry.on_card ? m_card_width : m_wrap_width);
    // A caption only grows, up to its longest text at this width: what follows it does not move under the mouse.
    const int height = entry.text->GetBestSize().y;
    if (entry.lines == 0 || height <= entry.text->GetMinSize().y)
        return false;
    entry.text->SetMinSize(wxSize(FromDIP(20), height));
    return true;
}

void GCodeToMeshPanel::relayout()
{
    m_page->Layout();
    // A card keeping its size does not lay out again what changed in it.
    for (const Row &row : m_rows)
        for (int i = 0; i < 3; ++i) {
            row.cards[i]->Layout();
            row.arrive[i]->Layout();
            row.opened[i]->Layout();
        }
    m_page->FitInside();
}

void GCodeToMeshPanel::scroll_to(int row)
{
    // Once the page is laid out with what was just opened.
    CallAfter([this, row]() {
        int unit_x = 1, unit_y = 1;
        m_page->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const wxPoint top = m_page->CalcUnscrolledPosition(m_rows[row].label->GetPosition());
        m_page->Scroll(-1, std::max(0, top.y - FromDIP(10)) / std::max(unit_y, 1));
    });
}

void GCodeToMeshPanel::choose_file()
{
    wxString path;
    wxGetApp().load_gcode(this, path);
    if (!path.IsEmpty())
        open(path);
}

bool GCodeToMeshPanel::open(const wxString &path)
{
    auto gcode = std::make_shared<GCodeToMesh>();
    try {
        wxBusyCursor wait;
        gcode->load(into_u8(path));
    } catch (const std::exception &ex) {
        show_error(this, _L("The G-code cannot be read.") + "\n" + from_u8(ex.what()));
        return false;
    }
    m_gcode = std::move(gcode);
    m_path  = path;
    m_view  = Scene::Thumbnail;
    // The meshes of the previous G-code dropped, the one being built stopped.
    m_gcode_worker.stop();
    m_meshes.clear();
    m_building     = false;
    m_build_failed = false;
    m_render       = wxBitmap();
    remember_gcode(path);
    read_pictures();
    update();
    request_meshes();
    scroll_to(ROW_GCODE);
    return true;
}

bool GCodeToMeshPanel::open_mesh(const wxString &path)
{
    TriangleMesh mesh;
    try {
        wxBusyCursor wait;
        const Model  model = Model::read_from_file(into_u8(path), nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel);
        for (const ModelObject *object : model.objects)
            mesh.merge(object_mesh(*object, -1));
    } catch (const std::exception &ex) {
        show_error(this, _L("The mesh cannot be read.") + "\n" + from_u8(ex.what()));
        return false;
    }
    if (mesh.empty()) {
        show_error(this, _L("This file has no triangle."));
        return false;
    }
    const wxFileName name(path);
    show_mesh(std::move(mesh.its), name.GetName(), name.GetPath());
    return true;
}

void GCodeToMeshPanel::open_object(const ModelObject &object, int instance)
{
    TriangleMesh mesh = object_mesh(object, instance);
    if (mesh.empty()) {
        show_error(this, _L("This object has no part to convert."));
        return;
    }
    show_mesh(std::move(mesh.its), from_u8(object.name), wxEmptyString);
}

void GCodeToMeshPanel::choose_mesh()
{
    wxFileDialog dialog(this, _L("Convert a mesh into STEP"), from_u8(wxGetApp().app_config->get_last_dir()), wxEmptyString,
                        _L("Meshes") + " (*.stl;*.obj;*.3mf)|*.stl;*.obj;*.3mf", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK)
        open_mesh(dialog.GetPath());
}

void GCodeToMeshPanel::convert_gcode()
{
    // No object, or an error already shown.
    const std::vector<indexed_triangle_set> meshes = current_meshes();
    if (meshes.empty())
        return;
    indexed_triangle_set all;
    for (const indexed_triangle_set &its : meshes)
        its_merge(all, its);
    if (all.indices.empty()) {
        show_error(this, _L("No object could be rebuilt from this G-code."));
        return;
    }
    show_mesh(std::move(all), wxFileName(m_path).GetName(), wxFileName(m_path).GetPath());
}

void GCodeToMeshPanel::show_mesh(indexed_triangle_set its, const wxString &name, const wxString &folder)
{
    BoundingBoxf3 box;
    for (const stl_vertex &v : its.vertices)
        box.merge(v.cast<double>());
    m_mesh_size   = box.defined ? Vec3d(box.size()) : Vec3d(Vec3d::Zero());
    m_mesh_volume = its_volume(its);
    m_mesh_its    = std::make_shared<indexed_triangle_set>(std::move(its));
    m_mesh_name   = name.IsEmpty() ? _L("Mesh") : name;
    m_mesh_folder = folder;
    // Not known until the surfaces are recognized.
    m_step.reset();
    m_face_kinds.clear();
    m_closed = -1;
    forget_renders();
    recognize();
    update();
    scroll_to(ROW_STEP);
}

void GCodeToMeshPanel::repair_mesh()
{
    if (!m_mesh_its)
        return;
    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(TriangleMesh(*m_mesh_its), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    ProgressDialog progress(_L("Repairing model object"), "", 100, wxGetApp().mainframe, wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_CAN_ABORT, true);
    std::string    failure;
    // Canceled: the mesh is left as it was.
    if (!fix_model_by_win10_sdk_gui(*object, -1, progress, _L("Repairing model object") + "\n", failure))
        return;
    if (!failure.empty()) {
        show_error(this, _L("The mesh could not be repaired.") + "\n" + from_u8(failure));
        return;
    }
    TriangleMesh repaired = object_mesh(*object, 0);
    show_mesh(std::move(repaired.its), m_mesh_name, m_mesh_folder);
}

void GCodeToMeshPanel::sys_color_changed()
{
    m_undo->rescale();
    m_smooth->rescale();
    forget_renders();
    apply_colors();
}

void GCodeToMeshPanel::read_pictures()
{
    // The thumbnail written by the slicer.
    m_thumbnail = wxBitmap();
    if (!m_gcode->thumbnail_base64.empty()) {
        const wxMemoryBuffer data = wxBase64Decode(m_gcode->thumbnail_base64.c_str(), m_gcode->thumbnail_base64.size(), wxBase64DecodeMode_SkipWS);
        if (data.GetDataLen() > 0) {
            // A broken picture is not shown, without an error message.
            wxLogNull           silent;
            wxMemoryInputStream stream(data.GetData(), data.GetDataLen());
            wxImage             image;
            if (image.LoadFile(stream, m_gcode->thumbnail_format == "jpg" ? wxBITMAP_TYPE_JPEG : wxBITMAP_TYPE_PNG) && image.IsOk())
                m_thumbnail = wxBitmap(image);
        }
    }
    // The outer walls of some layers seen from above (about 40 layers per object), the width of each layer seen from the side.
    m_top.clear();
    m_profile.clear();
    m_top_min = Vec2d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    m_top_max = -m_top_min;
    for (const GCodeToMesh::Object &object : m_gcode->objects) {
        const size_t count = object.layers.size();
        const size_t step  = std::max<size_t>(1, count / 40);
        for (size_t i = 0; i < count; ++i) {
            const GCodeToMesh::Layer &layer = object.layers[i];
            double                    x0    = std::numeric_limits<double>::max();
            double                    x1    = -x0;
            auto                      add   = [&](const Point &p) {
                const Vec2d q = unscaled(p);
                x0            = std::min(x0, q.x());
                x1            = std::max(x1, q.x());
                m_top_min     = m_top_min.cwiseMin(q);
                m_top_max     = m_top_max.cwiseMax(q);
                return q;
            };
            for (const Polygon &loop : layer.loops) {
                std::vector<Vec2d> points;
                points.reserve(loop.points.size());
                for (const Point &p : loop.points)
                    points.emplace_back(add(p));
                if ((i % step == 0 || i + 1 == count) && points.size() >= 3)
                    m_top.emplace_back(std::move(points));
            }
            for (const Polyline &path : layer.open_paths)
                for (const Point &p : path.points)
                    add(p);
            if (x0 <= x1)
                m_profile.push_back({ i > 0 ? double(object.layers[i - 1].z) : std::max(0., double(layer.z) - layer.height), double(layer.z), x0, x1 });
        }
    }
}

wxString GCodeToMeshPanel::header_details() const
{
    wxString out;
    auto     add = [&out](const wxString &part) {
        if (!part.IsEmpty())
            out += (out.IsEmpty() ? wxString() : separator()) + part;
    };
    add(from_u8(m_gcode->producer));
    add(print_time(m_gcode->print_time));
    const size_t count = m_gcode->objects.size();
    add(count == 1 ? _L("1 object") : wxString::Format(_L("%d objects"), int(count)));
    return out;
}

void GCodeToMeshPanel::update()
{
    // Row 1: the G-code open, or what the row is for.
    Row       &g      = m_rows[ROW_GCODE];
    const bool loaded = !m_path.IsEmpty();
    g.icon->Show(loaded);
    set_text(g.title, loaded ? wxFileName(m_path).GetFullName() : _L("No G-code open"));
    set_text(g.subtitle, loaded ? header_details() : _L("Find the mesh of a part from its G-code"));
    for (int i = 0; i < 3; ++i) {
        g.arrive[i]->Show(!loaded);
        g.opened[i]->Show(loaded);
    }
    wxString notes;
    if (loaded) {
        // What the G-code says.
        const GCodeToMesh::Stats stats   = m_gcode->stats();
        const wxString           unknown = _L("Unknown");
        std::string              printer = m_gcode->setting("printer_settings_id");
        boost::algorithm::trim_if(printer, boost::algorithm::is_any_of("\" "));
        if (printer.empty())
            printer = m_gcode->setting("printer_model");
        const double   nozzle       = m_gcode->number("nozzle_diameter");
        const wxString layer_height = stats.layer_height_max > 0.f ? mm_range(stats.layer_height_min, stats.layer_height_max) : mm(stats.first_layer_height);
        const wxString values[]     = { m_gcode->producer.empty() ? unknown : from_u8(m_gcode->producer),
                                        printer.empty() ? unknown : from_u8(printer),
                                        nozzle > 0. ? mm(nozzle) : unknown,
                                        stats.width_max > 0.f ? mm_range(stats.width_min, stats.width_max) : unknown,
                                        stats.layers > 0 ? wxString::Format("%d %s %s", int(stats.layers), wxString::FromUTF8("\xC3\x97"), layer_height) : unknown,
                                        stats.layers > 0 ? mm(stats.height) : unknown,
                                        wxString::Format("%d", int(m_gcode->objects.size())) };
        for (int i = 0; i < 7; ++i) {
            set_text(m_info[i], values[i]);
            m_info[i]->SetToolTip(values[i]);
        }
        // The size of the mesh: the outer walls seen from above widened by their line width, and their height.
        wxString size = unknown;
        if (m_top_min.x() <= m_top_max.x()) {
            const Vec2d    extent = m_top_max - m_top_min + Vec2d::Constant(stats.width_max);
            const wxString times  = wxString::FromUTF8(" \xC3\x97 ");
            size = number(extent.x(), 1) + times + number(extent.y(), 1) + times + number(stats.height, 1) + " mm";
        }
        set_text(m_size, size);

        // The compensations written in the G-code, the smoothing.
        const GCodeToMesh::Compensation compensation = m_gcode->compensation();
        wxString                        undone;
        auto                            add = [&undone](const wxString &part) { undone += (undone.IsEmpty() ? wxString() : separator()) + part; };
        if (compensation.contour != 0.)
            add(wxString::Format(_L("contours %s"), mm(compensation.contour)));
        if (compensation.hole != 0.)
            add(wxString::Format(_L("holes %s"), mm(compensation.hole)));
        if (compensation.elephant_foot != 0.)
            add(wxString::Format(_L("elephant foot %s"), mm(compensation.elephant_foot)));
        m_undo_available = !undone.IsEmpty();
        m_undo->enable(m_undo_available);
        m_undo->set_subtitle(m_undo_available ? undone : _L("none in this G-code"));
        m_smooth->set_subtitle(_L("slopes steeper than 20°"));

        // The picture of the print: the thumbnail of the slicer, otherwise the walls seen from above.
        m_views->set_enabled(0, m_thumbnail.IsOk());
        if (!m_thumbnail.IsOk() && m_view == Scene::Thumbnail)
            m_view = Scene::Top;
        m_views->set_selected(m_view == Scene::Thumbnail ? 0 : m_view == Scene::Top ? 1 : 2);
        m_picture->Refresh();

        std::vector<wxString> lines;
        if (m_gcode->objects.empty())
            lines.emplace_back(_L("No outer wall was found in this G-code. The line types are read in the comments written by Bambu "
                                  "Studio, Orio, Orca Slicer and PrusaSlicer."));
        else {
            if (stats.open_paths > 0)
                lines.emplace_back(wxString::Format(_L("%d outer wall paths do not close (walls of a single line): they keep their line width."),
                                                    int(stats.open_paths)));
            if (m_gcode->overhangs_left_out > 0)
                lines.emplace_back(wxString::Format(_L("%d overhang wall paths lying on inner walls were left out."), int(m_gcode->overhangs_left_out)));
            if (m_gcode->number("spiral_mode") > 0. || m_gcode->number("spiral_vase") > 0.)
                lines.emplace_back(_L("Vase mode: each turn of the spiral becomes a layer."));
        }
        for (const wxString &line : lines)
            notes += (notes.IsEmpty() ? wxString() : wxString("\n")) + line;
        m_transform_button->Enable(!m_gcode->objects.empty());
        m_convert_button->Enable(!m_gcode->objects.empty());
        show_setting(m_focus);
        show_result(Scene::Result);
    } else
        update_recent();
    set_text(g.notes, notes);
    g.notes->Show(!notes.IsEmpty());
    g.rail->set(loaded, g.hovered);

    // Row 2: the mesh open, or what the row is for.
    Row       &m    = m_rows[ROW_STEP];
    const bool mesh = m_mesh_its != nullptr;
    m.icon->Show(mesh);
    set_text(m.title, mesh ? m_mesh_name : _L("No mesh open"));
    set_text(m.subtitle, mesh ? wxString::Format(_L("%s triangles"), wxNumberFormatter::ToString(long(m_mesh_its->indices.size()))) :
                                _L("Make a mesh a STEP solid, for CAD software"));
    for (int i = 0; i < 3; ++i) {
        m.arrive[i]->Show(!mesh);
        m.opened[i]->Show(mesh);
    }
    const wxString mesh_notes = mesh ? _L("Beta: the faces of the STEP solid lie on the recognized surfaces, but their edges still follow "
                                          "the triangles of the mesh, within the deviation allowed.") :
                                       wxString();
    set_text(m.notes, mesh_notes);
    m.notes->Show(!mesh_notes.IsEmpty());
    m.rail->set(mesh, m.hovered);
    update_mesh();

    // The animations of the empty cards, while there are.
    if (loaded && mesh)
        m_timer.Stop();
    else if (!m_timer.IsRunning())
        m_timer.Start(40);
    apply_colors();
    rewrap(true);
}

void GCodeToMeshPanel::update_recent()
{
    wxSizer *sizer = m_recent->GetSizer();
    sizer->Clear(true);
    int shown = 0;
    for (const wxString &path : recent_gcodes()) {
        if (shown == 2)
            break;
        if (!wxFileName::FileExists(path))
            continue;
        // Colored by apply_colors().
        auto *row = new wxStaticText(m_recent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
        row->SetLabelText(wxFileName(path).GetFullName());
        row->SetFont(Label::Body_13);
        row->SetMinSize(wxSize(FromDIP(20), -1));
        row->SetToolTip(path);
        row->SetCursor(wxCursor(wxCURSOR_HAND));
        // Opened after the click: opening rebuilds this list.
        row->Bind(wxEVT_LEFT_UP, [this, path](wxMouseEvent &) { CallAfter([this, path]() { open(path); }); });
        sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(3));
        ++shown;
    }
    m_recent_title->Show(shown > 0);
    m_recent->Show(shown > 0);
    m_recent->Layout();
}

void GCodeToMeshPanel::apply_colors()
{
    const Palette pal = palette();
    SetBackgroundColour(pal.page);
    m_page->SetBackgroundColour(pal.page);
    for (const Row &row : m_rows)
        for (int i = 0; i < 3; ++i) {
            row.arrive[i]->SetBackgroundColour(pal.card);
            row.opened[i]->SetBackgroundColour(pal.card);
        }
    m_recent->SetBackgroundColour(pal.card);
    for (wxWindow *row : m_recent->GetChildren()) {
        row->SetBackgroundColour(pal.card);
        row->SetForegroundColour(pal.accent);
    }
    for (const Text &entry : m_texts) {
        entry.text->SetBackgroundColour(entry.on_card ? pal.card : pal.page);
        entry.text->SetForegroundColour(entry.role == PLAIN ? pal.text : entry.role == SECONDARY ? pal.text2 : pal.accent);
    }
    m_undo->apply();
    m_smooth->apply();
    // The corners of the buttons take the color of their card.
    for (::Button *button : { m_open_button, m_open_mesh_button, m_change_button, m_transform_button, m_view_button, m_convert_button, m_repair_button,
                              m_other_mesh_button, m_save_button }) {
        button->SetBackgroundColour(pal.card);
        button->Refresh();
    }
    m_page->Refresh();
}

void GCodeToMeshPanel::hover_card(int row, int index, bool hovered)
{
    Row &r = m_rows[row];
    if (hovered)
        r.hovered = index + 1;
    else if (r.hovered == index + 1)
        r.hovered = 0;
    for (int i = 0; i < 3; ++i)
        r.cards[i]->set_hovered(r.hovered == i + 1);
    r.rail->set(row == ROW_GCODE ? !m_path.IsEmpty() : m_mesh_its != nullptr, r.hovered);
}

void GCodeToMeshPanel::show_setting(Scene scene)
{
    m_focus = scene;
    m_undo->set_selected(scene == Scene::Compensation);
    m_smooth->set_selected(scene == Scene::Smooth);
    wxString caption;
    if (scene == Scene::Smooth)
        caption = m_smooth->value() ? _L("Checked: the slope joins the middle of each layer (blue) instead of the steps (dotted).") :
                                      _L("Unchecked: the steps of the layers are kept as they are.");
    else if (!m_undo_available)
        caption = _L("This G-code has no size compensation to undo.");
    else
        caption = m_undo->value() ? _L("Checked: the first layer, the contours and the holes get their sizes back (blue dotted lines).") :
                                    _L("Unchecked: the part keeps the sizes reduced by the slicer.");
    set_text(m_setting_caption, caption);
    m_setting->Refresh();
}

void GCodeToMeshPanel::show_result(Scene scene)
{
    m_result = scene;
    wxString     caption;
    const size_t count = m_gcode->objects.size();
    if (scene == Scene::View)
        caption = _L("The G-code alone in the Preview, layer by layer, in the usual colors.");
    else if (scene == Scene::Transform)
        caption = _L("The mesh goes to the Prepare tab: to slice again or to export as STL.");
    else if (scene == Scene::Mesh)
        caption = _L("The mesh becomes a STEP solid for CAD software: its planes and its curved surfaces are recognized.");
    else if (count == 0)
        caption = _L("No object to rebuild.");
    else if (count == 1)
        caption = _L("1 object, placed at its place on the plate of the Prepare tab.");
    else
        caption = wxString::Format(_L("%d objects, placed at their places on the plate of the Prepare tab."), int(count));
    set_text(m_result_caption, caption);
    m_result_canvas->Refresh();
}

void GCodeToMeshPanel::paint_top(wxGraphicsContext &gc, const wxRect2DDouble &area) const
{
    const Vec2d size = m_top_max - m_top_min;
    if (m_top.empty() || size.x() <= 0. || size.y() <= 0.)
        return;
    const double scale = std::min(area.m_width / size.x(), area.m_height / size.y());
    const double x0    = area.m_x + (area.m_width - size.x() * scale) / 2.;
    const double y0    = area.m_y + (area.m_height - size.y() * scale) / 2.;
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(OUTER_WALL, 1.2 * m_page->FromDIP(100) / 100.)));
    for (const std::vector<Vec2d> &loop : m_top) {
        wxGraphicsPath path = gc.CreatePath();
        for (size_t i = 0; i < loop.size(); ++i) {
            // The Y of the bed goes up, the one of the screen down.
            const double x = x0 + (loop[i].x() - m_top_min.x()) * scale;
            const double y = y0 + (m_top_max.y() - loop[i].y()) * scale;
            if (i == 0)
                path.MoveToPoint(x, y);
            else
                path.AddLineToPoint(x, y);
        }
        path.CloseSubpath();
        gc.StrokePath(path);
    }
}

void GCodeToMeshPanel::paint_profile(wxGraphicsContext &gc, const wxRect2DDouble &area, const wxColour &colour) const
{
    if (m_profile.empty())
        return;
    double x0 = std::numeric_limits<double>::max(), x1 = -x0, z0 = x0, z1 = -x0;
    for (const Bar &bar : m_profile) {
        x0 = std::min(x0, bar.x0);
        x1 = std::max(x1, bar.x1);
        z0 = std::min(z0, bar.z0);
        z1 = std::max(z1, bar.z1);
    }
    if (x1 <= x0 || z1 <= z0)
        return;
    const double scale  = std::min(area.m_width / (x1 - x0), area.m_height / (z1 - z0));
    const double left   = area.m_x + (area.m_width - (x1 - x0) * scale) / 2.;
    const double bottom = area.m_y + (area.m_height + (z1 - z0) * scale) / 2.;
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(colour));
    for (const Bar &bar : m_profile) {
        // A gap between thick layers, as in a print.
        const double h = (bar.z1 - bar.z0) * scale;
        gc.DrawRectangle(left + (bar.x0 - x0) * scale, bottom - (bar.z1 - z0) * scale, (bar.x1 - bar.x0) * scale, std::max(h - (h > 3. ? 0.6 : 0.), 0.5));
    }
}

void GCodeToMeshPanel::paint_meshes(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const
{
    if (m_meshes.empty()) {
        // Not built yet, or not buildable: the layers seen from the side, faint, and why.
        paint_profile(gc, area, with_alpha(pal.text2, 60));
        if (m_building || m_build_failed)
            draw_text(gc, m_building ? _L("Building the mesh...") : _L("No object could be rebuilt from this G-code."), area.m_x + area.m_width / 2.,
                      area.m_y + area.m_height / 2., pal.text, Label::Body_13, 0);
        return;
    }
    // Finer when still.
    draw_cached(gc, area, m_render, !m_dragging, [this, &pal](int w, int h) {
        return render_meshes(m_meshes.data(), m_meshes.size(), w, h, m_yaw, m_pitch, wxGetApp().dark_mode(), pal.well);
    });
    // Meshes with new settings on the way.
    if (m_building)
        draw_text(gc, _L("Building the mesh..."), area.m_x + FromDIP(8), area.m_y + area.m_height - FromDIP(12), pal.text2, Label::Body_12, -1);
}

void GCodeToMeshPanel::paint_part(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const
{
    if (m_mesh_its)
        draw_cached(gc, area, m_part_render, !m_dragging, [this, &pal](int w, int h) {
            return render_meshes(m_mesh_its.get(), 1, w, h, m_yaw, m_pitch, wxGetApp().dark_mode(), pal.well);
        });
}

void GCodeToMeshPanel::paint_deviation(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const
{
    // A rounded edge seen from the side (200 x 120): the surface found, the band of the deviation allowed around it (the
    // wider, the larger the deviation chosen), the mesh inside it, its points on the surface.
    fit(gc, area, 200., 120.);
    const auto arc = [](int steps) {
        Points2 points{ { 20., 95. } };
        for (int i = 0; i <= steps; ++i) {
            const double a = PI / 2. * i / steps;
            points.emplace_back(80. + 45. * std::sin(a), 50. + 45. * std::cos(a));
        }
        points.emplace_back(125., 15.);
        return points;
    };
    const double  band[4] = { 5., 9., 15., 22. };
    const Points2 surface = arc(24);
    gc.SetBrush(*wxTRANSPARENT_BRUSH);
    gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(with_alpha(pal.accent, 60), band[std::clamp(m_tolerance, 0, 3)]).Join(wxJOIN_ROUND).Cap(wxCAP_ROUND)));
    gc.StrokePath(make_path(gc, surface, false));
    stroke_path(gc, surface, pal.accent, 2., false);
    const Points2 mesh = arc(4);
    stroke_path(gc, mesh, pal.text2, 1.2, false);
    for (const wxPoint2DDouble &point : mesh)
        fill_circle(gc, point.m_x, point.m_y, 2.5, pal.text);
}

void GCodeToMeshPanel::paint_regions(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const
{
    if (!m_mesh_its)
        return;
    // The surfaces found, a color per kind; the mesh until they are.
    draw_cached(gc, area, m_regions_render, !m_dragging, [this, &pal](int w, int h) {
        const bool dark = wxGetApp().dark_mode();
        return m_step ? render_meshes(&m_step->mesh(), 1, w, h, m_yaw, m_pitch, dark, pal.well, m_face_kinds) :
                        render_meshes(m_mesh_its.get(), 1, w, h, m_yaw, m_pitch, dark, pal.well);
    });
    const double dip  = FromDIP(100) / 100.;
    const double left = area.m_x + 8. * dip;
    if (m_saving) {
        // A bar at the bottom, filled as the file is written.
        const double width = area.m_width - 16. * dip;
        const double h     = 6. * dip;
        const double y     = area.m_y + area.m_height - h - 8. * dip;
        fill_round_rect(gc, left, y, width, h, h / 2., pal.border);
        fill_round_rect(gc, left, y, std::max(h, width * m_save_progress), h, h / 2., pal.accent);
        draw_text(gc, _L("Saving the STEP...") + wxString::Format(" %d %%", int(100. * m_save_progress)), left, y - 10. * dip, pal.text, Label::Body_12, -1);
    } else if (m_recognizing)
        draw_text(gc, _L("Recognizing the surfaces..."), left, area.m_y + area.m_height - 12. * dip, pal.text2, Label::Body_12, -1);
}

void GCodeToMeshPanel::paint_legend(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area) const
{
    if (!m_step)
        return;
    size_t counts[10] = {};
    for (const MeshToStep::Surface &surface : m_step->surfaces())
        ++counts[std::min<size_t>(size_t(surface.kind), 9)];
    const wxString names[10] = { _L("Planes"), _L("Cylinders"),  _L("Cones"),       _L("Spheres"), _L("Tori"),
                                 _L("Facets"), _L("Extrusions"), _L("Revolutions"), _L("Helices"), _L("Free surfaces") };
    // A dot and a count per kind found, on as many lines as needed.
    const double dip  = FromDIP(100) / 100.;
    const double r    = 4.5 * dip;
    const double line = 18. * dip;
    double       x    = area.m_x;
    double       y    = area.m_y + line / 2.;
    gc.SetFont(Label::Body_12, pal.text);
    for (int kind = 0; kind < 10; ++kind) {
        if (counts[kind] == 0)
            continue;
        const wxString label = names[kind] + " " + wxNumberFormatter::ToString(long(counts[kind]));
        double         w = 0., h = 0.;
        gc.GetTextExtent(label, &w, &h);
        if (x > area.m_x && x + 2. * r + 5. * dip + w > area.m_x + area.m_width) {
            x = area.m_x;
            y += line;
        }
        fill_circle(gc, x + r, y, r, SURFACE[kind]);
        x += 2. * r + 5. * dip;
        x += draw_text(gc, label, x, y, pal.text, Label::Body_12, -1) + 12. * dip;
    }
}

void GCodeToMeshPanel::paint_scene(wxGraphicsContext &gc, const Palette &pal, const wxRect2DDouble &area, Scene scene) const
{
    const double seconds = m_clock.Time() / 1000.;
    gc.PushState();
    switch (scene) {
    case Scene::Drop: {
        // A G-code going down into the tray, and back (200 x 110).
        fit(gc, area, 200., 110.);
        gc.PushState();
        gc.Translate(0., 3.5 * (1. - std::cos(PI * std::fmod(seconds, 3.6) / 1.8)));
        fill_round_rect(gc, 74., 8., 52., 64., 6., pal.card);
        stroke_round_rect(gc, 74., 8., 52., 64., 6., pal.strong, 1.5);
        const double   widths[]  = { 34., 26., 30., 20., 28. };
        const wxColour colours[] = { OUTER_WALL, INNER_WALL, SPARSE_INFILL, OUTER_WALL, INNER_WALL };
        for (int i = 0; i < 5; ++i)
            fill_round_rect(gc, 83., 20. + i * 9., widths[i], 4., 2., colours[i]);
        gc.PopState();
        stroke_path(gc, { { 54., 70. }, { 54., 98. }, { 146., 98. }, { 146., 70. } }, pal.accent, 2., false);
        stroke_path(gc, { { 90., 86. }, { 100., 94. }, { 110., 86. } }, pal.accent, 2., false);
        break;
    }
    case Scene::DropMesh:
        // A mesh going down into the tray, and back (200 x 110).
        fit(gc, area, 200., 110.);
        gc.PushState();
        gc.Translate(0., 3.5 * (1. - std::cos(PI * std::fmod(seconds, 3.6) / 1.8)));
        // The block (200 x 120) at half its size, centered above the tray.
        gc.Translate(50., 4.);
        gc.Scale(0.5, 0.5);
        draw_mesh_block(gc, pal.accent);
        gc.PopState();
        stroke_path(gc, { { 54., 70. }, { 54., 98. }, { 146., 98. }, { 146., 70. } }, pal.accent, 2., false);
        stroke_path(gc, { { 90., 86. }, { 100., 94. }, { 110., 86. } }, pal.accent, 2., false);
        break;
    case Scene::Surfaces:
        // A true cylinder and its plane, in the colors of their kinds of surfaces.
        paint_cylinder(gc, area);
        break;
    case Scene::StepFile: {
        // A STEP file and the colors of the surfaces it holds (200 x 120).
        fit(gc, area, 200., 120.);
        fill_round_rect(gc, 70., 12., 60., 84., 6., pal.card);
        stroke_round_rect(gc, 70., 12., 60., 84., 6., pal.strong, 1.5);
        draw_text(gc, "STEP", 100., 44., pal.accent, Label::Head_14, 0);
        for (int i = 0; i < 3; ++i)
            fill_circle(gc, 86. + 14. * i, 70., 5., SURFACE[i]);
        break;
    }
    case Scene::Scan:
        // The layers read one after the other (200 x 120).
        fit(gc, area, 200., 120.);
        draw_beads(gc);
        fill_round_rect(gc, 22., 108. - 86. * std::fmod(seconds / 2.6, 1.), 156., 3., 1.5, pal.accent);
        break;
    case Scene::Mesh:
        fit(gc, area, 200., 120.);
        draw_mesh_block(gc, pal.accent);
        break;
    case Scene::Thumbnail:
        if (m_thumbnail.IsOk())
            draw_bitmap(gc, area, m_thumbnail);
        else
            paint_top(gc, area);
        break;
    case Scene::Top: paint_top(gc, area); break;
    case Scene::Profile: paint_profile(gc, area, with_alpha(pal.text2, 115)); break;
    case Scene::Result: paint_meshes(gc, pal, area); break;
    case Scene::View:
        // The layers of the G-code, in the colors of the Preview (200 x 165).
        fit(gc, area, 200., 165.);
        for (int i = 0; i < 12; ++i) {
            const double y = 128. - i * 7.;
            stroke_path(gc, { { 100., y - 21. }, { 164., y }, { 100., y + 21. }, { 36., y } }, OUTER_WALL, 2., true);
            stroke_path(gc, { { 100., y - 13. }, { 140., y }, { 100., y + 13. }, { 60., y } }, INNER_WALL, 1.5, true);
        }
        break;
    case Scene::Transform: {
        // The layers, then a mesh on the plate (200 x 165).
        fit(gc, area, 200., 165.);
        for (int i = 0; i < 7; ++i) {
            const double y = 118. - i * 8.;
            stroke_path(gc, { { 44., y - 14. }, { 82., y }, { 44., y + 14. }, { 6., y } }, OUTER_WALL, 2., true);
        }
        stroke_path(gc, { { 90., 96. }, { 114., 96. } }, pal.text2, 1.5, false);
        stroke_path(gc, { { 108., 90. }, { 115., 96. }, { 108., 102. } }, pal.text2, 1.5, false);
        const Iso iso{ 158., 104., 0.62 };
        iso_box(gc, iso, -36., -36., -3., 72., 72., 3., pal.plate);
        iso_box(gc, iso, -16., -16., 0., 32., 32., 56., MESH);
        break;
    }
    case Scene::Smooth: {
        // A slope seen from the side: the steps of the layers, or the middles of the layers joined (200 x 120).
        fit(gc, area, 200., 120.);
        const bool   on    = m_smooth != nullptr && m_smooth->value();
        const double H     = 12.;
        const double D     = 11.;
        const double left  = 30.;
        const double right = 150.;
        const int    n     = 7;
        Points2      steps{ { left, 106. } };
        Points2      slope{ { left, 106. }, { right, 106. } };
        for (int i = 0; i < n; ++i) {
            const double y = 106. - i * H;
            const double x = right - i * D;
            steps.emplace_back(x, y);
            steps.emplace_back(x, y - H);
            slope.emplace_back(x, y - H / 2.);
        }
        const double top = 106. - n * H;
        steps.emplace_back(left, top);
        slope.emplace_back(right - (n - 1) * D, top);
        slope.emplace_back(left, top);
        if (on) {
            stroke_path(gc, steps, pal.text2, 1., true, true);
            fill_path(gc, slope, with_alpha(pal.accent, 51));
            stroke_path(gc, slope, pal.accent, 1.5, true);
            for (int i = 0; i < n; ++i)
                fill_circle(gc, right - i * D, 106. - i * H - H / 2., 2., pal.accent);
        } else {
            fill_path(gc, steps, with_alpha(pal.text2, 64));
            stroke_path(gc, steps, pal.text2, 1.2, true);
        }
        break;
    }
    case Scene::Compensation: {
        // The part seen from the side, its first layer shrunk; seen from above, its contour shrunk and its hole grown. Undone:
        // the sizes of the part, dotted (200 x 120).
        fit(gc, area, 200., 120.);
        const bool on = m_undo_available && m_undo != nullptr && m_undo->value();
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(with_alpha(pal.text2, 72)));
        gc.DrawRectangle(20., 22., 70., 74.);
        gc.SetBrush(wxBrush(with_alpha(pal.text2, 128)));
        gc.DrawRectangle(26., 96., 58., 10.);
        fill_round_rect(gc, 117., 23., 66., 78., 11., with_alpha(pal.text2, 72));
        fill_circle(gc, 150., 62., 14., pal.well);
        if (on) {
            stroke_path(gc, { { 20., 96. }, { 90., 96. }, { 90., 106. }, { 20., 106. } }, pal.accent, 1.5, true, true);
            stroke_round_rect(gc, 112., 18., 76., 88., 14., pal.accent, 1.5, true);
            stroke_circle(gc, 150., 62., 9., pal.accent, 1.5, true);
        }
        break;
    }
    }
    gc.PopState();
}

void GCodeToMeshPanel::view()
{
    if (m_path.IsEmpty())
        return;
    Plater *plater = wxGetApp().plater();
    plater->preview_gcode(m_path);
    // Already shown, preview_gcode() does nothing: back to the Preview.
    if (plater->only_gcode_mode())
        wxGetApp().mainframe->select_tab(size_t(MainFrame::tpPreview));
}

wxString GCodeToMeshPanel::object_name(size_t index) const
{
    // PrusaSlicer, Orca Slicer and Klipper name the objects, Bambu Studio only numbers them.
    if (!m_gcode->objects[index].name.empty())
        return from_u8(m_gcode->objects[index].name);
    const wxString stem = wxFileName(m_path).GetName();
    return m_gcode->objects.size() == 1 ? stem : wxString::Format("%s (%d)", stem, int(index + 1));
}

std::vector<indexed_triangle_set> GCodeToMeshPanel::current_meshes()
{
    const bool undo_on = m_undo_available && m_undo->value();
    const bool smooth  = m_smooth->value();
    if (m_meshes.size() == m_gcode->objects.size() && m_meshes_undo == undo_on && m_meshes_smooth == smooth)
        return m_meshes;
    const GCodeToMesh::Compensation   undo = undo_on ? m_gcode->compensation() : GCodeToMesh::Compensation();
    std::vector<indexed_triangle_set> meshes;
    // ponytail: built on the interface thread under a busy cursor; a background job with its progress if large prints
    // take too long.
    wxBusyCursor wait;
    try {
        for (const GCodeToMesh::Object &object : m_gcode->objects)
            meshes.emplace_back(m_gcode->mesh(object, undo, smooth));
    } catch (const std::exception &ex) {
        // Coordinates out of range of the geometry library, out of memory...
        show_error(this, _L("No object could be rebuilt from this G-code.") + "\n" + from_u8(ex.what()));
        return {};
    }
    return meshes;
}

void GCodeToMeshPanel::transform()
{
    if (m_gcode->objects.empty())
        return;
    Plater *plater = wxGetApp().plater();
    // The G-code shown alone in the Preview: back to an empty project first.
    if (plater->only_gcode_mode() && plater->new_project() != wxID_YES)
        return;
    std::vector<indexed_triangle_set> built = current_meshes();
    if (built.empty())
        return;
    // The G-code is in the coordinates of the bed: the objects keep their places, on the current plate.
    const Vec3f                                    origin = plater->get_partplate_list().get_curr_plate()->get_origin().cast<float>();
    std::vector<std::pair<wxString, TriangleMesh>> meshes;
    for (size_t i = 0; i < built.size(); ++i)
        if (!built[i].indices.empty()) {
            TriangleMesh mesh(std::move(built[i]));
            mesh.translate(origin);
            meshes.emplace_back(object_name(i), std::move(mesh));
        }
    if (meshes.empty()) {
        show_error(this, _L("No object could be rebuilt from this G-code."));
        return;
    }
    Plater::TakeSnapshot snapshot(plater, "G-code to Mesh");
    for (const auto &[name, mesh] : meshes)
        plater->sidebar().obj_list()->load_mesh_object(mesh, name, false);
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
}

void GCodeToMeshPanel::request_meshes()
{
    const bool undo   = m_undo_available && m_undo->value();
    const bool smooth = m_smooth->value();
    // Being built, or impossible to build, with these settings already.
    if ((m_building || m_build_failed) && undo == m_wanted_undo && smooth == m_wanted_smooth)
        return;
    m_wanted_undo   = undo;
    m_wanted_smooth = smooth;
    m_build_failed  = false;
    // Back to the settings of the meshes shown, or nothing to build: what is being built is only stopped.
    m_building = !m_gcode->objects.empty() && (m_meshes.empty() || undo != m_meshes_undo || smooth != m_meshes_smooth);
    if (m_building)
        m_gcode_worker.post([this, gcode = m_gcode, undo, smooth](unsigned generation) {
            auto meshes = std::make_shared<std::vector<indexed_triangle_set>>();
            bool failed = false;
            try {
                const GCodeToMesh::Compensation compensation = undo ? gcode->compensation() : GCodeToMesh::Compensation();
                for (const GCodeToMesh::Object &object : gcode->objects)
                    meshes->emplace_back(gcode->mesh(object, compensation, smooth, [this, generation]() {
                        if (m_gcode_worker.stale(generation))
                            throw Canceled();
                    }));
            } catch (...) {
                // Stopped, or coordinates out of range of the geometry library, out of memory...
                failed = true;
            }
            finish(m_gcode_worker, generation, [this, meshes, failed, undo, smooth]() { on_meshes(std::move(*meshes), failed, undo, smooth); });
        });
    else
        m_gcode_worker.stop();
    m_result_canvas->Refresh();
}

void GCodeToMeshPanel::on_meshes(std::vector<indexed_triangle_set> meshes, bool failed, bool undo, bool smooth)
{
    m_building      = false;
    m_build_failed  = failed;
    m_meshes        = failed ? std::vector<indexed_triangle_set>() : std::move(meshes);
    m_meshes_undo   = undo;
    m_meshes_smooth = smooth;
    m_render        = wxBitmap();
    m_result_canvas->Refresh();
}

void GCodeToMeshPanel::recognize()
{
    if (!m_mesh_its)
        return;
    MeshToStep::Settings settings;
    settings.tolerance = TOLERANCES[m_tolerance];
    // A save under way is stopped, the report of the last one no longer holds.
    m_recognizing = true;
    m_saving      = false;
    m_save_report.clear();
    m_step_worker.post([this, mesh = m_mesh_its, settings](unsigned generation) {
        std::shared_ptr<const MeshToStep> step;
        size_t                            open_edges   = 0;
        size_t                            shared_edges = 0;
        std::string                       error;
        try {
            step = std::make_shared<MeshToStep>(*mesh, settings, [this, generation]() { return m_step_worker.stale(generation); });
        } catch (const MeshToStep::Canceled &) {
            return;
        } catch (const MeshToStep::NotClosed &e) {
            open_edges   = e.open_edges;
            shared_edges = e.shared_edges;
        } catch (const std::exception &e) {
            error = e.what();
        }
        finish(m_step_worker, generation, [this, step, open_edges, shared_edges, error]() { on_recognized(step, open_edges, shared_edges, error); });
    });
    update_mesh();
}

void GCodeToMeshPanel::on_recognized(std::shared_ptr<const MeshToStep> step, size_t open, size_t shared, const std::string &error)
{
    m_recognizing  = false;
    m_step         = std::move(step);
    m_open_edges   = open;
    m_shared_edges = shared;
    if (m_step)
        m_closed = 1;
    else if (open + shared > 0)
        m_closed = 0;
    m_step_error = from_u8(error);
    m_face_kinds.clear();
    if (m_step) {
        m_face_kinds.reserve(m_step->regions().size());
        for (int region : m_step->regions())
            m_face_kinds.push_back((unsigned char) m_step->surfaces()[region].kind);
    }
    m_regions_render = wxBitmap();
    update_mesh();
}

void GCodeToMeshPanel::save_step()
{
    if (m_saving) {
        // The button cancels the save under way.
        m_step_worker.stop();
        m_saving      = false;
        m_save_report = _L("Canceled.");
        update_mesh();
        return;
    }
    if (!m_step)
        return;
    const size_t faces = m_step->surfaces().size();
    if (faces > 100000) {
        MessageDialog question(this, wxString::Format(_L("The STEP solid will have %d faces: saving it may take long, and the file be large. Save it anyway?"), int(faces)),
                               _L("Save as STEP"), wxYES_NO | wxICON_QUESTION);
        if (question.ShowModal() != wxID_YES)
            return;
    }
    const wxString folder = m_mesh_folder.IsEmpty() ? from_u8(wxGetApp().app_config->get_last_dir()) : m_mesh_folder;
    wxFileDialog   dialog(this, _L("Save as STEP"), folder, m_mesh_name + ".step", "STEP (*.step;*.stp)|*.step;*.stp", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK)
        return;
    const wxString             path = dialog.GetPath();
    MeshToStep::Settings settings;
    settings.tolerance = TOLERANCES[m_tolerance];
    m_saving        = true;
    m_save_progress = 0.;
    m_save_report.clear();
    m_step_worker.post([this, step = m_step, settings, path, file = into_u8(path), name = into_u8(m_mesh_name)](unsigned generation) {
        const auto canceled = [this, generation]() { return m_step_worker.stale(generation); };
        const auto progress = [this, generation](double done) {
            finish(m_step_worker, generation, [this, done]() {
                m_save_progress = done;
                m_regions->Refresh(false);
            });
        };
        MeshToStep::Result result;
        int                level    = settings.level;
        bool               straight = false;
        size_t             facets   = 0;
        std::string        error;
        try {
            // Auto: a solid of level 4 which OpenCASCADE does not find valid (or cannot make) is made again with the
            // regions of its faces found not valid made of facets, the rest kept; then with straight edges, then with the
            // facets of level 3, flat faces, a face per triangle.
            auto attempt = [&](const MeshToStep &from, bool edges) {
                try {
                    result = from.write(file, name, progress, canceled, edges);
                } catch (const MeshToStep::Canceled &) {
                    throw;
                } catch (const MeshToStep::NotWritten &) {
                    // No other level would write it.
                    throw;
                } catch (const std::exception &) {
                    result = MeshToStep::Result();
                }
            };
            attempt(*step, true);
            std::optional<MeshToStep> faceted;
            for (int round = 0; round < 2 && !result.valid && !result.invalid.empty(); ++round) {
                facets += result.invalid.size();
                faceted.emplace((faceted ? *faceted : *step).with_facets(result.invalid));
                attempt(*faceted, true);
            }
            if (!result.valid) {
                straight = true;
                facets   = 0;
                attempt(*step, false);
            }
            while (!result.valid && level > 1) {
                --level;
                const MeshToStep lower(step->mesh(), { level, settings.tolerance, settings.min_faces }, canceled);
                result = lower.write(file, name, progress, canceled);
            }
        } catch (const MeshToStep::Canceled &) {
            return;
        } catch (const std::exception &e) {
            error = e.what();
        }
        finish(m_step_worker, generation, [this, result, level, straight, facets, error, path]() { on_saved(result, level, straight, facets, error, path); });
    });
    update_mesh();
}

void GCodeToMeshPanel::on_saved(const MeshToStep::Result &result, int level, bool straight, size_t facets, const std::string &error,
                                const wxString &path)
{
    m_saving = false;
    if (!error.empty())
        m_save_report = _L("The STEP file could not be written.") + " " + from_u8(error);
    else {
        wxString report = wxString::Format(_L("Saved: %s."), wxFileName(path).GetFullName());
        if (level == 4 && facets > 0 && !straight)
            report += " " + wxString::Format(_L("%d surfaces made of facets: OpenCASCADE did not find them valid."), int(facets));
        if (level == 4 && straight)
            report += " " + _L("With straight edges: the fitted edges did not make a valid solid.");
        else if (level == 3)
            report += " " + _L("With facets: the surfaces without facets did not make a valid solid.");
        else if (level < 3)
            report += " " + (level == 2 ? _L("With flat faces: the recognized surfaces did not make a valid solid.") :
                                          _L("With a face per triangle: the flat faces did not make a valid solid."));
        const wxString cube   = wxString::FromUTF8(" mm\xC2\xB3");
        const wxString volume = number(result.volume, 1) + cube;
        const wxString mesh   = number(m_mesh_volume, 1) + cube;
        report += " " + wxString::Format(result.valid ? _L("A valid solid: %d faces, %s (mesh: %s).") :
                                                        _L("A solid of %d faces, %s (mesh: %s), which OpenCASCADE does not find valid."),
                                         int(result.faces), volume, mesh);
        if (result.solids > 1)
            report += " " + wxString::Format(_L("%d solids, one per piece of the mesh."), int(result.solids));
        m_save_report = report;
    }
    update_mesh();
}

void GCodeToMeshPanel::update_mesh()
{
    if (!m_mesh_its)
        return;
    // 1: what the mesh measures, closed or not.
    const wxString times = wxString::FromUTF8(" \xC3\x97 ");
    set_text(m_part_info[0], wxNumberFormatter::ToString(long(m_mesh_its->indices.size())));
    set_text(m_part_info[1], number(m_mesh_size.x(), 1) + times + number(m_mesh_size.y(), 1) + times + number(m_mesh_size.z(), 1) + " mm");
    set_text(m_part_info[2], number(m_mesh_volume, 1) + wxString::FromUTF8(" mm\xC2\xB3"));
    set_text(m_part_info[3], m_closed < 0 ? wxString::FromUTF8("\xE2\x80\xA6") : m_closed > 0 ? _L("Yes") : _L("No"));
    m_repair_button->Show(m_closed == 0 && is_windows10());
    m_deviation->Refresh();

    // 3: the surfaces found or why not, the save.
    wxString caption;
    if (m_saving)
        caption = _L("Saving the STEP...");
    else if (!m_save_report.IsEmpty())
        caption = m_save_report;
    else if (m_closed == 0)
        caption = wxString::Format(_L("The mesh is not closed (%d open edges, %d edges of more than two faces): a STEP solid needs a closed mesh."),
                                   int(m_open_edges), int(m_shared_edges)) +
                  (is_windows10() ? " " + _L("Repair it first, with the button of card 1.") : wxString());
    else if (!m_step_error.IsEmpty())
        caption = _L("The surfaces could not be recognized.") + " " + m_step_error;
    else if (!m_step)
        caption = _L("Recognizing the surfaces...");
    else
        caption = _L("A color per kind of surface. What no plane, cylinder, cone, sphere or torus explains becomes an extrusion, "
                     "a revolution, a helix or a free surface: no facets.");
    set_text(m_step_caption, caption);
    m_save_button->SetLabel(m_saving ? _L("Cancel") : _L("Save as STEP..."));
    m_save_button->Enable(m_saving || (m_step != nullptr && !m_recognizing));
    m_legend->Refresh();
    m_regions->Refresh();
    m_part->Refresh();
    relayout();
}

void GCodeToMeshPanel::Worker::post(Job job)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_job            = std::move(job);
        m_job_generation = ++m_generation;
    }
    m_wake.notify_one();
    if (!m_thread.joinable())
        m_thread = std::thread([this]() { work(); });
}

void GCodeToMeshPanel::Worker::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_job = nullptr;
    ++m_generation;
}

void GCodeToMeshPanel::Worker::quit()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void GCodeToMeshPanel::Worker::work()
{
    for (;;) {
        Job      job;
        unsigned generation = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this]() { return m_quit || bool(m_job); });
            if (m_quit)
                return;
            job        = std::move(m_job);
            m_job      = nullptr;
            generation = m_job_generation;
        }
        // A job reports its own errors: one escaping would end the program.
        try {
            job(generation);
        } catch (...) {
        }
    }
}

void GCodeToMeshPanel::finish(const Worker &worker, unsigned generation, std::function<void()> done)
{
    if (!worker.stale(generation))
        CallAfter([&worker, generation, done = std::move(done)]() {
            // A newer job came since.
            if (!worker.stale(generation))
                done();
        });
}

} // namespace GUI
} // namespace Slic3r
