#include "GUI_Utils.hpp"
#include "GUI_App.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <utility>

#include <wx/dcbuffer.h>
#include <wx/log.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/time.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/make_shared.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/weak_ptr.hpp>

#include "nlohmann/json.hpp"

#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Printer/PrinterFileSystem.h"
#include "PartSkipPanel.hpp"
#include "SkipPartCanvas.hpp" // ModelSettingHelper: reads Metadata/slice_info.config
#include "DeviceCore/DevManager.h"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

namespace Slic3r { namespace GUI {

extern void refresh_agora_url(char const *device, char const *dev_ver, char const *channel, void *context, void (*callback)(void *context, char const *url));

namespace {

// Colours of the plate view. It is an image of the plate, drawn the same way in light and dark mode.
struct Rgb
{
    unsigned char r, g, b;
};
constexpr Rgb COL_NO_PLATE        {226, 226, 226};
constexpr Rgb COL_PRINTING        {255, 255, 255};
constexpr Rgb COL_PRINTING_EDGE   {130, 130, 130};
constexpr Rgb COL_HOVER           {220, 238, 254}; // Orio : bleu pâle
constexpr Rgb COL_HOVER_EDGE      {8, 119, 232};
constexpr Rgb COL_SELECTED        {239, 175, 175};
constexpr Rgb COL_SELECTED_HOVER  {246, 206, 206};
constexpr Rgb COL_SELECTED_EDGE   {208, 27, 27};
constexpr Rgb COL_PENDING_STRIPE  {150, 150, 150};
constexpr Rgb COL_SKIPPED         {105, 105, 105};
constexpr Rgb COL_SKIPPED_EDGE    {64, 64, 64};

constexpr int    BOX_WIDTH_DIP         = 586; // same width as the filament (AMS) box above
constexpr int    BOX_MARGIN_DIP        = 14;
constexpr int    MAP_SIZE_DIP          = 250;
constexpr int    INFO_SPACING_DIP      = 16;
constexpr long   LOAD_TIMEOUT_MS       = 60000;
constexpr long   AUTO_RETRY_DELAY_MS   = 20000;
constexpr long   PENDING_TIMEOUT_MS    = 30000;
constexpr long   PUSH_REQUEST_DELAYS_MS[] = {3000, 15000}; // full state asked for after an exclusion
constexpr long   NOTICE_DURATION_MS    = 30000;
constexpr size_t MAX_SKIPPABLE_OBJECTS = 64; // limit of the M624 labels of the G-code
constexpr size_t MAX_LISTED_NAMES      = 8;
constexpr size_t MAX_SENT_PLATES       = 30; // plate files kept for the last prints sent from this computer
constexpr size_t GCODE_HEADER_SIZE     = 16384;

wxColour to_wx(const Rgb &c) { return wxColour(c.r, c.g, c.b); }

Rgb object_colour(PartSkipMap::ObjectState state, bool hover, bool edge, int x, int y)
{
    using State = PartSkipMap::ObjectState;
    switch (state) {
    case State::Selected: return edge ? COL_SELECTED_EDGE : (hover ? COL_SELECTED_HOVER : COL_SELECTED);
    // Striped until the printer reports the object as skipped.
    case State::Pending: return edge ? COL_SELECTED_EDGE : ((((x + y) / 6) % 2) == 0 ? COL_SELECTED : COL_PENDING_STRIPE);
    case State::Skipped: return edge ? COL_SKIPPED_EDGE : COL_SKIPPED;
    case State::Printing:
    default: return edge ? (hover ? COL_HOVER_EDGE : COL_PRINTING_EDGE) : (hover ? COL_HOVER : COL_PRINTING);
    }
}

// Objects can be skipped while the printer runs or pauses a print, not during a calibration. The printer announces
// it ("fun"), or reports its skipped objects without announcing it (P1 series).
bool is_running_print(MachineObject *obj)
{
    if (obj == nullptr || !(obj->is_support_partskip || obj->m_partskip_ids_reported))
        return false;
    if (obj->is_in_calibration() || obj->is_system_printing())
        return false;
    return obj->print_status == "RUNNING" || obj->print_status == "PAUSE";
}

// Identifies the print shown in the box: a new print, or another printer, reloads the plate files.
std::string job_key(MachineObject *obj)
{
    return obj->get_dev_id() + "|" + obj->job_id_ + "|" + obj->task_id_ + "|" + obj->subtask_id_ + "|" + obj->subtask_name + "|" +
           obj->m_gcode_file + "|" + std::to_string(obj->m_plate_index);
}

// Plate files of the prints sent from this computer: one folder per print, named after the time it was sent,
// holding print.json and the Metadata files read by the box (see remember_sent_plate()).
boost::filesystem::path sent_plates_dir() { return boost::filesystem::path(data_dir()) / "orio_partskip"; }

struct SentPlate
{
    std::string dev_id;
    std::string subtask_name; // name of the print, as the printer reports it
    int         plate_idx{0};
    int         total_layers{0}; // from the header of the G-code, 0 if unknown
    long long   time{0};
};

bool read_sent_plate(const boost::filesystem::path &dir, SentPlate &plate)
{
    try {
        boost::nowide::ifstream in((dir / "print.json").string());
        if (!in)
            return false;
        const nlohmann::json j = nlohmann::json::parse(in);
        plate.dev_id       = j.at("dev_id").get<std::string>();
        plate.subtask_name = j.at("subtask_name").get<std::string>();
        plate.plate_idx    = j.at("plate").get<int>();
        plate.total_layers = j.value("total_layers", 0);
        plate.time         = j.at("time").get<long long>();
    } catch (...) {
        return false;
    }
    return !plate.dev_id.empty() && plate.plate_idx > 0;
}

// Copies a file of the 3MF to dest. Returns false if the 3MF does not contain it.
bool extract_3mf_file(mz_zip_archive &archive, const std::string &name, const boost::filesystem::path &dest)
{
    const int                index = mz_zip_reader_locate_file(&archive, name.c_str(), nullptr, 0);
    mz_zip_archive_file_stat stat;
    if (index < 0 || !mz_zip_reader_file_stat(&archive, mz_uint(index), &stat))
        return false;
    std::string data(size_t(stat.m_uncomp_size), '\0');
    if (!data.empty() && !mz_zip_reader_extract_to_mem(&archive, mz_uint(index), &data[0], data.size(), 0))
        return false;
    boost::nowide::ofstream out(dest.string(), std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    return bool(out);
}

// Number of layers written in the header of the G-code of the plate ("; total layer number: N"), the one the
// printer reports while printing. 0 if unknown.
int read_total_layers(mz_zip_archive &archive, int plate_idx)
{
    const std::string name  = "Metadata/plate_" + std::to_string(plate_idx) + ".gcode";
    const int         index = mz_zip_reader_locate_file(&archive, name.c_str(), nullptr, 0);
    if (index < 0)
        return 0;
    // Only the beginning of the G-code is decompressed.
    mz_zip_reader_extract_iter_state *iter = mz_zip_reader_extract_iter_new(&archive, mz_uint(index), 0);
    if (iter == nullptr)
        return 0;
    std::string header(GCODE_HEADER_SIZE, '\0');
    header.resize(mz_zip_reader_extract_iter_read(iter, &header[0], header.size()));
    mz_zip_reader_extract_iter_free(iter);

    const std::string tag = "; total layer number: ";
    const size_t      pos = header.find(tag);
    return pos == std::string::npos ? 0 : std::max(0, std::atoi(header.c_str() + pos + tag.size()));
}

} // namespace

void remember_sent_plate(const std::string &dev_id, const std::string &subtask_name, int plate_idx, const std::string &path_3mf)
{
    namespace bfs = boost::filesystem;
    if (dev_id.empty() || plate_idx <= 0 || path_3mf.empty())
        return;

    // Prints are sent one at a time; the lock keeps the clean-up below away from a folder being written anyway.
    static std::mutex           mutex;
    std::lock_guard<std::mutex> lock(mutex);

    const long long sent_at = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const bfs::path root    = sent_plates_dir();
    // Written aside then renamed: the box never reads a folder being written.
    const bfs::path tmp = root / (std::to_string(sent_at) + ".tmp");
    const bfs::path dir = root / std::to_string(sent_at);

    boost::system::error_code ec;
    bfs::create_directories(tmp / "Metadata", ec);
    bool ok           = !ec;
    int  total_layers = 0;
    if (ok) {
        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);
        ok = open_zip_reader(&archive, path_3mf);
        if (ok) {
            const std::string pick = "pick_" + std::to_string(plate_idx) + ".png";
            ok = extract_3mf_file(archive, "Metadata/" + pick, tmp / "Metadata" / pick) &&
                 extract_3mf_file(archive, "Metadata/slice_info.config", tmp / "Metadata" / "slice_info.config");
            if (ok)
                total_layers = read_total_layers(archive, plate_idx);
            close_zip_reader(&archive);
        }
    }
    if (ok) {
        nlohmann::json j;
        j["dev_id"]       = dev_id;
        j["subtask_name"] = subtask_name;
        j["plate"]        = plate_idx;
        j["total_layers"] = total_layers;
        j["time"]         = sent_at;
        boost::nowide::ofstream out((tmp / "print.json").string(), std::ios::trunc);
        out << j.dump();
        ok = bool(out);
    }
    if (ok) {
        bfs::rename(tmp, dir, ec);
        ok = !ec;
    }
    if (!ok) {
        bfs::remove_all(tmp, ec);
        BOOST_LOG_TRIVIAL(warning) << "part skip panel: the plate files of the print sent could not be kept";
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "part skip panel: plate files of the print sent kept (plate " << plate_idx << ", " << total_layers << " layers)";

    // Only the last prints are kept, and the leftovers of an interrupted copy are removed.
    std::vector<std::pair<long long, bfs::path>> kept;
    std::vector<bfs::path>                       removed;
    for (bfs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const bfs::path path = it->path();
        if (path.extension() == ".tmp") {
            removed.push_back(path);
            continue;
        }
        try {
            kept.emplace_back(std::stoll(path.filename().string()), path);
        } catch (...) {}
    }
    if (kept.size() > MAX_SENT_PLATES) {
        std::sort(kept.begin(), kept.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (size_t i = MAX_SENT_PLATES; i < kept.size(); ++i)
            removed.push_back(kept[i].second);
    }
    for (const bfs::path &path : removed)
        bfs::remove_all(path, ec);
}

// ---------------------------------------------------------------------------------------------------------------
// PartSkipMap
// ---------------------------------------------------------------------------------------------------------------

PartSkipMap::PartSkipMap(wxWindow *parent)
{
    // Painted entirely in on_paint(), with a buffered DC.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    SetBackgroundColour(StaticBox::GetParentBackgroundColor(parent));

    Bind(wxEVT_PAINT, &PartSkipMap::on_paint, this);
    Bind(wxEVT_SIZE, &PartSkipMap::on_size, this);
    Bind(wxEVT_MOTION, &PartSkipMap::on_motion, this);
    Bind(wxEVT_LEAVE_WINDOW, &PartSkipMap::on_leave, this);
    Bind(wxEVT_LEFT_DOWN, &PartSkipMap::on_left_down, this);
}

void PartSkipMap::set_background(const wxString &path)
{
    wxImage image;
    if (!path.empty()) {
        wxLogNull no_log;
        if (!image.LoadFile(path, wxBITMAP_TYPE_PNG))
            image = wxImage();
    }
    m_background_src = image;
    m_background     = wxImage();
    invalidate();
}

bool PartSkipMap::load_picking_image(const wxString &path)
{
    wxImage image;
    {
        wxLogNull no_log;
        if (!image.LoadFile(path, wxBITMAP_TYPE_PNG) || !image.IsOk())
            return false;
    }
    const int w = image.GetWidth();
    const int h = image.GetHeight();
    if (w <= 0 || h <= 0)
        return false;

    // Each object is painted with the colour of its identify_id, on a transparent black background.
    const unsigned char  *rgb   = image.GetData();
    const unsigned char  *alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    std::vector<uint32_t> ids(size_t(w) * size_t(h), 0);
    for (size_t i = 0; i < ids.size(); ++i) {
        if (alpha != nullptr && alpha[i] == 0)
            continue;
        ids[i] = uint32_t(rgb[3 * i]) | (uint32_t(rgb[3 * i + 1]) << 8) | (uint32_t(rgb[3 * i + 2]) << 16);
    }

    // Outline of the objects: about 3 pixels of the picking image, 1.5 pixel on screen.
    const int            edge_width = std::max(2, std::min(w, h) / 170);
    std::vector<uint8_t> edges(ids.size(), 0);
    auto                 differs = [&ids, w, h](int x, int y, uint32_t id) {
        return x < 0 || y < 0 || x >= w || y >= h || ids[size_t(y) * size_t(w) + size_t(x)] != id;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t id = ids[size_t(y) * size_t(w) + size_t(x)];
            if (id == 0)
                continue;
            for (int d = 1; d <= edge_width; ++d)
                if (differs(x - d, y, id) || differs(x + d, y, id) || differs(x, y - d, id) || differs(x, y + d, id)) {
                    edges[size_t(y) * size_t(w) + size_t(x)] = 1;
                    break;
                }
        }

    m_width  = w;
    m_height = h;
    m_ids.swap(ids);
    m_edges.swap(edges);
    m_background = wxImage();
    m_hover      = 0;
    update_tooltip();
    invalidate();
    return true;
}

void PartSkipMap::clear()
{
    m_width  = 512;
    m_height = 512;
    m_ids.clear();
    m_edges.clear();
    m_objects.clear();
    m_background = wxImage();
    m_hover      = 0;
    update_tooltip();
    invalidate();
}

void PartSkipMap::set_objects(const std::map<uint32_t, Object> &objects)
{
    if (objects == m_objects)
        return;
    m_objects = objects;
    if (m_hover != 0 && m_objects.find(m_hover) == m_objects.end())
        m_hover = 0;
    update_tooltip();
    invalidate();
}

void PartSkipMap::set_clickable(bool clickable)
{
    if (m_clickable == clickable)
        return;
    m_clickable = clickable;
    update_tooltip();
    invalidate();
}

void PartSkipMap::msw_rescale()
{
    m_bitmap_dirty = true;
    Refresh();
}

wxRect PartSkipMap::view_rect() const
{
    const wxSize size = GetClientSize();
    if (size.x <= 0 || size.y <= 0 || m_width <= 0 || m_height <= 0)
        return wxRect();
    const double scale = std::min(double(size.x) / double(m_width), double(size.y) / double(m_height));
    const int    w     = std::max(1, int(m_width * scale));
    const int    h     = std::max(1, int(m_height * scale));
    return wxRect((size.x - w) / 2, (size.y - h) / 2, w, h);
}

uint32_t PartSkipMap::object_at(const wxPoint &pos) const
{
    if (m_ids.empty())
        return 0;
    const wxRect rect = view_rect();
    if (rect.IsEmpty() || !rect.Contains(pos))
        return 0;
    const int      x  = std::clamp(int((long long) (pos.x - rect.x) * m_width / rect.width), 0, m_width - 1);
    const int      y  = std::clamp(int((long long) (pos.y - rect.y) * m_height / rect.height), 0, m_height - 1);
    const uint32_t id = m_ids[size_t(y) * size_t(m_width) + size_t(x)];
    return (id != 0 && m_objects.find(id) != m_objects.end()) ? id : 0;
}

bool PartSkipMap::is_clickable(uint32_t id) const
{
    if (!m_clickable || id == 0)
        return false;
    auto it = m_objects.find(id);
    return it != m_objects.end() && (it->second.state == ObjectState::Printing || it->second.state == ObjectState::Selected);
}

void PartSkipMap::set_hover(uint32_t id)
{
    if (id == m_hover)
        return;
    m_hover = id;
    update_tooltip();
    invalidate();
}

void PartSkipMap::update_tooltip()
{
    auto it = m_objects.find(m_hover);
    if (m_hover == 0 || it == m_objects.end()) {
        UnsetToolTip();
        SetCursor(wxCursor(wxCURSOR_ARROW));
        return;
    }
    wxString tip = it->second.name;
    switch (it->second.state) {
    case ObjectState::Printing:
        if (m_clickable)
            tip += "\n" + _L("Click to select it for exclusion.");
        break;
    case ObjectState::Selected: tip += "\n" + _L("Selected for exclusion. Click again to deselect it."); break;
    case ObjectState::Pending: tip += "\n" + _L("Exclusion requested, waiting for the printer."); break;
    case ObjectState::Skipped: tip += "\n" + _L("Excluded: the printer no longer prints it."); break;
    }
    SetToolTip(tip);
    SetCursor(wxCursor(is_clickable(m_hover) ? wxCURSOR_HAND : wxCURSOR_ARROW));
}

void PartSkipMap::invalidate()
{
    m_composed_dirty = true;
    m_bitmap_dirty   = true;
    Refresh();
}

void PartSkipMap::compose()
{
    const int    w     = m_width;
    const int    h     = m_height;
    const size_t count = size_t(w) * size_t(h);

    // The plate silhouette is stretched over the picking image, as on the printer screen.
    if (m_background_src.IsOk() && (!m_background.IsOk() || m_background.GetWidth() != w || m_background.GetHeight() != h))
        m_background = m_background_src.Scale(w, h, wxIMAGE_QUALITY_HIGH);
    const bool           has_background = m_background_src.IsOk() && m_background.IsOk();
    const unsigned char *bg             = has_background ? m_background.GetData() : nullptr;
    const unsigned char *bg_alpha       = has_background && m_background.HasAlpha() ? m_background.GetAlpha() : nullptr;
    const wxColour       panel          = GetBackgroundColour();
    const bool           has_objects    = m_ids.size() == count && m_edges.size() == count;
    const bool           hover_active   = is_clickable(m_hover);

    wxImage        image(w, h, false);
    unsigned char *out = image.GetData();
    for (size_t i = 0; i < count; ++i) {
        Rgb c = COL_NO_PLATE;
        if (bg != nullptr) {
            c = Rgb{bg[3 * i], bg[3 * i + 1], bg[3 * i + 2]};
            if (bg_alpha != nullptr && bg_alpha[i] < 255) {
                // Rounded corners of the plate: blend with the box background.
                const unsigned a = bg_alpha[i];
                c.r              = (unsigned char) ((c.r * a + panel.Red() * (255 - a)) / 255);
                c.g              = (unsigned char) ((c.g * a + panel.Green() * (255 - a)) / 255);
                c.b              = (unsigned char) ((c.b * a + panel.Blue() * (255 - a)) / 255);
            }
        }
        if (has_objects && m_ids[i] != 0) {
            auto it = m_objects.find(m_ids[i]);
            if (it != m_objects.end())
                c = object_colour(it->second.state, hover_active && m_ids[i] == m_hover, m_edges[i] != 0, int(i % size_t(w)), int(i / size_t(w)));
        }
        out[3 * i]     = c.r;
        out[3 * i + 1] = c.g;
        out[3 * i + 2] = c.b;
    }
    m_composed       = image;
    m_composed_dirty = false;
    m_bitmap_dirty   = true;
}

void PartSkipMap::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    const wxRect rect = view_rect();
    if (rect.IsEmpty())
        return;
    if (m_composed_dirty || !m_composed.IsOk())
        compose();
    if (m_bitmap_dirty || !m_bitmap.IsOk() || m_bitmap.GetWidth() != rect.width || m_bitmap.GetHeight() != rect.height) {
        m_bitmap       = wxBitmap(m_composed.Scale(rect.width, rect.height, wxIMAGE_QUALITY_HIGH));
        m_bitmap_dirty = false;
    }
    dc.DrawBitmap(m_bitmap, rect.GetPosition());
}

void PartSkipMap::on_size(wxSizeEvent &event)
{
    m_bitmap_dirty = true;
    Refresh();
    event.Skip();
}

void PartSkipMap::on_motion(wxMouseEvent &event)
{
    set_hover(object_at(event.GetPosition()));
    event.Skip();
}

void PartSkipMap::on_leave(wxMouseEvent &event)
{
    set_hover(0);
    event.Skip();
}

void PartSkipMap::on_left_down(wxMouseEvent &event)
{
    const uint32_t id = object_at(event.GetPosition());
    if (is_clickable(id) && m_on_click)
        m_on_click(id);
    event.Skip();
}

// ---------------------------------------------------------------------------------------------------------------
// PartSkipPanel
// ---------------------------------------------------------------------------------------------------------------

PartSkipPanel::PartSkipPanel(wxWindow *parent) : StaticBox(parent)
{
    // Same box as the filament (AMS) one.
    SetBackgroundColor(StateColor(std::pair<wxColour, int>(*wxWHITE, StateColor::Normal)));
    SetBorderColor(StateColor(std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Normal)));
    SetCornerRadius(5);
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    create_ui();
}

PartSkipPanel::~PartSkipPanel()
{
    release_file_system(false);
    if (!m_tmp_dir.empty()) {
        boost::system::error_code ec;
        boost::filesystem::remove_all(boost::filesystem::path(m_tmp_dir), ec);
    }
}

void PartSkipPanel::create_ui()
{
    const wxColour text_dark  = StateColor::darkModeColorFor(wxColour("#262E30"));
    const wxColour text_light = StateColor::darkModeColorFor(wxColour("#6B6B6B"));

    auto *title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_title           = new Label(this, Label::Head_14, _L("Objects being printed"));
    m_title->SetForegroundColour(text_dark);
    m_counter = new Label(this, Label::Body_12, wxEmptyString);
    m_counter->SetForegroundColour(text_light);
    title_sizer->Add(m_title, 0, wxALIGN_CENTER_VERTICAL);
    title_sizer->AddStretchSpacer();
    title_sizer->Add(m_counter, 0, wxALIGN_CENTER_VERTICAL);

    m_map = new PartSkipMap(this);
    m_map->SetMinSize(wxSize(FromDIP(MAP_SIZE_DIP), FromDIP(MAP_SIZE_DIP)));
    m_map->SetMaxSize(wxSize(FromDIP(MAP_SIZE_DIP), FromDIP(MAP_SIZE_DIP)));
    m_map->set_on_click([this](uint32_t id) { on_object_clicked(id); });

    m_message = new Label(this, Label::Body_13, wxEmptyString);
    m_message->SetForegroundColour(text_dark);

    // Legend of the colours of the map.
    m_legend_sizer = new wxBoxSizer(wxVERTICAL);
    const std::pair<Rgb, wxString> legend[] = {
        {COL_PRINTING, _L("Being printed")},
        {COL_SELECTED, _L("Selected to exclude")},
        {COL_SKIPPED, _L("Excluded from print")},
    };
    for (const auto &item : legend) {
        auto *row    = new wxBoxSizer(wxHORIZONTAL);
        auto *swatch = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(12), FromDIP(12)), wxBORDER_SIMPLE);
        swatch->SetMinSize(wxSize(FromDIP(12), FromDIP(12)));
        swatch->SetBackgroundColour(to_wx(item.first));
        auto *label = new Label(this, Label::Body_12, item.second);
        label->SetForegroundColour(text_light);
        row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        m_legend_sizer->Add(row, 0, wxTOP, FromDIP(4));
        m_legend_swatches.push_back(swatch);
        m_legend_windows.push_back(swatch);
        m_legend_windows.push_back(label);
    }

    // Red: excluding an object cannot be undone.
    m_exclude_btn = new Button(this, _L("Exclude"));
    StateColor exclude_bg(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Disabled),
                          std::pair<wxColour, int>(wxColour(170, 20, 20), StateColor::Pressed),
                          std::pair<wxColour, int>(wxColour(225, 71, 71), StateColor::Hovered),
                          std::pair<wxColour, int>(wxColour(208, 27, 27), StateColor::Normal));
    m_exclude_btn->SetBackgroundColor(exclude_bg);
    m_exclude_btn->SetBorderColor(exclude_bg);
    m_exclude_btn->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Normal)));
    m_exclude_btn->SetFont(Label::Body_13);
    m_exclude_btn->SetMinSize(wxSize(FromDIP(110), FromDIP(30)));
    m_exclude_btn->SetCornerRadius(FromDIP(15));
    m_exclude_btn->Enable(false);
    m_exclude_btn->Hide();
    m_exclude_btn->Bind(wxEVT_BUTTON, &PartSkipPanel::on_exclude, this);

    m_retry_btn = new Button(this, _L("Retry"));
    StateColor retry_bg(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                        std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    StateColor retry_fg(std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Normal));
    m_retry_btn->SetBackgroundColor(retry_bg);
    m_retry_btn->SetBorderColor(retry_fg);
    m_retry_btn->SetTextColor(retry_fg);
    m_retry_btn->SetFont(Label::Body_13);
    m_retry_btn->SetMinSize(wxSize(FromDIP(90), FromDIP(30)));
    m_retry_btn->SetCornerRadius(FromDIP(15));
    m_retry_btn->Hide();
    m_retry_btn->Bind(wxEVT_BUTTON, &PartSkipPanel::on_retry, this);

    auto *button_sizer = new wxBoxSizer(wxHORIZONTAL);
    button_sizer->Add(m_exclude_btn, 0, wxRIGHT, FromDIP(8));
    button_sizer->Add(m_retry_btn, 0);

    auto *info_sizer = new wxBoxSizer(wxVERTICAL);
    info_sizer->Add(m_message, 0, wxEXPAND);
    info_sizer->Add(m_legend_sizer, 0, wxTOP, FromDIP(8));
    info_sizer->AddStretchSpacer();
    info_sizer->Add(button_sizer, 0, wxTOP, FromDIP(12));

    auto *body_sizer = new wxBoxSizer(wxHORIZONTAL);
    body_sizer->Add(m_map, 0, wxALIGN_TOP);
    body_sizer->Add(info_sizer, 1, wxEXPAND | wxLEFT, FromDIP(INFO_SPACING_DIP));

    auto *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(title_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(BOX_MARGIN_DIP));
    main_sizer->Add(body_sizer, 0, wxEXPAND | wxALL, FromDIP(BOX_MARGIN_DIP));

    SetMinSize(wxSize(FromDIP(BOX_WIDTH_DIP), -1));
    SetMaxSize(wxSize(FromDIP(BOX_WIDTH_DIP), -1));
    SetSizer(main_sizer);
    refresh_view();
    Layout();
    Fit();
}

int PartSkipPanel::info_width() const
{
    return FromDIP(BOX_WIDTH_DIP - 2 * BOX_MARGIN_DIP - MAP_SIZE_DIP - INFO_SPACING_DIP - 8);
}

bool PartSkipPanel::update(MachineObject *obj)
{
    if (!is_running_print(obj)) {
        if (!m_job_key.empty() || m_load_state != LoadState::Idle)
            clear_job();
        return false;
    }

    // Plate silhouette of the printer model, as in the "Skip objects" dialog.
    const std::string background = obj->is_series_o() ? "skippart_bg_o.png" : (obj->printer_type == "N1" ? "skippart_bg_a1mini.png" : "skippart_bg_default.png");
    if (background != m_background_name) {
        m_background_name = background;
        m_map->set_background(wxString::FromUTF8((boost::format("%1%/images/%2%") % resources_dir() % background).str().c_str()));
    }

    const std::string key = job_key(obj);
    // The plate files kept when a print was sent must be those of the running print: the printer also reports its
    // number of layers, sometimes a little after the start.
    const bool other_print = m_load_state == LoadState::Ready && m_sent_layers > 0 && obj->total_layers > 0 && obj->total_layers != m_sent_layers;
    if (other_print)
        BOOST_LOG_TRIVIAL(warning) << "part skip panel: the print sent from this computer has " << m_sent_layers << " layers, the running one "
                                   << obj->total_layers;
    if (key != m_job_key || other_print) {
        // The P1 series sends its full state, with the list of skipped objects, every 5 minutes: the list of the
        // previous print could be shown on the objects of this one until then.
        if (key != m_job_key)
            obj->command_request_push_all(true);
        clear_job();
        m_job_key = key;
        start_loading(obj);
    }

    const wxLongLong now = wxGetLocalTimeMillis();
    if (m_load_state == LoadState::Loading && now - m_load_start > LOAD_TIMEOUT_MS) {
        BOOST_LOG_TRIVIAL(warning) << "part skip panel: loading the plate files timed out";
        finish_loading(false);
    } else if (m_load_state == LoadState::Failed && !m_auto_retried && now - m_failed_at > AUTO_RETRY_DELAY_MS) {
        // A print that has just started can keep the printer busy: one more try on its own.
        m_auto_retried = true;
        start_loading(obj);
    }

    sync_with_printer(obj);
    refresh_view();
    return true;
}

void PartSkipPanel::reset()
{
    clear_job();
}

void PartSkipPanel::clear_job()
{
    release_file_system(true);
    if (!m_tmp_dir.empty()) {
        boost::system::error_code ec;
        boost::filesystem::remove_all(boost::filesystem::path(m_tmp_dir), ec);
        m_tmp_dir.clear();
    }
    m_job_key.clear();
    m_dev_id.clear();
    m_sent_layers   = 0;
    m_can_download  = false;
    m_pending_since = 0;
    m_push_requests = 0;
    m_load_state    = LoadState::Idle;
    m_auto_retried  = false;
    m_can_skip      = false;
    m_cannot_skip_reason.clear();
    m_names.clear();
    m_selected.clear();
    m_pending.clear();
    m_skipped.clear();
    m_notice.clear();
    m_map->clear();
    refresh_view();
}

void PartSkipPanel::start_loading(MachineObject *obj)
{
    release_file_system(true);
    boost::system::error_code ec;
    if (!m_tmp_dir.empty()) {
        boost::filesystem::remove_all(boost::filesystem::path(m_tmp_dir), ec);
        m_tmp_dir.clear();
    }

    m_dev_id      = obj->get_dev_id();
    m_plate_idx   = obj->m_plate_index > 0 ? obj->m_plate_index : 1;
    m_sent_layers = 0;
    m_load_state  = LoadState::Loading;
    m_load_start  = wxGetLocalTimeMillis();
    m_map->clear();

    // Print sent from this computer: its plate files were kept when it was sent, no need to ask the printer.
    if (find_sent_plate(obj)) {
        if (read_plate_files()) {
            m_load_state = LoadState::Ready;
            BOOST_LOG_TRIVIAL(info) << "part skip panel: " << m_names.size() << " objects on plate " << m_plate_idx
                                    << " of the print sent from this computer, skipping " << (m_can_skip ? "possible" : "impossible");
            refresh_view();
            return;
        }
        BOOST_LOG_TRIVIAL(warning) << "part skip panel: the plate files kept when the print was sent could not be read";
        m_sent_layers = 0;
        m_plate_idx   = obj->m_plate_index > 0 ? obj->m_plate_index : 1;
        m_map->clear();
    }

    // Otherwise the files are read from the 3MF of the print, stored on the printer. One folder per download: a
    // failed download never leaves the files of another print behind.
    const std::string             dev  = m_dev_id.size() > 6 ? m_dev_id.substr(0, 3) + m_dev_id.substr(m_dev_id.size() - 3) : m_dev_id;
    const boost::filesystem::path dir  = boost::filesystem::path(temporary_dir()) / "orio_partskip" / (dev + "_" + std::to_string(++m_load_counter));
    const boost::filesystem::path meta = dir / "Metadata";
    boost::filesystem::create_directories(meta, ec);
    m_tmp_dir = dir.string();

    const std::string pick_name = "pick_" + std::to_string(m_plate_idx) + ".png";
    // GetPickImages() expects these three files, in this order.
    m_local_paths  = {(meta / pick_name).string(), (meta / "model_settings.config").string(), (meta / "slice_info.config").string()};
    m_target_paths = {"Metadata/" + pick_name, "Metadata/model_settings.config", "Metadata/slice_info.config"};

    // First the printer itself on the local network, then the cloud tunnel (except in LAN only mode). The local
    // connection only when the printer announces access to its files, as for the storage browser: the X1 and P1
    // series do not in LAN mode (connection refused, -9001), and the P1 series camera uses the same port.
    const bool can_try_local = !obj->get_dev_ip().empty() && obj->file_local == MachineObject::FL_Local;
    m_can_try_remote         = !obj->is_lan_mode_printer();
    m_can_download           = can_try_local || m_can_try_remote;
    m_url_attempt            = can_try_local ? UrlAttempt::Local : UrlAttempt::Remote;
    if (!m_can_download) {
        finish_loading(false);
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "part skip panel: loading the files of plate " << m_plate_idx << " from the printer";
    m_file_sys = boost::make_shared<PrinterFileSystem>();
    m_file_sys->Attached();
    m_file_sys->Bind(EVT_STATUS_CHANGED, &PartSkipPanel::on_fs_status, this);
    m_file_sys->Bind(EVT_RAMDOWNLOAD, &PartSkipPanel::on_fs_download, this);
    m_file_sys->Start();
    refresh_view();
}

// Looks for the plate files kept when the running print was sent from this computer (remember_sent_plate()):
// same printer and same print name, and the same plate and number of layers when the printer reports them.
// The most recent one wins: sending a print under the same name replaces the file on the printer too.
bool PartSkipPanel::find_sent_plate(MachineObject *obj)
{
    namespace bfs = boost::filesystem;
    boost::system::error_code ec;
    const bfs::path           root = sent_plates_dir();
    if (obj->subtask_name.empty() || !bfs::is_directory(root, ec))
        return false;

    SentPlate best;
    bfs::path best_dir;
    for (bfs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const bfs::path dir = it->path();
        SentPlate       plate;
        if (dir.extension() == ".tmp" || !read_sent_plate(dir, plate))
            continue;
        if (plate.dev_id != obj->get_dev_id() || plate.subtask_name != obj->subtask_name)
            continue;
        if ((obj->m_plate_index > 0 && plate.plate_idx != obj->m_plate_index) ||
            (obj->total_layers > 0 && plate.total_layers > 0 && plate.total_layers != obj->total_layers))
            continue;
        if (best_dir.empty() || plate.time > best.time) {
            best     = plate;
            best_dir = dir;
        }
    }
    if (best_dir.empty())
        return false;

    const bfs::path   meta = best_dir / "Metadata";
    const std::string pick = "pick_" + std::to_string(best.plate_idx) + ".png";
    // Same order as for GetPickImages(); model_settings.config is not read.
    m_local_paths = {(meta / pick).string(), (meta / "model_settings.config").string(), (meta / "slice_info.config").string()};
    m_plate_idx   = best.plate_idx;
    m_sent_layers = best.total_layers;
    return true;
}

void PartSkipPanel::finish_loading(bool downloaded)
{
    release_file_system(true);
    if (downloaded && read_plate_files()) {
        m_load_state = LoadState::Ready;
        BOOST_LOG_TRIVIAL(info) << "part skip panel: " << m_names.size() << " objects on plate " << m_plate_idx << ", skipping "
                                << (m_can_skip ? "possible" : "impossible");
    } else {
        BOOST_LOG_TRIVIAL(warning) << "part skip panel: the plate files could not be loaded";
        m_load_state = LoadState::Failed;
        m_failed_at  = wxGetLocalTimeMillis();
        m_map->clear();
    }
    refresh_view();
}

bool PartSkipPanel::read_plate_files()
{
    if (m_local_paths.size() < 3)
        return false;

    ModelSettingHelper helper(m_local_paths[2]);
    if (!helper.Parse()) {
        helper.log_errors();
        return false;
    }
    const std::vector<ObjectInfo> objects = helper.GetPlateObjects(m_plate_idx);

    // The instances of an object share its name: number them to tell them apart.
    std::map<std::string, int> name_count;
    for (const ObjectInfo &object : objects)
        ++name_count[object.name];
    std::map<std::string, int> name_index;
    m_names.clear();
    for (const ObjectInfo &object : objects) {
        if (object.identify_id <= 0)
            continue;
        wxString name = wxString::FromUTF8(object.name.c_str());
        if (name.empty())
            name = wxString::Format("#%d", object.identify_id);
        else if (name_count[object.name] > 1)
            name += wxString::Format(" #%d", ++name_index[object.name]);
        m_names[uint32_t(object.identify_id)] = name;
    }
    if (m_names.empty())
        return false;

    // The G-code labels the objects (M624/M625) only for 2 to 64 objects, out of calibrations.
    m_can_skip = false;
    if (m_names.size() < 2)
        m_cannot_skip_reason = _L("Only one object on the plate: nothing to exclude.");
    else if (m_names.size() > MAX_SKIPPABLE_OBJECTS)
        m_cannot_skip_reason = _L("More than 64 objects on the plate: they cannot be excluded.");
    else if (!helper.GetLabelObjectEnabled(m_plate_idx))
        m_cannot_skip_reason = _L("This print cannot exclude objects.");
    else {
        m_can_skip = true;
        m_cannot_skip_reason.clear();
    }
    return m_map->load_picking_image(wxString::FromUTF8(m_local_paths[0].c_str()));
}

void PartSkipPanel::release_file_system(bool deferred)
{
    if (!m_file_sys)
        return;
    boost::shared_ptr<PrinterFileSystem> fs;
    fs.swap(m_file_sys);
    fs->Unbind(EVT_STATUS_CHANGED, &PartSkipPanel::on_fs_status, this);
    fs->Unbind(EVT_RAMDOWNLOAD, &PartSkipPanel::on_fs_download, this);
    if (deferred)
        // May be called from an event of this file system: keep it alive until that event is processed.
        wxGetApp().CallAfter([fs]() { fs->Stop(true); });
    else
        fs->Stop(true);
}

// Same addresses as the "Skip objects" dialog (PartSkipDialog::fetchUrl) for the printer on the local network, and
// as the storage browser of the Device tab (MediaFilePanel::fetchUrl) through the cloud.
void PartSkipPanel::fetch_url()
{
    if (!m_file_sys)
        return;
    DeviceManager *dm  = wxGetApp().getDeviceManager();
    MachineObject *obj = dm ? dm->get_selected_machine() : nullptr;
    if (obj == nullptr || obj->get_dev_id() != m_dev_id) {
        m_file_sys->SetUrl("0");
        return;
    }

    NetworkAgent     *agent         = wxGetApp().getAgent();
    const std::string agent_version = agent ? agent->get_version() : "";
    const std::string dev_id        = obj->get_dev_id();
    const std::string dev_ver       = obj->get_ota_version();

    if (m_url_attempt == UrlAttempt::Local) {
        m_file_sys->SetUrl("bambu:///local/" + obj->get_dev_ip() + "?port=6000&user=bblp&passwd=" + obj->get_access_code());
        return;
    }

    if (agent == nullptr) {
        m_file_sys->SetUrl("3");
        return;
    }
    int remote = obj->get_file_remote();
    if (remote <= 0 || remote > 3)
        remote = 3;
    const std::string                  protocols[] = {"", "\"tutk\"", "\"agora\"", "\"tutk\",\"agora\""};
    boost::weak_ptr<PrinterFileSystem> wfs(m_file_sys);
    agent->get_camera_url(dev_id + "|" + dev_ver + "|" + protocols[remote], [wfs, dev_id, agent_version, dev_ver](std::string url) {
        if (boost::algorithm::starts_with(url, "bambu:///")) {
            url += "&device=" + dev_id;
            url += "&net_ver=" + agent_version;
            url += "&dev_ver=" + dev_ver;
            url += "&refresh_url=" + boost::lexical_cast<std::string>(&refresh_agora_url);
            url += "&cli_id=" + wxGetApp().app_config->get("slicer_uuid");
            url += "&cli_ver=" + std::string(SLIC3R_VERSION);
        } else {
            url = "3";
        }
        wxGetApp().CallAfter([wfs, url]() {
            if (boost::shared_ptr<PrinterFileSystem> fs = wfs.lock())
                fs->SetUrl(url);
        });
    });
}

void PartSkipPanel::on_fs_status(wxCommandEvent &event)
{
    event.Skip();
    if (!m_file_sys || event.GetEventObject() != static_cast<wxObject *>(m_file_sys.get()) || m_load_state != LoadState::Loading)
        return;

    switch (event.GetInt()) {
    case PrinterFileSystem::Initializing: fetch_url(); break;
    case PrinterFileSystem::ListSyncing: m_file_sys->GetPickImages(m_local_paths, m_target_paths); break;
    case PrinterFileSystem::Failed:
        if (m_url_attempt == UrlAttempt::Local && m_can_try_remote) {
            BOOST_LOG_TRIVIAL(info) << "part skip panel: no local connection to the printer, trying through the cloud";
            m_url_attempt = UrlAttempt::Remote;
            // Wakes the connection loop now instead of 10 s later; it asks for the next address.
            m_file_sys->Retry();
        } else {
            BOOST_LOG_TRIVIAL(warning) << "part skip panel: connection to the printer failed, error " << event.GetExtraLong();
            finish_loading(false);
        }
        break;
    default: break;
    }
}

void PartSkipPanel::on_fs_download(wxCommandEvent &event)
{
    event.Skip();
    if (!m_file_sys || event.GetEventObject() != static_cast<wxObject *>(m_file_sys.get()) || m_load_state != LoadState::Loading)
        return;
    finish_loading(event.GetInt() == 0);
}

void PartSkipPanel::sync_with_printer(MachineObject *obj)
{
    std::set<uint32_t> skipped;
    for (int id : obj->m_partskip_ids)
        if (id > 0)
            skipped.insert(uint32_t(id));
    m_skipped.swap(skipped);

    const wxLongLong now = wxGetLocalTimeMillis();
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (m_skipped.count(it->first) != 0) {
            it = m_pending.erase(it);
        } else if (now - it->second > PENDING_TIMEOUT_MS) {
            BOOST_LOG_TRIVIAL(warning) << "part skip panel: the printer did not confirm the exclusion of object " << it->first;
            m_notice      = _L("The printer did not confirm the exclusion. You can try again.");
            m_notice_time = now;
            it            = m_pending.erase(it);
        } else
            ++it;
    }
    // Between two full states (every 5 minutes), the P1 series only sends what changed, maybe without the list of
    // skipped objects: ask for the full state a few seconds after an exclusion.
    if (!m_pending.empty() && m_push_requests < std::size(PUSH_REQUEST_DELAYS_MS) &&
        now - m_pending_since > PUSH_REQUEST_DELAYS_MS[m_push_requests]) {
        obj->command_request_push_all(true);
        ++m_push_requests;
    }
    for (auto it = m_selected.begin(); it != m_selected.end();)
        it = m_skipped.count(*it) != 0 ? m_selected.erase(it) : std::next(it);
    if (!m_notice.empty() && now - m_notice_time > NOTICE_DURATION_MS)
        m_notice.clear();
}

void PartSkipPanel::on_object_clicked(uint32_t id)
{
    if (m_load_state != LoadState::Ready || !m_can_skip || m_names.find(id) == m_names.end() || m_skipped.count(id) != 0 || m_pending.count(id) != 0)
        return;
    if (m_selected.erase(id) == 0)
        m_selected.insert(id);
    m_notice.clear();
    refresh_view();
}

void PartSkipPanel::on_exclude(wxCommandEvent &event)
{
    event.Skip();
    if (m_load_state != LoadState::Ready || !m_can_skip || m_selected.empty())
        return;

    size_t remaining = 0;
    for (const auto &[id, name] : m_names)
        if (m_skipped.count(id) == 0 && m_pending.count(id) == 0)
            ++remaining;
    // As in the "Skip objects" dialog, excluding every object left stops the print.
    const bool stop_print = m_selected.size() >= remaining;

    wxString message;
    if (m_selected.size() == 1) {
        message = wxString::Format(_L("Exclude \"%s\" from the print?"), m_names[*m_selected.begin()]);
    } else {
        message = wxString::Format(_L("Exclude these %d objects from the print?"), int(m_selected.size()));
        size_t listed = 0;
        for (uint32_t id : m_selected) {
            if (listed++ == MAX_LISTED_NAMES) {
                message += "\n- ...";
                break;
            }
            message += "\n- " + m_names[id];
        }
    }
    message += "\n\n";
    if (stop_print)
        message += _L("No object would be left: the print will be stopped.");
    else if (m_selected.size() == 1)
        message += _L("The printer will not print it anymore. This cannot be undone.");
    else
        message += _L("The printer will not print them anymore. This cannot be undone.");

    // The panel may be updated while the dialog is open: remember what is being confirmed.
    const std::string        job      = m_job_key;
    const std::set<uint32_t> selected = m_selected;

    MessageDialog dlg(this, message, _L("Exclude from the print"), wxOK | wxCANCEL | wxICON_WARNING);
    dlg.SetButtonLabel(wxID_OK, stop_print ? _L("Stop printing") : _L("Exclude"));
    if (dlg.ShowModal() != wxID_OK)
        return;

    DeviceManager *dm  = wxGetApp().getDeviceManager();
    MachineObject *obj = dm ? dm->get_selected_machine() : nullptr;
    if (obj == nullptr || m_job_key != job || m_load_state != LoadState::Ready || obj->get_dev_id() != m_dev_id || !is_running_print(obj)) {
        BOOST_LOG_TRIVIAL(warning) << "part skip panel: the print changed, exclusion cancelled";
        return;
    }

    if (stop_print) {
        BOOST_LOG_TRIVIAL(info) << "part skip panel: every object left excluded, stopping the print";
        obj->command_task_abort();
        m_selected.clear();
    } else {
        std::vector<int> ids;
        for (uint32_t id : selected)
            if (m_skipped.count(id) == 0 && m_pending.count(id) == 0)
                ids.push_back(int(id));
        if (ids.empty())
            return;
        BOOST_LOG_TRIVIAL(info) << "part skip panel: excluding " << ids.size() << " objects";
        obj->command_task_partskip(ids);
        const wxLongLong now = wxGetLocalTimeMillis();
        for (int id : ids) {
            m_pending[uint32_t(id)] = now;
            m_selected.erase(uint32_t(id));
        }
        m_pending_since = now;
        m_push_requests = 0;
    }
    m_notice.clear();
    refresh_view();
}

void PartSkipPanel::on_retry(wxCommandEvent &event)
{
    event.Skip();
    if (m_load_state != LoadState::Failed)
        return;
    DeviceManager *dm  = wxGetApp().getDeviceManager();
    MachineObject *obj = dm ? dm->get_selected_machine() : nullptr;
    if (is_running_print(obj) && obj->get_dev_id() == m_dev_id)
        start_loading(obj);
}

void PartSkipPanel::set_message(const wxString &message)
{
    if (message == m_message_text)
        return;
    m_message_text = message;
    m_message->SetLabel(message);
    m_message->Wrap(info_width());
}

void PartSkipPanel::refresh_view()
{
    const bool ready = m_load_state == LoadState::Ready;

    std::map<uint32_t, PartSkipMap::Object> objects;
    if (ready)
        for (const auto &[id, name] : m_names) {
            PartSkipMap::Object object;
            object.name  = name;
            object.state = m_skipped.count(id) != 0  ? PartSkipMap::ObjectState::Skipped :
                           m_pending.count(id) != 0  ? PartSkipMap::ObjectState::Pending :
                           m_selected.count(id) != 0 ? PartSkipMap::ObjectState::Selected :
                                                       PartSkipMap::ObjectState::Printing;
            objects.emplace(id, object);
        }
    m_map->set_objects(objects);
    m_map->set_clickable(ready && m_can_skip);

    wxString message;
    switch (m_load_state) {
    case LoadState::Loading: message = _L("Loading the plate view from the printer..."); break;
    case LoadState::Failed:
        // No print sent from this computer, and a printer in LAN mode that keeps its files to itself.
        if (!m_can_download)
            message = _L("In LAN mode, the printer does not give access to the plate view. It is shown for the prints sent from this computer. Objects can also be excluded from the printer screen.");
        else
            message = _L("Unable to load the plate view from the printer. Objects can also be excluded from the printer screen.");
        break;
    case LoadState::Ready:
        if (!m_can_skip)
            message = m_cannot_skip_reason;
        else if (!m_notice.empty())
            message = m_notice;
        else if (!m_pending.empty())
            message = _L("Waiting for the printer to confirm the exclusion...");
        else
            message = _L("Click the objects to exclude, then confirm.");
        break;
    default: break;
    }
    bool layout = message != m_message_text;
    set_message(message);

    wxString counter;
    if (ready) {
        size_t skipped = 0;
        for (const auto &[id, name] : m_names)
            if (m_skipped.count(id) != 0)
                ++skipped;
        counter = wxString::Format(_L("Excluded: %d / %d"), int(skipped), int(m_names.size()));
    }
    if (m_counter->GetLabel() != counter) {
        m_counter->SetLabel(counter);
        layout = true;
    }

    for (wxWindow *window : m_legend_windows)
        if (window->IsShown() != ready) {
            window->Show(ready);
            layout = true;
        }

    const bool show_exclude = ready && m_can_skip;
    if (m_exclude_btn->IsShown() != show_exclude) {
        m_exclude_btn->Show(show_exclude);
        layout = true;
    }
    const wxString exclude_label = m_selected.empty() ? _L("Exclude") : wxString::Format(_L("Exclude (%d)"), int(m_selected.size()));
    if (m_exclude_btn->GetLabel() != exclude_label) {
        m_exclude_btn->SetLabel(exclude_label);
        layout = true;
    }
    m_exclude_btn->Enable(!m_selected.empty());

    const bool show_retry = m_load_state == LoadState::Failed && m_can_download;
    if (m_retry_btn->IsShown() != show_retry) {
        m_retry_btn->Show(show_retry);
        layout = true;
    }

    if (layout)
        Layout();
}

void PartSkipPanel::msw_rescale()
{
    SetMinSize(wxSize(FromDIP(BOX_WIDTH_DIP), -1));
    SetMaxSize(wxSize(FromDIP(BOX_WIDTH_DIP), -1));
    m_map->SetMinSize(wxSize(FromDIP(MAP_SIZE_DIP), FromDIP(MAP_SIZE_DIP)));
    m_map->SetMaxSize(wxSize(FromDIP(MAP_SIZE_DIP), FromDIP(MAP_SIZE_DIP)));
    m_map->msw_rescale();
    for (wxWindow *swatch : m_legend_swatches)
        swatch->SetMinSize(wxSize(FromDIP(12), FromDIP(12)));
    m_exclude_btn->SetMinSize(wxSize(FromDIP(110), FromDIP(30)));
    m_exclude_btn->SetCornerRadius(FromDIP(15));
    m_exclude_btn->Rescale();
    m_retry_btn->SetMinSize(wxSize(FromDIP(90), FromDIP(30)));
    m_retry_btn->SetCornerRadius(FromDIP(15));
    m_retry_btn->Rescale();

    // Wrap the message again at the new width.
    const wxString message = m_message_text;
    m_message_text.clear();
    set_message(message);
    Layout();
}

}} // namespace Slic3r::GUI
