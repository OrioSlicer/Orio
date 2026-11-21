#include "GCodeToMesh.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"
#include "libslic3r/Tesselate.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/nowide/fstream.hpp>

#include <fast_float/fast_float.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace Slic3r {

namespace {

// Kinds of extrusions, from the line type comments.
enum class Role : unsigned char { None, OuterWall, OverhangWall, Part, Other };

Role role_of(std::string_view text)
{
    std::string name(text);
    boost::algorithm::trim(name);
    boost::algorithm::to_lower(name);
    // Bambu Studio and Orca Slicer, PrusaSlicer, Cura.
    if (name == "outer wall" || name == "external perimeter" || name == "wall-outer")
        return Role::OuterWall;
    if (name == "overhang wall" || name == "overhang perimeter")
        return Role::OverhangWall;
    // Supports, brim, skirt, prime tower, start and end G-codes: not the object.
    for (const char *other : { "support", "brim", "skirt", "tower", "custom", "flush", "wipe", "prime" })
        if (name.find(other) != std::string::npos)
            return Role::Other;
    return Role::Part;
}

std::string_view trim_left(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    return text;
}

// Removes the prefix if the text starts with it.
bool take(std::string_view &text, std::string_view prefix)
{
    if (text.substr(0, prefix.size()) != prefix)
        return false;
    text.remove_prefix(prefix.size());
    return true;
}

// Number at the start of the text, whatever the locale.
bool parse_number(std::string_view text, double &value)
{
    text = trim_left(text);
    const auto [end, error] = fast_float::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end != text.data();
}

long long z_key(double z) { return std::llround(z * 1000.); }

double distance_to_segment(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d  ab  = b - a;
    const double len = ab.squaredNorm();
    const double t   = len > 0. ? std::clamp((p - a).dot(ab) / len, 0., 1.) : 0.;
    return (p - (a + t * ab)).norm();
}

// A path of outer wall, overhang wall included, printed without interruption.
struct Run
{
    size_t    object{ 0 };
    long long key{ 0 };
    float     z{ 0.f };
    float     height{ 0.f };
    Points    points;
    // Lengths, and lengths times line widths: of the whole path, of its outer wall moves.
    double    length{ 0. };
    double    width_length{ 0. };
    double    outer_length{ 0. };
    double    outer_width_length{ 0. };

    double width() const
    {
        return outer_length > 0. ? outer_width_length / outer_length : length > 0. ? width_length / length : 0.;
    }
};

// Another extrusion of the object, to tell inner walls from outer ones.
struct Stroke
{
    Vec2d     a;
    Vec2d     b;
    float     width;
    long long key;
};

constexpr size_t NO_RUN = std::numeric_limits<size_t>::max();

// Lines of a layer in cells of 1 mm, to find whether a point is covered by one of them.
class LineGrid
{
public:
    void add(const Vec2d &a, const Vec2d &b, float width, size_t run)
    {
        const size_t index = m_lines.size();
        m_lines.push_back({ a, b, width, run });
        // Cells of points every half cell along the line: a line passing at less than a cell from a point (its half
        // width is less than a cell) has one of them among the 9 cells around the point.
        const size_t steps = size_t(std::ceil((b - a).norm() / (0.5 * CELL)));
        for (size_t step = 0; step <= steps; ++step) {
            const Vec2d          p     = steps == 0 ? a : Vec2d(a + (b - a) * (double(step) / double(steps)));
            std::vector<size_t> &lines = m_cells[cell_key(cell_index(p.x()), cell_index(p.y()))];
            if (lines.empty() || lines.back() != index)
                lines.push_back(index);
        }
    }

    // Is the point within the half width of a line which is not of this run?
    bool covered(const Vec2d &p, size_t run) const
    {
        const long long ix = cell_index(p.x());
        const long long iy = cell_index(p.y());
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                const auto cell = m_cells.find(cell_key(ix + dx, iy + dy));
                if (cell == m_cells.end())
                    continue;
                for (size_t index : cell->second) {
                    const Line &line = m_lines[index];
                    if (line.run != run && distance_to_segment(p, line.a, line.b) <= 0.5 * line.width)
                        return true;
                }
            }
        return false;
    }

    // Overhang walls are found on inner walls too, between other lines. An outer one has no material on one of its
    // sides: points beside it, at 0.75 of its width (an inner wall lies at a line spacing), are tested on both sides.
    bool has_free_side(const Run &run, size_t run_index) const
    {
        const double width   = run.width() > 0. ? run.width() : 0.4;
        const double side    = 0.75 * width;
        const double spacing = std::max(run.length / 40., 0.2);
        size_t       samples = 0;
        size_t       left    = 0;
        size_t       right   = 0;
        double       next    = 0.5 * spacing;
        double       done    = 0.;
        for (size_t i = 1; i < run.points.size(); ++i) {
            const Vec2d  a      = unscaled(run.points[i - 1]);
            const Vec2d  b      = unscaled(run.points[i]);
            const double length = (b - a).norm();
            if (length <= 0.)
                continue;
            const Vec2d direction = (b - a) / length;
            const Vec2d normal(-direction.y(), direction.x());
            for (; next <= done + length; next += spacing) {
                const Vec2d p = a + direction * (next - done);
                ++samples;
                left += covered(p + normal * side, run_index) ? 1 : 0;
                right += covered(p - normal * side, run_index) ? 1 : 0;
            }
            done += length;
        }
        return samples == 0 || 2 * std::min(left, right) < samples;
    }

private:
    struct Line
    {
        Vec2d  a;
        Vec2d  b;
        float  width;
        size_t run;
    };

    static constexpr double CELL = 1.;

    static long long cell_index(double x) { return (long long) std::floor(x / CELL); }
    static uint64_t  cell_key(long long ix, long long iy) { return (uint64_t(uint32_t(int32_t(ix))) << 32) | uint32_t(int32_t(iy)); }

    std::vector<Line>                                 m_lines;
    std::unordered_map<uint64_t, std::vector<size_t>> m_cells;
};

// Points of an arc from start to end around center, clockwise (G2) or not (G3), deviating at most 5 µm from it.
std::vector<Vec3d> arc_points(const Vec3d &start, const Vec3d &end, const Vec2d &center, bool ccw)
{
    const Vec2d  from   = start.head<2>() - center;
    const Vec2d  to     = end.head<2>() - center;
    const double radius = from.norm();
    if (radius < 1e-4)
        return { end };
    const double a0    = std::atan2(from.y(), from.x());
    double       sweep = std::atan2(to.y(), to.x()) - a0;
    // Same start and end: a whole turn.
    if (ccw && sweep <= 0.)
        sweep += 2. * M_PI;
    else if (!ccw && sweep >= 0.)
        sweep -= 2. * M_PI;
    const double tolerance = 0.005;
    const double step      = radius > tolerance ? 2. * std::acos(1. - tolerance / radius) : 0.5 * M_PI;
    const size_t n         = std::clamp<size_t>(size_t(std::ceil(std::abs(sweep) / step)), 1, 1000);
    std::vector<Vec3d> points;
    points.reserve(n);
    for (size_t k = 1; k < n; ++k) {
        const double t     = double(k) / double(n);
        const double angle = a0 + sweep * t;
        points.emplace_back(center.x() + radius * std::cos(angle), center.y() + radius * std::sin(angle),
                            start.z() + (end.z() - start.z()) * t);
    }
    points.emplace_back(end);
    return points;
}

// Point of the segment ab closest to p, with its distance (scaled coordinates).
std::pair<double, Point> closest_on_segment(const Point &p, const Point &a, const Point &b)
{
    const Vec2d  ab     = (b - a).cast<double>();
    const double length = ab.squaredNorm();
    const double t      = length > 0. ? std::clamp((p - a).cast<double>().dot(ab) / length, 0., 1.) : 0.;
    const Vec2d  q      = a.cast<double>() + t * ab;
    return { (p.cast<double>() - q).norm(), Point(q.x(), q.y()) };
}

// Loop of a path which comes back to its start, empty if it does not: its ends meet (gap at the seam), or one end runs
// again over the other half of the path (a scarf seam prints the start of the loop again, rising: Orca Slicer, Bambu
// Studio). Scaled length and distances; a path shorter than twice the gap is a dot, not a loop.
Points closed_loop(const Points &points, double length, double gap, double retrace)
{
    if (points.size() < 3 || length <= 2. * gap)
        return {};
    if ((points.back() - points.front()).cast<double>().norm() <= gap)
        return points;
    // The end over the first half of the path: the loop starts where the end lies.
    double done   = 0.;
    double best   = retrace;
    size_t best_k = NO_RUN;
    Point  best_point;
    // Not the last segment, which holds the end itself.
    for (size_t k = 0; k + 2 < points.size() && done <= 0.5 * length; ++k) {
        const std::pair<double, Point> closest = closest_on_segment(points.back(), points[k], points[k + 1]);
        if (closest.first <= best) {
            best       = closest.first;
            best_k     = k;
            best_point = closest.second;
        }
        done += (points[k + 1] - points[k]).cast<double>().norm();
    }
    if (best_k != NO_RUN) {
        Points loop{ best_point };
        loop.insert(loop.end(), points.begin() + best_k + 1, points.end());
        return loop;
    }
    // The start over the last half: the loop ends where the start lies.
    done = 0.;
    best = retrace;
    // Not the first segment, which holds the start itself.
    for (size_t k = points.size() - 1; k > 1 && done <= 0.5 * length; --k) {
        const std::pair<double, Point> closest = closest_on_segment(points.front(), points[k - 1], points[k]);
        if (closest.first <= best) {
            best       = closest.first;
            best_k     = k;
            best_point = closest.second;
        }
        done += (points[k] - points[k - 1]).cast<double>().norm();
    }
    if (best_k != NO_RUN) {
        Points loop(points.begin(), points.begin() + best_k);
        loop.emplace_back(best_point);
        return loop;
    }
    return {};
}

// Closes the paths of a layer into loops: each path alone first, then those left, joined end to end (a path cut in
// two). The others stay open.
void close_runs(std::vector<Run> &runs, GCodeToMesh::Layer &layer)
{
    const double gap     = scale_(2. * layer.width);
    const double retrace = scale_(layer.width);
    auto         add     = [&layer](Points &&points, double width) {
        Polygon loop;
        loop.points = std::move(points);
        layer.loops.emplace_back(std::move(loop));
        layer.loop_widths.emplace_back(float(width > 0. ? width : layer.width));
    };

    std::vector<size_t> pending;
    for (size_t i = 0; i < runs.size(); ++i) {
        Points loop = closed_loop(runs[i].points, scale_(runs[i].length), gap, retrace);
        if (loop.size() >= 3)
            add(std::move(loop), runs[i].width());
        else
            pending.emplace_back(i);
    }
    while (!pending.empty()) {
        Run chain = std::move(runs[pending.back()]);
        pending.pop_back();
        Points loop;
        for (;;) {
            loop = closed_loop(chain.points, scale_(chain.length), gap, retrace);
            if (loop.size() >= 3)
                break;
            size_t best    = NO_RUN;
            bool   reverse = false;
            double best_d2 = gap * gap;
            for (size_t k = 0; k < pending.size(); ++k) {
                const Points &points = runs[pending[k]].points;
                const double  front  = (points.front() - chain.points.back()).cast<double>().squaredNorm();
                const double  back   = (points.back() - chain.points.back()).cast<double>().squaredNorm();
                if (front <= best_d2) {
                    best    = k;
                    reverse = false;
                    best_d2 = front;
                }
                if (back <= best_d2) {
                    best    = k;
                    reverse = true;
                    best_d2 = back;
                }
            }
            if (best == NO_RUN)
                break;
            Run &next = runs[pending[best]];
            if (reverse)
                std::reverse(next.points.begin(), next.points.end());
            chain.points.insert(chain.points.end(), next.points.begin(), next.points.end());
            chain.length += next.length;
            chain.width_length += next.width_length;
            chain.outer_length += next.outer_length;
            chain.outer_width_length += next.outer_width_length;
            pending.erase(pending.begin() + best);
        }
        if (loop.size() >= 3)
            add(std::move(loop), chain.width());
        else {
            layer.open_paths.emplace_back(std::move(chain.points));
            layer.open_widths.emplace_back(float(chain.width() > 0. ? chain.width() : layer.width));
        }
    }
}

// Reads the G-code line by line into the outer walls of each object and layer.
class Reader
{
public:
    explicit Reader(GCodeToMesh &out) : m_out(out) {}

    void line(const GCodeReader::GCodeLine &line)
    {
        const std::string &raw = line.raw();
        if (raw.empty())
            return;
        const std::string_view comment = line.comment();
        if (!comment.empty())
            this->comment(trim_left(comment));
        const std::string_view cmd = line.cmd();
        if (cmd.empty() || cmd.front() == ';')
            return;
        // A picture is only comments: a command closes one left open.
        m_thumbnail_open = false;
        if (cmd == "G0" || cmd == "G1")
            move(line, 0);
        else if (cmd == "G2")
            move(line, 1);
        else if (cmd == "G3")
            move(line, 2);
        else if (cmd == "G90")
            m_relative = false;
        else if (cmd == "G91")
            m_relative = true;
        else if (cmd == "M82")
            m_relative_e = false;
        else if (cmd == "M83")
            m_relative_e = true;
        else if (cmd == "G92") {
            for (int axis = 0; axis < 3; ++axis)
                if (line.has(Axis(axis)))
                    m_position[axis] = line.value(Axis(axis));
            if (line.has(E))
                m_e = line.value(E);
        } else if (cmd == "EXCLUDE_OBJECT_START") {
            // Klipper: EXCLUDE_OBJECT_START NAME=...
            const size_t      name  = raw.find("NAME=");
            const std::string label = name == std::string::npos ? std::string() : raw.substr(name + 5, raw.find_first_of(" \t;", name) - name - 5);
            set_object(label, label);
        } else if (cmd == "EXCLUDE_OBJECT_END")
            set_object(std::string(), std::string());
    }

    void finish()
    {
        end_layer();
        for (size_t i = 0; i < m_labels.size(); ++i) {
            GCodeToMesh::Object object;
            object.label = m_labels[i];
            object.name  = m_names[i];
            for (auto &[key, runs] : m_layers[i]) {
                GCodeToMesh::Layer layer;
                double             length = 0., width_length = 0., outer_length = 0., outer_width_length = 0.;
                for (const Run &run : runs) {
                    layer.z      = std::max(layer.z, run.z);
                    layer.height = std::max(layer.height, run.height);
                    length += run.length;
                    width_length += run.width_length;
                    outer_length += run.outer_length;
                    outer_width_length += run.outer_width_length;
                }
                // Overhang walls may be printed wider: the width of the outer wall moves, if any.
                layer.width = float(outer_length > 0. ? outer_width_length / outer_length : length > 0. ? width_length / length : 0.4);
                if (layer.width <= 0.f)
                    layer.width = 0.4f;
                close_runs(runs, layer);
                object.layers.emplace_back(std::move(layer));
            }
            if (!object.layers.empty())
                m_out.objects.emplace_back(std::move(object));
        }
    }

private:
    void comment(std::string_view text)
    {
        // Pictures of the print written by the slicer: "thumbnail begin 220x124 13336", lines of base64, "thumbnail end"
        // (PrusaSlicer also writes thumbnail_QOI / thumbnail_JPG blocks). The largest PNG or JPG is kept.
        if (m_thumbnail_open) {
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
                text.remove_suffix(1);
            if (boost::algorithm::starts_with(text, "thumbnail") && boost::algorithm::ends_with(text, " end"))
                end_thumbnail();
            else
                m_thumbnail_data.append(text.data(), text.size());
            return;
        }
        if (take(text, "thumbnail begin") || take(text, "thumbnail_PNG begin")) {
            begin_thumbnail("png", text);
            return;
        }
        if (take(text, "thumbnail_JPG begin")) {
            begin_thumbnail("jpg", text);
            return;
        }
        if (take(text, "thumbnail_QOI begin")) {
            begin_thumbnail("qoi", text);
            return;
        }
        // Print time estimated by the slicer. PrusaSlicer, Orca Slicer: "estimated printing time (normal mode) = 6h 13m 2s";
        // Bambu Studio: "model printing time: 5h 3m 1s; total estimated time: 5h 10m 4s".
        if (m_out.print_time.empty()) {
            const size_t total = text.find("total estimated time: ");
            if (take(text, "estimated printing time (normal mode) = ") || total != std::string_view::npos) {
                m_out.print_time = std::string(total != std::string_view::npos ? text.substr(total + 22) : text);
                boost::algorithm::trim(m_out.print_time);
                return;
            }
        }
        double value = 0.;
        if (take(text, "FEATURE:") || take(text, "TYPE:")) {
            m_role = role_of(text);
            return;
        }
        if (take(text, "LINE_WIDTH:") || take(text, "WIDTH:")) {
            if (parse_number(text, value))
                m_width = value;
            return;
        }
        if (take(text, "LAYER_HEIGHT:") || take(text, "HEIGHT:")) {
            if (parse_number(text, value))
                m_height = value;
            return;
        }
        if (take(text, "Z_HEIGHT:") || take(text, "Z:")) {
            end_layer();
            m_layer_tags = true;
            if (parse_number(text, value))
                m_layer_z = value;
            return;
        }
        if (take(text, "CHANGE_LAYER") || take(text, "LAYER_CHANGE") || take(text, "LAYER:")) {
            end_layer();
            m_layer_tags = true;
            return;
        }
        // Bambu Studio, Orca Slicer: "start printing object, unique label id: 144"; PrusaSlicer: "printing object Cube
        // id:0 copy 0", "stop printing object Cube id:0 copy 0". Orca Slicer writes both, the name first.
        if (take(text, "stop printing object")) {
            set_object(std::string(), std::string());
            return;
        }
        if (take(text, "start printing object")) {
            const size_t id = text.rfind(':');
            set_object(std::string(trim_left(id == std::string_view::npos ? text : text.substr(id + 1))), m_object_name);
            return;
        }
        if (take(text, "printing object ")) {
            const std::string label(text);
            set_object(label, label.substr(0, label.find(" id:")));
            return;
        }
        // Settings block: "key = value".
        size_t key_end = 0;
        while (key_end < text.size() && ((text[key_end] >= 'a' && text[key_end] <= 'z') || (text[key_end] >= '0' && text[key_end] <= '9') ||
                                         text[key_end] == '_'))
            ++key_end;
        if (key_end > 0 && text.substr(key_end, 3) == " = ") {
            std::string setting(text.substr(key_end + 3));
            boost::algorithm::trim(setting);
            m_out.settings[std::string(text.substr(0, key_end))] = std::move(setting);
            return;
        }
        // Software which wrote the G-code, in the first comments.
        if (m_out.producer.empty() && m_comments < 50) {
            ++m_comments;
            for (const char *name : { "BambuStudio", "Orio", "Orio ", "OrcaSlicer", "PrusaSlicer", "SuperSlicer", "Cura_SteamEngine", "Slic3r" })
                if (text.find(name) != std::string_view::npos) {
                    std::string producer(text);
                    for (const char *prefix : { "generated by ", "Generated with ", "Generated by " })
                        if (boost::algorithm::starts_with(producer, prefix))
                            producer.erase(0, strlen(prefix));
                    producer = producer.substr(0, producer.find(" on "));
                    boost::algorithm::trim(producer);
                    m_out.producer = producer;
                    break;
                }
        }
    }

    // G0, G1; G2 (arc = 1), G3 (arc = 2).
    void move(const GCodeReader::GCodeLine &line, int arc)
    {
        Vec3d target = m_position;
        for (int axis = 0; axis < 3; ++axis)
            if (line.has(Axis(axis)))
                target[axis] = m_relative ? m_position[axis] + line.value(Axis(axis)) : line.value(Axis(axis));
        double extruded = 0.;
        if (line.has(E)) {
            if (m_relative || m_relative_e) {
                extruded = line.value(E);
                m_e += extruded;
            } else {
                extruded = line.value(E) - m_e;
                m_e      = line.value(E);
            }
        }

        std::vector<Vec3d> points;
        float              radius = 0.f;
        if (arc != 0 && (line.has(I) || line.has(J)))
            points = arc_points(m_position, target, m_position.head<2>() + Vec2d(line.has(I) ? line.value(I) : 0., line.has(J) ? line.value(J) : 0.), arc == 2);
        else if (arc != 0 && line.has_value('R', radius) && target.head<2>() != m_position.head<2>()) {
            // Center on the bisector of the chord, on the left of it for a counter-clockwise arc, on the other side
            // for a negative radius (more than half a turn).
            const Vec2d  chord = target.head<2>() - m_position.head<2>();
            const double d     = chord.norm();
            const double r     = std::max(std::abs(double(radius)), 0.5 * d);
            const double h     = std::sqrt(r * r - 0.25 * d * d) * (arc == 2 ? 1. : -1.) * (radius < 0.f ? -1. : 1.);
            const Vec2d  left(-chord.y() / d, chord.x() / d);
            points = arc_points(m_position, target, m_position.head<2>() + 0.5 * chord + left * h, arc == 2);
        } else
            points.emplace_back(target);

        double length = 0.;
        Vec3d  last   = m_position;
        for (const Vec3d &p : points) {
            length += (p - last).head<2>().norm();
            last = p;
        }
        if (extruded > 0. && length > 0.)
            extrude(points, extruded, length);
        else if (length > 0. || extruded != 0. || line.has(Z))
            end_run();
        m_position = target;
    }

    void extrude(const std::vector<Vec3d> &points, double extruded, double length)
    {
        // Top of the layer: from the layer comments (a scarf seam ramps up below it), otherwise from the nozzle.
        double z = m_layer_z;
        if (z < 0.)
            for (const Vec3d &p : points)
                z = std::max(z, p.z());
        // Line width from the comments, otherwise from the extruded volume: a rectangle with round ends.
        double width = m_width;
        if (width <= 0.) {
            // ponytail: filament diameter and layer height read so far (1.75 mm, 0.2 mm otherwise), enough for slicers
            // which write no width comments.
            const double diameter = m_out.number("filament_diameter") > 0. ? m_out.number("filament_diameter") : 1.75;
            const double height   = m_height > 0. ? m_height : m_out.number("layer_height") > 0. ? m_out.number("layer_height") : 0.2;
            const double area     = extruded * 0.25 * M_PI * diameter * diameter / length;
            width                 = area / height + height * (1. - 0.25 * M_PI);
        }

        if (m_role == Role::OuterWall || m_role == Role::OverhangWall) {
            const size_t object = this->object();
            if (!m_in_run || m_run.object != object) {
                end_run();
                m_run        = Run();
                m_run.object = object;
                m_run.height = float(m_height);
                m_run.points.emplace_back(Point::new_scale(m_position.x(), m_position.y()));
                m_in_run = true;
            }
            m_run.z    = std::max(m_run.z, float(z));
            Vec3d last = m_position;
            for (const Vec3d &p : points) {
                const double l = (p - last).head<2>().norm();
                m_run.length += l;
                m_run.width_length += l * width;
                if (m_role == Role::OuterWall) {
                    m_run.outer_length += l;
                    m_run.outer_width_length += l * width;
                }
                m_run.points.emplace_back(Point::new_scale(p.x(), p.y()));
                last = p;
            }
            return;
        }
        end_run();
        // The other lines of the object tell inner walls from outer ones: kept until the end of the layer, when the
        // G-code says where the layers change.
        if (m_role == Role::Part && m_layer_tags) {
            const long long key  = z_key(z);
            Vec3d           last = m_position;
            for (const Vec3d &p : points) {
                m_strokes.push_back({ last.head<2>(), p.head<2>(), float(width), key });
                last = p;
            }
        }
    }

    void end_run()
    {
        if (m_in_run && m_run.points.size() >= 2) {
            m_run.key = z_key(m_run.z);
            m_runs.emplace_back(std::move(m_run));
        }
        m_run    = Run();
        m_in_run = false;
    }

    // Overhang wall paths without outer wall are left out when they lie between other lines, then the paths of the
    // layer join those of their object and height.
    void end_layer()
    {
        end_run();
        if (!m_strokes.empty()) {
            std::map<long long, LineGrid> grids;
            for (size_t i = 0; i < m_runs.size(); ++i) {
                Run &run = m_runs[i];
                if (run.outer_length > 0.)
                    continue;
                auto [grid, created] = grids.try_emplace(run.key);
                if (created) {
                    for (const Stroke &stroke : m_strokes)
                        if (stroke.key == run.key)
                            grid->second.add(stroke.a, stroke.b, stroke.width, NO_RUN);
                    for (size_t k = 0; k < m_runs.size(); ++k)
                        if (m_runs[k].key == run.key)
                            for (size_t p = 1; p < m_runs[k].points.size(); ++p)
                                grid->second.add(unscaled(m_runs[k].points[p - 1]), unscaled(m_runs[k].points[p]), float(m_runs[k].width()), k);
                }
                if (!grid->second.has_free_side(run, i)) {
                    run.points.clear();
                    ++m_out.overhangs_left_out;
                }
            }
        }
        for (Run &run : m_runs)
            if (run.points.size() >= 2)
                m_layers[run.object][run.key].emplace_back(std::move(run));
        m_runs.clear();
        m_strokes.clear();
    }

    // Label of the object printed from now on, and its name ("" if not written).
    void set_object(std::string label, std::string name)
    {
        end_run();
        m_object      = std::move(label);
        m_object_name = std::move(name);
    }

    // Index of the current object, by its label.
    size_t object()
    {
        const auto [it, created] = m_object_index.try_emplace(m_object, m_labels.size());
        if (created) {
            m_labels.emplace_back(m_object);
            m_names.emplace_back(m_object_name);
            m_layers.emplace_back();
        }
        return it->second;
    }

    // size: " 220x124 13336", width x height then the length of the data.
    void begin_thumbnail(const char *format, std::string_view size)
    {
        size           = trim_left(size);
        int          w = 0;
        int          h = 0;
        const size_t x = size.find('x');
        if (x != std::string_view::npos) {
            std::from_chars(size.data(), size.data() + x, w);
            const std::string_view rest = size.substr(x + 1);
            std::from_chars(rest.data(), rest.data() + rest.size(), h);
        }
        m_thumbnail_open   = true;
        m_thumbnail_format = format;
        m_thumbnail_pixels = (long long) w * h;
        m_thumbnail_data.clear();
    }

    void end_thumbnail()
    {
        m_thumbnail_open = false;
        // QOI pictures cannot be read by the interface.
        if (m_thumbnail_format != "qoi" && !m_thumbnail_data.empty() && m_thumbnail_pixels > m_best_thumbnail) {
            m_best_thumbnail       = m_thumbnail_pixels;
            m_out.thumbnail_format = m_thumbnail_format;
            m_out.thumbnail_base64 = std::move(m_thumbnail_data);
        }
        m_thumbnail_data.clear();
    }

    GCodeToMesh &m_out;

    // Machine state.
    Vec3d  m_position{ Vec3d::Zero() };
    double m_e{ 0. };
    bool   m_relative{ false };
    bool   m_relative_e{ false };

    // Comments in force.
    Role        m_role{ Role::None };
    double      m_width{ 0. };
    double      m_height{ 0. };
    double      m_layer_z{ -1. };
    bool        m_layer_tags{ false };
    std::string m_object;
    std::string m_object_name;
    size_t      m_comments{ 0 };

    // Picture being read, and the size of the largest one kept.
    bool        m_thumbnail_open{ false };
    std::string m_thumbnail_format;
    std::string m_thumbnail_data;
    long long   m_thumbnail_pixels{ 0 };
    long long   m_best_thumbnail{ -1 };

    // Path being printed, those of the layer, the other lines of the layer.
    Run                 m_run;
    bool                m_in_run{ false };
    std::vector<Run>    m_runs;
    std::vector<Stroke> m_strokes;

    // Objects by label; paths of each object by layer.
    std::map<std::string, size_t>                          m_object_index;
    std::vector<std::string>                               m_labels;
    std::vector<std::string>                               m_names;
    std::vector<std::map<long long, std::vector<Run>>>     m_layers;
};

// Undoes the X-Y compensations of the slicer, contour and holes apart, island by island (PrintObject::
// _shrink_contour_holes() with the opposite values). Scaled values.
ExPolygons undo_xy_compensation(const ExPolygons &expolygons, double contour, double hole)
{
    ExPolygons out;
    for (const ExPolygon &expolygon : expolygons) {
        Polygons contours = contour == 0. ? Polygons{ expolygon.contour } : offset(expolygon.contour, float(-contour));
        Polygons holes;
        for (Polygon h : expolygon.holes) {
            h.make_counter_clockwise();
            if (hole == 0.)
                holes.emplace_back(std::move(h));
            else
                polygons_append(holes, offset(h, float(-hole)));
        }
        append(out, diff_ex(union_(contours), union_(holes)));
    }
    return union_ex(out);
}

// slices_to_mesh() leaves T-junctions between the walls of two layers and the horizontal faces between them: a vertex
// of one side lies inside an edge of the other side. These edges are split at such vertices: the mesh closes.
void close_t_junctions(indexed_triangle_set &its)
{
    auto edge_key = [](int a, int b) {
        if (a > b)
            std::swap(a, b);
        return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
    };
    std::unordered_map<uint64_t, int> faces_of_edge;
    faces_of_edge.reserve(its.indices.size() * 2);
    for (const stl_triangle_vertex_indices &face : its.indices)
        for (int k = 0; k < 3; ++k)
            ++faces_of_edge[edge_key(face[k], face[(k + 1) % 3])];
    auto open = [&](int a, int b) { return faces_of_edge[edge_key(a, b)] == 1; };

    // Vertices of the open edges, by height (the T-junctions are on the horizontal edges), sorted along X.
    struct Candidate
    {
        float x;
        int   vertex;
    };
    std::unordered_map<float, std::vector<Candidate>> levels;
    std::vector<char>                                 listed(its.vertices.size(), 0);
    for (const stl_triangle_vertex_indices &face : its.indices)
        for (int k = 0; k < 3; ++k) {
            const int a = face[k];
            const int b = face[(k + 1) % 3];
            if (!open(a, b))
                continue;
            for (int v : { a, b })
                if (!listed[v]) {
                    listed[v] = 1;
                    levels[its.vertices[v].z()].push_back({ its.vertices[v].x(), v });
                }
        }
    if (levels.empty())
        return;
    for (auto &level : levels)
        std::sort(level.second.begin(), level.second.end(), [](const Candidate &l, const Candidate &r) { return l.x < r.x; });

    // ponytail: 0.2 µm, above the float rounding of the coordinates of a print (a few hundredths of a µm at 300 mm).
    const float                              epsilon = 2e-4f;
    std::vector<stl_triangle_vertex_indices> faces;
    faces.reserve(its.indices.size() + its.indices.size() / 4);
    std::vector<std::pair<float, int>> splits[3];
    for (const stl_triangle_vertex_indices &face : its.indices) {
        int split_edges = 0;
        for (int k = 0; k < 3; ++k) {
            splits[k].clear();
            const int    a  = face[k];
            const int    b  = face[(k + 1) % 3];
            const Vec3f &pa = its.vertices[a];
            const Vec3f &pb = its.vertices[b];
            if (pa.z() != pb.z() || !open(a, b))
                continue;
            const auto level = levels.find(pa.z());
            if (level == levels.end())
                continue;
            const Vec2f ab     = (pb - pa).head<2>();
            const float length = ab.squaredNorm();
            if (length <= 0.f)
                continue;
            const float                   x_min      = std::min(pa.x(), pb.x()) - epsilon;
            const float                   x_max      = std::max(pa.x(), pb.x()) + epsilon;
            const std::vector<Candidate> &candidates = level->second;
            for (auto it = std::lower_bound(candidates.begin(), candidates.end(), x_min, [](const Candidate &c, float x) { return c.x < x; });
                 it != candidates.end() && it->x <= x_max; ++it) {
                if (it->vertex == a || it->vertex == b)
                    continue;
                const Vec2f ap = (its.vertices[it->vertex] - pa).head<2>();
                const float t  = ap.dot(ab) / length;
                if (t > 0.f && t < 1.f && (ap - t * ab).squaredNorm() <= epsilon * epsilon)
                    splits[k].emplace_back(t, it->vertex);
            }
            if (!splits[k].empty()) {
                std::sort(splits[k].begin(), splits[k].end());
                ++split_edges;
            }
        }
        if (split_edges == 0) {
            faces.emplace_back(face);
        } else if (split_edges == 1) {
            // Fan from the vertex in front of the split edge.
            const int        k = !splits[0].empty() ? 0 : !splits[1].empty() ? 1 : 2;
            std::vector<int> side{ face[k] };
            for (const auto &split : splits[k])
                side.emplace_back(split.second);
            side.emplace_back(face[(k + 1) % 3]);
            for (size_t i = 0; i + 1 < side.size(); ++i)
                faces.emplace_back(side[i], side[i + 1], face[(k + 2) % 3]);
        } else {
            // Fan from the center of the face, around its vertices and those inserted in its edges.
            std::vector<int> ring;
            for (int k = 0; k < 3; ++k) {
                ring.emplace_back(face[k]);
                for (const auto &split : splits[k])
                    ring.emplace_back(split.second);
            }
            const Vec3f center = (its.vertices[face[0]] + its.vertices[face[1]] + its.vertices[face[2]]) / 3.f;
            const int   middle = int(its.vertices.size());
            its.vertices.emplace_back(center);
            for (size_t i = 0; i < ring.size(); ++i)
                faces.emplace_back(ring[i], ring[(i + 1) % ring.size()], middle);
        }
    }
    its.indices = std::move(faces);
}

// Welds the vertices of the same height closer than epsilon (near duplicates left by the rounding of the boolean
// operations), then drops the faces left without area.
void weld_vertices(indexed_triangle_set &its, float epsilon)
{
    struct Cell
    {
        long long x;
        long long y;
        float     z;
        bool      operator==(const Cell &other) const { return x == other.x && y == other.y && z == other.z; }
    };
    struct CellHash
    {
        size_t operator()(const Cell &cell) const
        {
            return std::hash<long long>()(cell.x * 73856093LL ^ cell.y * 19349663LL) ^ std::hash<float>()(cell.z);
        }
    };
    std::unordered_map<Cell, std::vector<int>, CellHash> cells;
    std::vector<int>                                      remap(its.vertices.size());
    std::vector<Vec3f>                                    kept;
    kept.reserve(its.vertices.size());
    for (size_t i = 0; i < its.vertices.size(); ++i) {
        const Vec3f     p     = its.vertices[i];
        const long long x     = std::llround(p.x() / epsilon);
        const long long y     = std::llround(p.y() / epsilon);
        int             found = -1;
        for (long long dx = -1; dx <= 1 && found < 0; ++dx)
            for (long long dy = -1; dy <= 1 && found < 0; ++dy) {
                const auto cell = cells.find(Cell{ x + dx, y + dy, p.z() });
                if (cell == cells.end())
                    continue;
                for (int j : cell->second)
                    if ((kept[j].head<2>() - p.head<2>()).squaredNorm() <= epsilon * epsilon) {
                        found = j;
                        break;
                    }
            }
        if (found < 0) {
            found = int(kept.size());
            kept.emplace_back(p);
            cells[Cell{ x, y, p.z() }].emplace_back(found);
        }
        remap[i] = found;
    }
    its.vertices = std::move(kept);
    for (stl_triangle_vertex_indices &face : its.indices)
        for (int k = 0; k < 3; ++k)
            face[k] = remap[face[k]];
    its_remove_degenerate_faces(its);
}

// Edges of rings in cells (1 mm by default), to find the edge closest to a point.
class RingEdges
{
public:
    // A point laid onto a ring: at a parameter along it (index of an edge + position on it), onto a vertex or not.
    struct Snap
    {
        size_t ring;
        double param;
        Point  point;
        bool   on_vertex;
    };

    // The tolerance of nearest() must not exceed half a cell.
    explicit RingEdges(const Polygons &rings, double cell = scale_(1.)) : m_cell(cell), m_rings(rings)
    {
        for (size_t r = 0; r < rings.size(); ++r) {
            const Points &points = rings[r].points;
            for (size_t e = 0; e < points.size(); ++e) {
                // Cells of points every half cell along the edge: an edge passing within the tolerance (half a cell
                // at most) of a point has one of them among the 9 cells around the point.
                const Vec2d  a     = points[e].cast<double>();
                const Vec2d  b     = points[(e + 1) % points.size()].cast<double>();
                const size_t steps = size_t(std::ceil((b - a).norm() / (0.5 * m_cell)));
                uint64_t     last  = std::numeric_limits<uint64_t>::max();
                for (size_t step = 0; step <= steps; ++step) {
                    const Vec2d    p   = steps == 0 ? a : Vec2d(a + (b - a) * (double(step) / double(steps)));
                    const uint64_t key = cell_key(cell_index(p.x()), cell_index(p.y()));
                    if (key != last)
                        m_cells[key].emplace_back(r, e);
                    last = key;
                }
            }
        }
    }

    // The point laid onto the closest edge going the same way (material on the same side) within the tolerance,
    // onto one of its ends when it is that close (unless !onto_vertex); none if no edge is that close.
    std::optional<Snap> nearest(const Point &p, const Vec2d &direction, double tolerance, bool onto_vertex = true) const
    {
        size_t best_ring = 0, best_edge = 0;
        double best = tolerance, best_t = 0.;
        bool   found = false;
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy) {
                const auto cell = m_cells.find(cell_key(cell_index(p.x()) + dx, cell_index(p.y()) + dy));
                if (cell == m_cells.end())
                    continue;
                for (const auto &[r, e] : cell->second) {
                    const Points &points = m_rings[r].points;
                    const Vec2d   a      = points[e].cast<double>();
                    const Vec2d   ab     = points[(e + 1) % points.size()].cast<double>() - a;
                    const double  length = ab.squaredNorm();
                    if (length <= 0. || ab.dot(direction) <= 0.)
                        continue;
                    const double t        = std::clamp((p.cast<double>() - a).dot(ab) / length, 0., 1.);
                    const double distance = (p.cast<double>() - (a + t * ab)).norm();
                    if (distance <= best) {
                        best      = distance;
                        best_ring = r;
                        best_edge = e;
                        best_t    = t;
                        found     = true;
                    }
                }
            }
        if (!found)
            return std::nullopt;
        const Points &points = m_rings[best_ring].points;
        const size_t  next   = (best_edge + 1) % points.size();
        if (onto_vertex && (points[best_edge] - p).cast<double>().norm() <= tolerance)
            return Snap{ best_ring, double(best_edge), points[best_edge], true };
        if (onto_vertex && (points[next] - p).cast<double>().norm() <= tolerance)
            return Snap{ best_ring, double(next), points[next], true };
        const Vec2d q = points[best_edge].cast<double>() + best_t * (points[next] - points[best_edge]).cast<double>();
        return Snap{ best_ring, double(best_edge) + best_t, Point(q.x(), q.y()), false };
    }

private:
    long long       cell_index(double x) const { return (long long) std::floor(x / m_cell); }
    static uint64_t cell_key(long long ix, long long iy) { return (uint64_t(uint32_t(int32_t(ix))) << 32) | uint32_t(int32_t(iy)); }

    double                                                                 m_cell;
    const Polygons                                                        &m_rings;
    std::unordered_map<uint64_t, std::vector<std::pair<size_t, size_t>>> m_cells;
};

// Remainder of x / n in [0, n).
double positive_fmod(double x, double n) { return x - n * std::floor(x / n); }

// Vertices of the ring strictly between the parameters pa and pb (points qa and qb), the shorter way round, if this
// way is shorter than max_length; none otherwise.
std::optional<Points> ring_between(const Polygon &ring, double pa, const Point &qa, double pb, const Point &qb, double max_length)
{
    const double n           = double(ring.points.size());
    double       best_length = std::numeric_limits<double>::max();
    Points       best;
    for (bool forward : { true, false }) {
        const double span = positive_fmod(forward ? pb - pa : pa - pb, n);
        Points       between;
        if (span > 0.) {
            long long k = forward ? (long long) std::floor(pa) + 1 : (long long) std::ceil(pa) - 1;
            for (size_t step = 0; step < ring.points.size(); ++step) {
                const double offset = positive_fmod(forward ? double(k) - pa : pa - double(k), n);
                if (offset <= 0. || offset >= span)
                    break;
                between.emplace_back(ring.points[size_t(positive_fmod(double(k), n))]);
                k += forward ? 1 : -1;
            }
        }
        double length = 0.;
        Point  last   = qa;
        for (const Point &p : between) {
            length += (p - last).cast<double>().norm();
            last = p;
        }
        length += (qb - last).cast<double>().norm();
        if (length < best_length) {
            best_length = length;
            best        = std::move(between);
        }
    }
    if (best_length > max_length)
        return std::nullopt;
    return best;
}

// The outline of a layer with its edges laid exactly onto those of the slab below wherever it follows them within the
// tolerance (scaled): each vertex near an edge below going the same way is laid onto it, onto a vertex below when one
// is that close; between two vertices laid onto the same ring, the vertices below are taken. Inside such a stretch the
// vertices of the layer are dropped, at its ends they are kept as they are, so that the edges cross cleanly. The faces
// between the two layers then meet exactly, without the slivers which two nearly equal outlines would leave.
ExPolygons snap_to(const ExPolygons &outline, const ExPolygons &below, double tolerance)
{
    const Polygons  rings_below = to_polygons(below);
    const RingEdges edges(rings_below);
    Polygons        out;
    for (const Polygon &ring : to_polygons(outline)) {
        const size_t n = ring.points.size();
        if (n < 3)
            continue;
        std::vector<std::optional<RingEdges::Snap>> snaps(n);
        for (size_t k = 0; k < n; ++k)
            snaps[k] = edges.nearest(ring.points[k], (ring.points[(k + 1) % n] - ring.points[(k + n - 1) % n]).cast<double>(), tolerance);
        // Vertices below between two neighbours laid onto the same ring, none if they are not linked.
        std::vector<std::optional<Points>> links(n);
        for (size_t k = 0; k < n; ++k) {
            const std::optional<RingEdges::Snap> &a = snaps[k];
            const std::optional<RingEdges::Snap> &b = snaps[(k + 1) % n];
            if (a && b && a->ring == b->ring) {
                const double limit = (ring.points[(k + 1) % n] - ring.points[k]).cast<double>().norm() + 4. * tolerance;
                links[k]           = ring_between(rings_below[a->ring], a->param, a->point, b->param, b->point, limit);
            }
        }
        Polygon snapped;
        for (size_t k = 0; k < n; ++k) {
            const std::optional<RingEdges::Snap> &snap   = snaps[k];
            const bool                            inside = snap && links[(k + n - 1) % n] && links[k];
            if (!snap)
                snapped.points.emplace_back(ring.points[k]);
            else if (snap->on_vertex)
                snapped.points.emplace_back(snap->point);
            else if (!inside)
                snapped.points.emplace_back(ring.points[k]);
            if (links[k])
                append(snapped.points, *links[k]);
        }
        snapped.points.erase(std::unique(snapped.points.begin(), snapped.points.end()), snapped.points.end());
        while (snapped.points.size() > 1 && snapped.points.front() == snapped.points.back())
            snapped.points.pop_back();
        if (snapped.points.size() >= 3)
            out.emplace_back(std::move(snapped));
    }
    return union_ex(out, ClipperLib::pftEvenOdd);
}

// Smoothing between the layers (GCodeToMesh::mesh(..., smooth)).

// Layers with the same outline (within the tolerance) from bottom to top; low and high: mid-heights of its first and
// last layers. An empty outline: no outer wall there.
struct Slab
{
    ExPolygons outline;
    double     bottom;
    double     top;
    double     low;
    double     high;
};

using RingPairs = std::vector<std::pair<size_t, size_t>>;
constexpr size_t NO_RING = std::numeric_limits<size_t>::max();

// Clipper operation with the even-odd fill on both sides: a ring counts once whichever way it goes.
ExPolygons even_odd(ClipperLib::ClipType type, const Polygons &subject, const Polygons &clip)
{
    ClipperLib::Clipper clipper;
    clipper.AddPaths(ClipperUtils::PolygonsProvider(subject), ClipperLib::ptSubject, true);
    clipper.AddPaths(ClipperUtils::PolygonsProvider(clip), ClipperLib::ptClip, true);
    ClipperLib::Paths out;
    clipper.Execute(type, out, ClipperLib::pftEvenOdd, ClipperLib::pftEvenOdd);
    Polygons rings;
    rings.reserve(out.size());
    for (const ClipperLib::Path &path : out)
        rings.emplace_back(path);
    return union_ex(rings, ClipperLib::pftEvenOdd);
}

double area_of(const ExPolygons &region)
{
    double out = 0.;
    for (const ExPolygon &expolygon : region)
        out += expolygon.area();
    return out;
}

// Does the ring pass inside the region?
bool crosses(const Polygon &ring, const Polygons &region)
{
    for (const Polyline &piece : intersection_pl(Polygons{ ring }, region))
        if (piece.length() > SCALED_EPSILON)
            return true;
    return false;
}

// Pairs (i, j) of a ring below and a ring above which follow each other: each vertex of one lies within max_offset
// (scaled) of an edge of the other going the same way, and of no other ring; both ways.
RingPairs match_rings(const Polygons &below, const Polygons &above, double max_offset)
{
    auto partners = [max_offset](const Polygons &from, const Polygons &to) {
        std::vector<size_t> out(from.size(), NO_RING);
        const RingEdges     edges(to, std::max(2. * max_offset, scale_(1.)));
        for (size_t r = 0; r < from.size(); ++r) {
            const Points &points = from[r].points;
            const size_t  n      = points.size();
            size_t        found  = NO_RING;
            for (size_t k = 0; k < n; ++k) {
                const std::optional<RingEdges::Snap> snap = edges.nearest(points[k], (points[(k + 1) % n] - points[(k + n - 1) % n]).cast<double>(), max_offset);
                if (!snap || (found != NO_RING && snap->ring != found)) {
                    found = NO_RING;
                    break;
                }
                found = snap->ring;
            }
            out[r] = found;
        }
        return out;
    };
    RingPairs pairs;
    if (below.empty() || above.empty())
        return pairs;
    const std::vector<size_t> up   = partners(below, above);
    const std::vector<size_t> down = partners(above, below);
    for (size_t i = 0; i < up.size(); ++i)
        if (up[i] != NO_RING && down[up[i]] == i)
            pairs.emplace_back(i, up[i]);
    return pairs;
}

// Rings below and rings above left without a partner.
std::pair<Polygons, Polygons> loose_rings(const Polygons &below, const Polygons &above, const RingPairs &pairs)
{
    std::vector<char> paired_below(below.size(), 0);
    std::vector<char> paired_above(above.size(), 0);
    for (const auto &[i, j] : pairs) {
        paired_below[i] = 1;
        paired_above[j] = 1;
    }
    std::pair<Polygons, Polygons> out;
    for (size_t i = 0; i < below.size(); ++i)
        if (!paired_below[i])
            out.first.emplace_back(below[i]);
    for (size_t j = 0; j < above.size(); ++j)
        if (!paired_above[j])
            out.second.emplace_back(above[j]);
    return out;
}

// Drops the pairs whose band would cross a plateau (the region between the rings left without a partner) or another
// band, until none does: the plateaus are then bounded by the rings left without a partner only, whose walls they meet.
RingPairs consistent_pairs(const Polygons &below, const Polygons &above, RingPairs pairs)
{
    // Overlaps of less than 1e-6 mm² are rounding.
    const double epsilon = 1e-6 / sqr(SCALING_FACTOR);
    while (!pairs.empty()) {
        auto [loose, loose_above] = loose_rings(below, above, pairs);
        append(loose, std::move(loose_above));
        const ExPolygons flat = loose.empty() ? ExPolygons() : union_ex(loose, ClipperLib::pftEvenOdd);
        // 1 µm inside the plateaus: a ring running along their edge does not cross them.
        const Polygons           inner = flat.empty() ? Polygons() : to_polygons(offset_ex(flat, -float(scale_(0.001))));
        std::vector<ExPolygons>  swept;
        std::vector<BoundingBox> boxes;
        for (const auto &[i, j] : pairs) {
            swept.emplace_back(even_odd(ClipperLib::ctXor, Polygons{ below[i] }, Polygons{ above[j] }));
            boxes.emplace_back(get_extents(swept.back()));
        }
        std::vector<char> drop(pairs.size(), 0);
        for (size_t p = 0; p < pairs.size(); ++p) {
            if (!inner.empty() && (crosses(below[pairs[p].first], inner) || crosses(above[pairs[p].second], inner)))
                drop[p] = 1;
            if (swept[p].empty())
                continue;
            if (!flat.empty() && area_of(intersection_ex(swept[p], flat)) > epsilon)
                drop[p] = 1;
            // ponytail: bands compared two by two, a few tens of rings per layer; a grid if parts have thousands.
            for (size_t q = p + 1; q < pairs.size(); ++q)
                if (!swept[q].empty() && boxes[p].overlap(boxes[q]) && area_of(intersection_ex(swept[p], swept[q])) > epsilon)
                    drop[p] = drop[q] = 1;
        }
        if (std::find(drop.begin(), drop.end(), 1) == drop.end())
            break;
        RingPairs kept;
        for (size_t p = 0; p < pairs.size(); ++p)
            if (!drop[p])
                kept.emplace_back(pairs[p]);
        pairs = std::move(kept);
    }
    return pairs;
}

// Horizontal faces at the boundary of two slabs, facing up and down. Out of the bands, a ring with a partner has the
// same parity below and above: it is put on both sides. Each one in a single operation on the rings themselves: a
// second operation would move the rounded crossings of the first one off the edges of the walls.
std::pair<ExPolygons, ExPolygons> plateaus(const Polygons &below, const Polygons &above, const RingPairs &pairs)
{
    auto [loose_below, loose_above] = loose_rings(below, above, pairs);
    if (loose_below.empty() && loose_above.empty())
        return {};
    for (const auto &[i, j] : pairs) {
        loose_below.emplace_back(below[i]);
        loose_above.emplace_back(below[i]);
    }
    return { even_odd(ClipperLib::ctDifference, loose_below, loose_above), even_odd(ClipperLib::ctDifference, loose_above, loose_below) };
}

// Faces between the ring a at za and the ring b at zb, going the same way, every vertex of b within the tolerance
// (scaled) of an edge of a: each vertex of b is placed along a where it projects (closest edge going the same way), so
// that the corners face the corners. Placed by length along each ring instead, the two rings slipped against each
// other where their details differ and the faces cut the corners. The faces join points facing each other.
void band(indexed_triangle_set &its, const Polygon &a, float za, const Polygon &b, float zb, double tolerance)
{
    const size_t n = a.points.size();
    const size_t m = b.points.size();
    // Length along a at each vertex of a, and where each vertex of b projects onto a (-1: nowhere).
    std::vector<double> sa{ 0. };
    for (size_t i = 0; i < n; ++i)
        sa.emplace_back(sa.back() + (a.points[(i + 1) % n] - a.points[i]).cast<double>().norm());
    const double        length = std::max(sa.back(), 1.);
    const Polygons      rings{ a };
    const RingEdges     edges(rings, std::max(2. * tolerance, scale_(1.)));
    std::vector<double> along(m, -1.);
    for (size_t j = 0; j < m; ++j) {
        const std::optional<RingEdges::Snap> snap = edges.nearest(b.points[j], (b.points[(j + 1) % m] - b.points[(j + m - 1) % m]).cast<double>(),
                                                                  tolerance, false);
        if (snap) {
            const size_t e = std::min(size_t(snap->param), n - 1);
            along[j]       = sa[e] + (snap->param - double(e)) * (sa[e + 1] - sa[e]);
        }
    }
    // Start of b: the vertex which projects closest to the start of a (the closest vertex if none projects).
    auto from_start = [&along, length](size_t j) {
        return along[j] < 0. ? std::numeric_limits<double>::max() : std::min(along[j], length - along[j]);
    };
    size_t start = 0;
    for (size_t j = 1; j < m; ++j)
        if (from_start(j) < from_start(start))
            start = j;
    if (along[start] < 0.)
        for (size_t j = 1; j < m; ++j)
            if ((b.points[j] - a.points.front()).cast<double>().squaredNorm() < (b.points[start] - a.points.front()).cast<double>().squaredNorm())
                start = j;
    // Places of the vertices of b along a, on one turn from the start: never going back, never beyond the turn (a
    // vertex at a corner which projects just after the start would otherwise come before it).
    const double        first = along[start] < 0. ? 0. : along[start] > 0.5 * length ? along[start] - length : along[start];
    std::vector<double> sb{ first };
    sb.reserve(m);
    for (size_t k = 1; k < m; ++k) {
        double place = along[(start + k) % m];
        if (place < 0.)
            place = sb.back();
        else
            place += length * std::round((sb.back() - place) / length);
        sb.emplace_back(std::clamp(place, sb.back(), first + length));
    }

    // Points facing each other: every vertex of a and every vertex of b, in their order along a over that turn. Under a
    // vertex of b a point is laid onto a, over a vertex of a a point onto b (between the vertices of b around it): the
    // band leans only where the rings differ. Joined vertex to vertex instead, a small detail of b in the middle of a
    // long edge of a stretched faces from one end of the edge to the other, leaning all along it (lines across a face).
    struct Event
    {
        double place;
        int    a;
        int    b;
    };
    std::vector<Event> events;
    events.reserve(n + m);
    for (size_t k = 0; k < m; ++k)
        events.push_back({ sb[k], -1, int((start + k) % m) });
    for (size_t i = 0; i < n; ++i)
        events.push_back({ first + positive_fmod(sa[i] - first, length), int(i), -1 });
    // Stable: at the same place, b before a, b in its order.
    std::stable_sort(events.begin(), events.end(), [](const Event &l, const Event &r) { return l.place < r.place; });
    std::vector<Event> merged;
    merged.reserve(events.size());
    for (const Event &event : events) {
        // A vertex of a and a vertex of b at the same place (vertices shared by snap_to()): one pair.
        if (!merged.empty()) {
            Event &last = merged.back();
            if (event.place - last.place <= 1. && (event.a >= 0) != (last.a >= 0) && (last.a < 0 || last.b < 0)) {
                if (event.a >= 0)
                    last.a = event.a;
                else
                    last.b = event.b;
                continue;
            }
        }
        merged.emplace_back(event);
    }
    std::vector<size_t> with_b;
    for (size_t k = 0; k < merged.size(); ++k)
        if (merged[k].b >= 0)
            with_b.emplace_back(k);
    // Point of a at a place, point of b between the vertices of b around an event.
    auto on_a = [&a, &sa, n, length](double place) {
        const double at   = positive_fmod(place, length);
        const size_t e    = std::min(size_t(std::upper_bound(sa.begin(), sa.end(), at) - sa.begin()), n) - 1;
        const double span = sa[e + 1] - sa[e];
        const double f    = span > 0. ? std::clamp((at - sa[e]) / span, 0., 1.) : 0.;
        return Vec2d(a.points[e].cast<double>() + f * (a.points[(e + 1) % n] - a.points[e]).cast<double>());
    };
    auto on_b = [&b, &merged, &with_b, length](size_t k) {
        const size_t idx    = size_t(std::lower_bound(with_b.begin(), with_b.end(), k) - with_b.begin());
        const size_t before = idx > 0 ? with_b[idx - 1] : with_b.back();
        const size_t after  = idx < with_b.size() ? with_b[idx] : with_b.front();
        const double from   = merged[before].place - (idx > 0 ? 0. : length);
        const double to     = merged[after].place + (idx < with_b.size() ? 0. : length);
        const double f      = to > from ? std::clamp((merged[k].place - from) / (to - from), 0., 1.) : 0.;
        const Point &p      = b.points[merged[before].b];
        const Point &q      = b.points[merged[after].b];
        return Vec2d(p.cast<double>() + f * (q - p).cast<double>());
    };
    // Same conversion as the walls and the tesselation (unscaled(), then float): shared vertices are equal.
    auto vertex = [](const Vec2d &p, float z) { return Vec3f(float(p.x() * SCALING_FACTOR), float(p.y() * SCALING_FACTOR), z); };
    const int base = int(its.vertices.size());
    for (size_t k = 0; k < merged.size(); ++k) {
        const Event &event = merged[k];
        its.vertices.emplace_back(vertex(event.a >= 0 ? Vec2d(a.points[event.a].cast<double>()) : on_a(event.place), za));
        its.vertices.emplace_back(vertex(event.b >= 0 ? Vec2d(b.points[event.b].cast<double>()) : on_b(k), zb));
    }
    // Two faces between two neighbouring events; the points laid inside the edges of the walls are sewn by
    // close_t_junctions().
    for (size_t k = 0; k < merged.size(); ++k) {
        const int bottom      = base + 2 * int(k);
        const int bottom_next = base + 2 * int((k + 1) % merged.size());
        its.indices.emplace_back(bottom, bottom_next, bottom + 1);
        its.indices.emplace_back(bottom_next, bottom_next + 1, bottom + 1);
    }
}

// Vertical faces along the ring from lo to hi.
void wall(indexed_triangle_set &its, const Polygon &ring, float lo, float hi)
{
    if (!(hi > lo))
        return;
    const int n    = int(ring.points.size());
    const int base = int(its.vertices.size());
    for (const Point &p : ring.points)
        its.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), lo));
    for (const Point &p : ring.points)
        its.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), hi));
    for (int i = 0; i < n; ++i) {
        const int next = (i + 1) % n;
        its.indices.emplace_back(base + i, base + next, base + n + i);
        its.indices.emplace_back(base + next, base + n + next, base + n + i);
    }
}

void cap(indexed_triangle_set &its, const ExPolygons &region, float z, bool up)
{
    if (!region.empty())
        its_merge(its, triangulate_expolygons_3d(region, z, up ? NORMALS_UP : NORMALS_DOWN));
}

// A ring at the mid-height of the last layer of its slab, carried on along its slope to the top of that layer where it
// stops (top of the object, plateau): it went straight up there and stood out of a chamfer by half a layer of slope (a
// lip of 0.14 mm on a chamfer of 45° with layers of 0.28 mm). Each edge goes on moving from the ring below joined to
// it by a band (the distance from its middle to that ring, along its normal), ratio times as much, parallel to itself;
// each vertex where its two edges cross (a corner projected onto a single edge below would move one way only). Nothing
// when nothing moves, when the ring turns over or when it really crosses itself.
std::optional<Polygon> extended(const Polygon &ring, const Polygon &partner, double ratio, double max_shift)
{
    const size_t n = ring.points.size();
    if (n < 3)
        return {};
    // The edges of the ring below going the same way (material on the same side) as each edge here.
    const Polygons      partners{ partner };
    const RingEdges     edges(partners, std::max(2. * max_shift, scale_(1.)));
    std::vector<Vec2d>  normal(n, Vec2d::Zero());
    std::vector<double> shift(n, 0.);
    for (size_t e = 0; e < n; ++e) {
        const Vec2d  a      = ring.points[e].cast<double>();
        const Vec2d  ab     = ring.points[(e + 1) % n].cast<double>() - a;
        const double length = ab.norm();
        if (length <= 0.)
            continue;
        const Vec2d t(ab.x() / length, ab.y() / length);
        normal[e]       = Vec2d(-t.y(), t.x());
        const Vec2d mid = a + 0.5 * ab;
        if (const std::optional<RingEdges::Snap> q = edges.nearest(Point(coord_t(std::round(mid.x())), coord_t(std::round(mid.y()))), t, max_shift, false))
            // From below to here the edge moved by minus that distance along its normal: ratio times as much again.
            shift[e] = -ratio * (q->point.cast<double>() - mid).dot(normal[e]);
    }
    // Two edges at less than 10 degrees: along their mean normal.
    const double parallel = std::sin(10. * PI / 180.);
    Polygon      out;
    out.points.reserve(n);
    bool moved = false;
    for (size_t k = 0; k < n; ++k) {
        const size_t prev = (k + n - 1) % n;
        const Vec2d &n1 = normal[prev], &n2 = normal[k];
        const double o1 = shift[prev], o2 = shift[k];
        const double det  = n1.x() * n2.y() - n1.y() * n2.x();
        Vec2d        move = Vec2d::Zero();
        if (std::abs(det) >= parallel)
            move = Vec2d((o1 * n2.y() - o2 * n1.y()) / det, (n1.x() * o2 - n2.x() * o1) / det);
        else if (const Vec2d mean = n1 + n2; mean.norm() > 0.)
            move = mean.normalized() * (0.5 * (o1 + o2));
        const Point p = ring.points[k];
        const Point c(coord_t(std::round(double(p.x()) + move.x())), coord_t(std::round(double(p.y()) + move.y())));
        moved = moved || c != p;
        out.points.emplace_back(c);
    }
    if (!moved)
        return {};
    const double area = ring.area();
    if (area * out.area() <= 0.)
        return {};
    // At a corner a short chamfer which moves less than the sides makes a minute loop: the main loop is kept when the
    // rest is negligible; otherwise (the ring really crosses itself) nothing.
    Polygons simple = simplify_polygons(Polygons{ out });
    if (simple.empty())
        return {};
    size_t largest = 0;
    double total   = 0.;
    for (size_t i = 0; i < simple.size(); ++i) {
        total += std::abs(simple[i].area());
        if (std::abs(simple[i].area()) > std::abs(simple[largest].area()))
            largest = i;
    }
    if (total - std::abs(simple[largest].area()) > 1e-3 * std::abs(simple[largest].area()))
        return {};
    if (simple[largest].area() * area < 0.)
        simple[largest].reverse();
    return std::move(simple[largest]);
}

// Rings, some carried on, which still nest without crossing each other: their even-odd area is their signed area (a
// contour and a hole, or two islands, carried towards each other across a narrow flat would cross).
bool nested(const Polygons &rings)
{
    double signed_area = 0.;
    for (const Polygon &ring : rings)
        signed_area += ring.area();
    return std::abs(area_of(union_ex(rings, ClipperLib::pftEvenOdd)) - std::abs(signed_area)) <= 1e-6 / sqr(SCALING_FACTOR);
}

// The outline of a layer is the section of the object at mid-height of the layer, where the line is widest. Between two
// slabs, the rings which follow each other are joined by a band from one mid-height to the other; the others keep a
// step: a vertical wall up to the boundary of the layers, and a plateau there. A ring which stops (top of the object,
// plateau) right after a band goes on along the slope of that band up to the top of its layer (extended()).
indexed_triangle_set smooth_mesh(const std::vector<Slab> &slabs, const std::function<void()> &throw_on_cancel)
{
    // ponytail: slopes flatter than 20° keep their steps (rings further apart than this slope allows are not paired);
    // a setting if finer control is wanted.
    const double flattest = std::tan(20. * PI / 180.);
    // The rings of each slab, and the pairs at the top of each one (with the slab above): a slab needs those of the slab
    // below to carry a ring on along its slope.
    std::vector<Polygons>  rings_of(slabs.size());
    std::vector<RingPairs> pairs_of(slabs.size());
    tbb::parallel_for(size_t(0), slabs.size(), [&slabs, &rings_of](size_t k) { rings_of[k] = to_polygons(slabs[k].outline); });
    tbb::parallel_for(size_t(0), slabs.size(), [&slabs, &rings_of, &pairs_of, flattest, &throw_on_cancel](size_t k) {
        throw_on_cancel();
        if (k + 1 >= slabs.size() || slabs[k].outline.empty() || slabs[k + 1].outline.empty())
            return;
        // Largest offset between two rings joined by a band: the flattest slope over the height between them.
        const double max_offset = scale_((slabs[k + 1].low - slabs[k].high) / flattest);
        if (max_offset > 0.)
            pairs_of[k] = consistent_pairs(rings_of[k], rings_of[k + 1], match_rings(rings_of[k], rings_of[k + 1], max_offset));
    });
    // Ring j of slab k carried on to the top of the slab: a slab of one layer (its outline has just changed), the ring
    // joined from below by a band, at most twice as far up as that band.
    auto extension = [&slabs, &rings_of, &pairs_of, flattest](size_t k, size_t j) -> std::optional<Polygon> {
        const Slab &slab = slabs[k];
        if (k == 0 || slab.high > slab.low || slabs[k - 1].outline.empty())
            return {};
        const auto from = std::find_if(pairs_of[k - 1].begin(), pairs_of[k - 1].end(), [j](const std::pair<size_t, size_t> &p) { return p.second == j; });
        if (from == pairs_of[k - 1].end())
            return {};
        const double rise = slab.high - slabs[k - 1].high;
        if (!(rise > 0.) || !(slab.top > slab.high) || slab.top - slab.high > 2. * rise)
            return {};
        return extended(rings_of[k][j], rings_of[k - 1][from->first], (slab.top - slab.high) / rise, scale_(rise / flattest));
    };
    std::vector<indexed_triangle_set> parts(slabs.size());
    tbb::parallel_for(size_t(0), slabs.size(), [&slabs, &rings_of, &pairs_of, &parts, &extension, flattest, &throw_on_cancel](size_t k) {
        throw_on_cancel();
        const Slab &slab = slabs[k];
        if (slab.outline.empty())
            return;
        indexed_triangle_set &its   = parts[k];
        const bool            below = k > 0 && !slabs[k - 1].outline.empty();
        const bool            above = k + 1 < slabs.size() && !slabs[k + 1].outline.empty();
        const Polygons       &rings = rings_of[k];
        // How far the vertices of a ring carried on may be from it, for its band.
        const double          reach = below ? scale_(std::max(slab.top - slab.high, slab.high - slabs[k - 1].high) / flattest) : 0.;
        if (!below)
            cap(its, slab.outline, float(slab.bottom), false);
        if (!above) {
            // The top: each ring up to the top of the slab, along its slope when it can be carried on (none if the rings
            // carried on would cross each other).
            std::vector<std::optional<Polygon>> ends(rings.size());
            Polygons                            top     = rings;
            bool                                carried = false;
            for (size_t j = 0; j < rings.size(); ++j)
                if ((ends[j] = extension(k, j))) {
                    top[j]  = *ends[j];
                    carried = true;
                }
            if (carried && !nested(top)) {
                ends.assign(rings.size(), std::nullopt);
                carried = false;
            }
            for (size_t j = 0; j < rings.size(); ++j) {
                wall(its, rings[j], float(below ? slab.low : slab.bottom), float(ends[j] ? slab.high : slab.top));
                if (ends[j])
                    band(its, rings[j], float(slab.high), *ends[j], float(slab.top), reach);
            }
            cap(its, carried ? union_ex(top, ClipperLib::pftEvenOdd) : slab.outline, float(slab.top), true);
            return;
        }
        for (const Polygon &ring : rings)
            wall(its, ring, float(below ? slab.low : slab.bottom), float(slab.high));
        const Slab      &upper       = slabs[k + 1];
        const Polygons  &rings_above = rings_of[k + 1];
        const float      za          = float(slab.high);
        const float      zb          = float(upper.low);
        const float      zm          = float(slab.top);
        const double     max_offset  = scale_((upper.low - slab.high) / flattest);
        const RingPairs &pairs       = pairs_of[k];
        for (const auto &[i, j] : pairs)
            band(its, rings[i], za, rings_above[j], zb, max_offset);
        // The rings below which stop here, under a plateau: carried on along their slope when they can be and stay out of
        // the slab above (1 µm inside it, as in consistent_pairs(): a ring running along its edge does not cross it) and
        // of each other; the plateau then starts from them.
        std::vector<char> paired(rings.size(), 0);
        for (const auto &[i, j] : pairs)
            paired[i] = 1;
        const Polygons                      inner_above = to_polygons(offset_ex(upper.outline, -float(scale_(0.001))));
        std::vector<std::optional<Polygon>> carried(rings.size());
        Polygons                            ends = rings;
        bool                                any  = false;
        for (size_t i = 0; i < rings.size(); ++i) {
            if (paired[i])
                continue;
            carried[i] = extension(k, i);
            if (carried[i] && crosses(*carried[i], inner_above))
                carried[i].reset();
            if (carried[i]) {
                ends[i] = *carried[i];
                any     = true;
            }
        }
        if (any && !nested(ends)) {
            carried.assign(rings.size(), std::nullopt);
            ends = rings;
        }
        for (size_t i = 0; i < rings.size(); ++i)
            if (carried[i])
                band(its, rings[i], za, *carried[i], zm, reach);
            else if (!paired[i])
                wall(its, rings[i], za, zm);
        const auto loose = loose_rings(rings, rings_above, pairs);
        for (const Polygon &ring : loose.second)
            wall(its, ring, zm, zb);
        const auto [up, down] = plateaus(ends, rings_above, pairs);
        cap(its, up, zm, true);
        cap(its, down, zm, false);
    });
    indexed_triangle_set its;
    for (const indexed_triangle_set &part : parts)
        its_merge(its, part);
    return its;
}

} // namespace

void GCodeToMesh::load(const std::string &path)
{
    *this = GCodeToMesh();
    if (!boost::nowide::ifstream(path).good())
        throw Slic3r::RuntimeError("The G-code file cannot be read: " + path);
    CNumericLocalesSetter locales_setter;
    Reader                reader(*this);
    GCodeReader           parser;
    if (!parser.parse_file(path, [&reader](GCodeReader &, const GCodeReader::GCodeLine &line) { reader.line(line); }))
        throw Slic3r::RuntimeError("The G-code file cannot be read: " + path);
    reader.finish();
}

std::string GCodeToMesh::setting(const std::string &key) const
{
    const auto it = settings.find(key);
    return it == settings.end() ? std::string() : it->second;
}

double GCodeToMesh::number(const std::string &key) const
{
    double value = 0.;
    return parse_number(setting(key), value) ? value : 0.;
}

GCodeToMesh::Compensation GCodeToMesh::compensation() const
{
    Compensation compensation;
    // PrusaSlicer: xy_size_compensation.
    compensation.contour       = settings.count("xy_contour_compensation") ? number("xy_contour_compensation") : number("xy_size_compensation");
    compensation.hole          = number("xy_hole_compensation");
    compensation.elephant_foot = number("elefant_foot_compensation");
    return compensation;
}

GCodeToMesh::Stats GCodeToMesh::stats() const
{
    Stats               stats;
    std::set<long long> tops;
    for (const Object &object : objects)
        for (size_t i = 0; i < object.layers.size(); ++i) {
            const Layer &layer = object.layers[i];
            tops.insert(z_key(layer.z));
            stats.height = std::max(stats.height, layer.z);
            stats.loops += layer.loops.size();
            stats.open_paths += layer.open_paths.size();
            for (const std::vector<float> *widths : { &layer.loop_widths, &layer.open_widths })
                for (float width : *widths) {
                    stats.width_min = stats.width_min == 0.f ? width : std::min(stats.width_min, width);
                    stats.width_max = std::max(stats.width_max, width);
                }
            const float height = layer.height > 0.f ? layer.height : i == 0 ? layer.z : layer.z - object.layers[i - 1].z;
            if (i == 0)
                stats.first_layer_height = std::max(stats.first_layer_height, height);
            else {
                stats.layer_height_min = stats.layer_height_min == 0.f ? height : std::min(stats.layer_height_min, height);
                stats.layer_height_max = std::max(stats.layer_height_max, height);
            }
        }
    stats.layers = tops.size();
    return stats;
}

std::vector<ExPolygons> GCodeToMesh::slices(const Object &object, const Compensation &undo, const std::function<void()> &throw_on_cancel) const
{
    // The slicer shrinks the first layer only when the object is on the bed.
    const bool              on_raft = number("raft_layers") > 0.;
    std::vector<ExPolygons> out(object.layers.size());
    // Each layer apart, in parallel.
    tbb::parallel_for(size_t(0), object.layers.size(), [&object, &undo, on_raft, &out, &throw_on_cancel](size_t i) {
        throw_on_cancel();
        const Layer &layer = object.layers[i];
        // The outer wall lies half its width inside the outline: each loop is offset by half its own width, outwards
        // for a contour, into the hole for a hole. A loop is a hole when an odd number of loops surround it, whichever
        // way it was printed; even-odd fill.
        std::vector<BoundingBox> boxes;
        for (const Polygon &loop : layer.loops)
            boxes.emplace_back(get_extents(loop));
        Polygons rings;
        for (size_t k = 0; k < layer.loops.size(); ++k) {
            const Point &point = layer.loops[k].points.front();
            size_t       depth = 0;
            for (size_t j = 0; j < layer.loops.size(); ++j)
                if (j != k && boxes[j].contains(point) && layer.loops[j].contains(point))
                    ++depth;
            Polygon ring = layer.loops[k];
            ring.make_counter_clockwise();
            const float half = float(scale_(0.5 * layer.loop_widths[k]));
            polygons_append(rings, offset(ring, depth % 2 == 0 ? half : -half));
        }
        ExPolygons outline = union_ex(rings, ClipperLib::pftEvenOdd);
        if (!layer.open_paths.empty()) {
            Polygons lines;
            // Square ends, closer to the round ends of a line than butt ends.
            for (size_t k = 0; k < layer.open_paths.size(); ++k)
                polygons_append(lines, offset(layer.open_paths[k], float(scale_(0.5 * layer.open_widths[k])), ClipperLib::jtSquare,
                                              DefaultLineMiterLimit, ClipperLib::etOpenSquare));
            outline = union_ex(outline, lines);
        }
        // Undone in the reverse order of the slicer: elephant foot, then X-Y.
        if (i == 0 && undo.elephant_foot > 0. && !on_raft)
            outline = offset_ex(outline, float(scale_(undo.elephant_foot)));
        if (undo.contour != 0. || undo.hole != 0.)
            outline = undo_xy_compensation(outline, scale_(undo.contour), scale_(undo.hole));
        // Points closer than 5 µm to the line of their neighbours removed (arcs of the G-code, straight walls), then
        // the specks left without area and the crossings.
        for (ExPolygon &expolygon : outline)
            expolygon.douglas_peucker(scale_(0.005));
        out[i] = union_ex(outline);
    });
    return out;
}

indexed_triangle_set GCodeToMesh::mesh(const Object &object, const Compensation &undo, bool smooth, const std::function<void()> &throw_on_cancel) const
{
    if (object.layers.empty())
        return {};
    const std::vector<ExPolygons> outlines = slices(object, undo, throw_on_cancel);
    // Height of a layer: from the G-code, otherwise from the settings, otherwise down to the layer below.
    const double default_height = number("layer_height");
    auto         height_of      = [default_height](const Layer &layer) { return double(layer.height > 0.f ? layer.height : default_height); };

    // Layers which differ by less than 0.05 mm make one slab: the noise of the G-code (seams, arcs, rounding) leaves no
    // slivers between them, a straight wall is one face. A layer which differs more is laid onto the slab below
    // wherever it follows it within this tolerance (snap_to()), so that the faces between them meet exactly.
    // ponytail: 0.025 mm on each side, below the precision of a printer; a setting if finer steps are wanted.
    const float  tolerance = float(scale_(0.025));
    auto         thick     = [tolerance](const ExPolygons &region) {
        // Parts thicker than twice the tolerance (opening), with the exact edges of the region.
        ExPolygons opened = offset_ex(offset_ex(region, -tolerance), tolerance);
        if (!opened.empty())
            opened = intersection_ex(opened, region);
        return opened;
    };
    std::vector<Slab> slabs;
    const Layer      &first = object.layers.front();
    const double      zmin  = std::max(0., double(first.z) - (height_of(first) > 0. ? height_of(first) : double(first.z)));
    for (size_t i = 0; i < object.layers.size(); ++i) {
        throw_on_cancel();
        const Layer &layer = object.layers[i];
        // No outer wall of the object between two layers (a part starting higher, on supports): an empty slab.
        if (i > 0 && height_of(layer) > 0. && layer.z - height_of(layer) > slabs.back().top + 0.01)
            slabs.push_back({ ExPolygons(), slabs.back().top, layer.z - height_of(layer), 0., 0. });
        // Middle of the layer, between its top and the top of the layer below.
        const double bottom = slabs.empty() ? zmin : slabs.back().top;
        const double middle = 0.5 * (layer.z + (height_of(layer) > 0. ? std::max(bottom, layer.z - height_of(layer)) : bottom));
        if (slabs.empty() || slabs.back().outline.empty() || outlines[i].empty()) {
            slabs.push_back({ outlines[i], bottom, layer.z, middle, middle });
            continue;
        }
        const ExPolygons removed = thick(diff_ex(slabs.back().outline, outlines[i]));
        const ExPolygons added   = thick(diff_ex(outlines[i], slabs.back().outline));
        if (removed.empty() && added.empty()) {
            // The slab goes on up.
            slabs.back().top  = layer.z;
            slabs.back().high = middle;
            continue;
        }
        ExPolygons next = snap_to(outlines[i], slabs.back().outline, tolerance);
        slabs.push_back({ std::move(next), bottom, layer.z, middle, middle });
    }
    indexed_triangle_set its;
    if (smooth)
        its = smooth_mesh(slabs, throw_on_cancel);
    else {
        std::vector<ExPolygons> sections;
        std::vector<float>      tops;
        for (Slab &slab : slabs) {
            sections.emplace_back(std::move(slab.outline));
            tops.emplace_back(float(slab.top));
        }
        its = slices_to_mesh(sections, zmin, tops);
    }
    // Near duplicate vertices first (0.2 µm, the tolerance of close_t_junctions()), then the T-junctions.
    throw_on_cancel();
    weld_vertices(its, 2e-4f);
    close_t_junctions(its);
    return its;
}

} // namespace Slic3r
