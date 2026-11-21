#include "GCodeSketch.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GCodeSketch {

namespace {

// Points closer than this are merged.
constexpr double MIN_LENGTH   = 1e-3;
// Points closer than this in Z are at the same height.
constexpr double Z_EPSILON    = 1e-4;
// Arcs of a larger radius are straight lines.
constexpr double MAX_RADIUS   = 1e5;
// mm/s, lifting and lowering the nozzle.
constexpr double Z_SPEED      = 10.;
// Moves read from a G-code at most.
constexpr size_t MAX_IMPORTED = 100000;
// Arcs of a curve (ellipse, spline, conic) at most.
constexpr size_t MAX_CURVE_ARCS = 2048;

constexpr const char *DRAWING_BEGIN = "; ORIO_GCODE_DRAWING_BEGIN";
constexpr const char *DRAWING_END   = "; ORIO_GCODE_DRAWING_END";

bool inside_polygon(const Pointfs &polygon, const Vec2d &p)
{
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i ++) {
        const Vec2d &a = polygon[i];
        const Vec2d &b = polygon[j];
        if ((a.y() > p.y()) != (b.y() > p.y()) && p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x())
            inside = !inside;
    }
    return inside;
}

double distance_to_outline(const Pointfs &polygon, const Vec2d &p)
{
    double distance = std::numeric_limits<double>::max();
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i ++) {
        const Vec2d  a  = polygon[j];
        const Vec2d  ab = polygon[i] - a;
        const double l2 = ab.squaredNorm();
        const double t  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
        distance = std::min(distance, (a + t * ab - p).norm());
    }
    return distance;
}

// Number with a dot whatever the locale, without trailing zeros.
std::string num(double value, int decimals)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::fixed << std::setprecision(decimals) << value;
    std::string s = ss.str();
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (s == "-0")
        s = "0";
    return s;
}

double round_to(double value, int decimals)
{
    const double factor = std::pow(10., decimals);
    return std::round(value * factor) / factor;
}

// mm/min of a speed in mm/s.
double feedrate(double speed) { return std::round(std::max(speed, 1.) * 60.); }

// Part of v in the plane of the unit normal n.
Vec3d in_plane(const Vec3d &v, const Vec3d &n) { return v - v.dot(n) * n; }

// Two unit axes of the plane of the unit normal n.
void plane_axes(const Vec3d &n, Vec3d &e1, Vec3d &e2)
{
    // From the axis the least along the normal.
    const Vec3d a = std::abs(n.x()) < 0.9 ? Vec3d(Vec3d::UnitX()) : Vec3d(Vec3d::UnitY());
    e1 = in_plane(a, n).normalized();
    e2 = n.cross(e1);
}

// p turned by angle around the unit axis n through center.
Vec3d turn(const Vec3d &center, const Vec3d &p, double angle, const Vec3d &n)
{
    const Vec3d  r = p - center;
    const double c = std::cos(angle);
    return center + c * r + std::sin(angle) * n.cross(r) + (1. - c) * r.dot(n) * n;
}

Segment line_to(const Vec3d &end)
{
    Segment segment;
    segment.end = end;
    return segment;
}

// Arc around center from start by angle, counterclockwise around n when positive: one piece up to three quarters of
// turn, two beyond, so that the 3 points of each piece keep its direction.
void add_arc_around(std::vector<Segment> &segments, const Vec3d &center, const Vec3d &start, double angle, const Vec3d &n)
{
    const int pieces = std::abs(angle) > 1.5 * PI ? 2 : 1;
    Vec3d     from   = start;
    for (int k = 0; k < pieces; ++ k) {
        Segment segment;
        segment.type = SegmentType::Arc;
        segment.mid  = turn(center, start, angle * (k + 0.5) / pieces, n);
        segment.end  = turn(center, start, angle * double(k + 1) / pieces, n);
        // Too short to be told from a straight line: one.
        if (!arc_through(from, segment.mid, segment.end).valid)
            segment.type = SegmentType::Line;
        from = segment.end;
        segments.push_back(segment);
    }
}

double distance_to_segment(const Vec3d &p, const Vec3d &a, const Vec3d &b)
{
    const Vec3d  ab = b - a;
    const double l2 = ab.squaredNorm();
    const double t  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
    return (a + t * ab - p).norm();
}

// Distance from p to the circle of an arc.
double distance_to_circle(const Arc &arc, const Vec3d &p)
{
    const Vec3d  d      = p - arc.center;
    const double height = d.dot(arc.normal);
    const double radial = (d - height * arc.normal).norm() - arc.radius;
    return std::sqrt(radial * radial + height * height);
}

// Writes the moves, following the position, the extrusion and the feedrate of the printer.
class Writer
{
public:
    Writer(bool relative_e, bool comments) : m_relative_e(relative_e), m_comments(comments) {}

    const std::string &gcode() const { return m_gcode; }

    void comment(const std::string &text)
    {
        if (m_comments)
            m_gcode += "; " + text + "\n";
    }

    void line(const std::string &text, const char *comment = nullptr)
    {
        m_gcode += text;
        if (m_comments && comment != nullptr)
            (m_gcode += " ; ") += comment;
        m_gcode += '\n';
    }

    void set_position(const Vec3d &position)
    {
        m_position = Vec3d(round_to(position.x(), 3), round_to(position.y(), 3), round_to(position.z(), 3));
        m_known    = true;
    }
    void set_feedrate(double value) { m_feedrate = value; }

    // Straight move extruding de, speed in mm/s. The coordinates which don't change aren't written.
    void move_to(const Vec3d &to, double de, double speed, const char *comment = nullptr)
    {
        std::string text = "G1";
        for (int axis = 0; axis < 3; ++ axis) {
            const double value = round_to(to[axis], 3);
            if (!m_known || value != m_position[axis]) {
                text += ' ';
                text += "XYZ"[axis];
                text += num(value, 3);
                m_position[axis] = value;
            }
        }
        m_known = true;
        append_e(text, de);
        if (text.size() == 2)
            // Nothing to move.
            return;
        append_f(text, speed);
        line(text, comment);
    }

    // Arc in the horizontal plane of the current position, counterclockwise if ccw.
    void arc_to(bool ccw, const Vec3d &to, const Vec3d &center, double de, double speed, const char *comment = nullptr)
    {
        // I and J from the start as written, so that the printer finds the same center.
        const double x = round_to(to.x(), 3);
        const double y = round_to(to.y(), 3);
        std::string text = ccw ? "G3" : "G2";
        text += " X" + num(x, 3) + " Y" + num(y, 3);
        text += " I" + num(round_to(center.x() - m_position.x(), 3), 3) + " J" + num(round_to(center.y() - m_position.y(), 3), 3);
        m_position.x() = x;
        m_position.y() = y;
        append_e(text, de);
        append_f(text, speed);
        line(text, comment);
    }

    void retract(double length, double speed)
    {
        if (m_retracted || length <= 0.)
            return;
        std::string text = "G1";
        append_e(text, -length);
        append_f(text, speed);
        line(text, "retract");
        m_retracted      = true;
        m_retract_length = length;
    }

    void unretract(double speed)
    {
        if (!m_retracted)
            return;
        std::string text = "G1";
        append_e(text, m_retract_length);
        append_f(text, speed);
        line(text, "unretract");
        m_retracted = false;
    }

private:
    void append_e(std::string &text, double de)
    {
        if (std::abs(de) < 1e-9)
            return;
        m_e += de;
        text += " E" + num(m_relative_e ? de : m_e, 5);
    }

    void append_f(std::string &text, double speed)
    {
        const double value = feedrate(speed);
        if (value == m_feedrate)
            return;
        text += " F" + num(value, 0);
        m_feedrate = value;
    }

    std::string m_gcode;
    bool        m_relative_e;
    bool        m_comments;
    Vec3d       m_position       { Vec3d::Zero() };
    bool        m_known          { false };
    double      m_e              { 0. };
    double      m_feedrate       { -1. };
    bool        m_retracted      { false };
    double      m_retract_length { 0. };
};

double filament_area(const Settings &settings) { return 0.25 * PI * settings.filament_diameter * settings.filament_diameter; }

// mm of filament per mm of an extruding segment.
double e_per_mm(const Segment &segment, const Settings &settings)
{
    const double area   = filament_area(settings);
    const double width  = segment.width > 0. ? segment.width : settings.line_width;
    const double height = segment.height > 0. ? segment.height : settings.layer_height;
    return area > 0. ? extrusion_area(width, height) / area * settings.flow_ratio : 0.;
}

// mm/s
double print_speed(const Segment &segment, const Settings &settings) { return std::max(segment.speed > 0. ? segment.speed : settings.print_speed, 1.); }
double travel_speed(const Segment &segment, const Settings &settings) { return std::max(segment.speed > 0. ? segment.speed : settings.travel_speed, 1.); }

// Moves of a segment from 'from', raised by hop (travel moves).
void write_path(Writer &writer, const Vec3d &from, const Segment &segment, const Arc &arc, double hop, double e_rate, double speed,
                const ExportOptions &options)
{
    const Vec3d offset(0., 0., hop);
    if (!arc.valid) {
        writer.move_to(segment.end + offset, (segment.end - from).norm() * e_rate, speed);
        return;
    }
    const bool horizontal = std::abs(segment.mid.z() - from.z()) < Z_EPSILON && std::abs(segment.end.z() - from.z()) < Z_EPSILON;
    if (options.arcs && horizontal) {
        writer.arc_to(arc.normal.z() > 0., segment.end + offset, arc.center + offset, arc.length() * e_rate, speed);
        return;
    }
    Vec3d previous = from;
    for (const Vec3d &point : discretize_arc(arc, options.arc_deviation)) {
        writer.move_to(point + offset, (point - previous).norm() * e_rate, speed);
        previous = point;
    }
}

// First layer of the drawing: the drawing itself, raised along its length in spiral mode. Its arcs are then split in
// quarters of turn, so that each piece rises steadily: an arc through 3 raised points over more than half a turn would
// lie in a tilted plane and dip below its start, down to a vertical circle for a full turn.
Drawing first_layer(const Drawing &drawing, const Settings &settings)
{
    if (!settings.spiral || !drawing.has_start)
        return drawing;
    const double dz = std::max(settings.layer_height, 0.);
    Drawing      layer;
    layer.has_start = true;
    layer.start     = drawing.start;
    layer.segments.reserve(drawing.segments.size());
    // Length of the drawing up to the end and up to the middle point of each piece.
    std::vector<double> to_end;
    std::vector<double> to_mid;
    double              length = 0.;
    for (size_t i = 0; i < drawing.segments.size(); ++ i) {
        const Segment &segment = drawing.segments[i];
        const Vec3d   &from    = drawing.segment_start(i);
        const Arc      arc     = segment.type == SegmentType::Arc ? arc_through(from, segment.mid, segment.end) : Arc();
        if (!arc.valid) {
            // Arc through aligned points: the straight line it is printed as. Its middle point raised by half the rise
            // of its end would make a real arc, out of the line, even below the bed.
            Segment line = segment;
            line.type    = SegmentType::Line;
            const double piece = (segment.end - from).norm();
            to_mid.push_back(length + 0.5 * piece);
            length += piece;
            to_end.push_back(length);
            layer.segments.push_back(line);
            continue;
        }
        const int    pieces = std::max(1, int(std::ceil(arc.angle / (0.5 * PI) - 1e-9)));
        const double piece  = arc.length() / pieces;
        for (int k = 0; k < pieces; ++ k) {
            Segment part = segment;
            part.mid     = arc.point_at((k + 0.5) / pieces);
            part.end     = k + 1 == pieces ? segment.end : arc.point_at(double(k + 1) / pieces);
            to_mid.push_back(length + 0.5 * piece);
            length += piece;
            to_end.push_back(length);
            layer.segments.push_back(part);
        }
    }
    if (length > 0.)
        for (size_t i = 0; i < layer.segments.size(); ++ i) {
            layer.segments[i].end.z() += dz * to_end[i] / length;
            if (layer.segments[i].type == SegmentType::Arc)
                layer.segments[i].mid.z() += dz * to_mid[i] / length;
        }
    return layer;
}

// The drawing on its layers, read segment by segment without storing them: the first layer, then each next layer one
// layer height higher, after a travel move from the end of the previous layer (none for a closed spiral).
class Stack
{
public:
    Stack(const Drawing &drawing, const Settings &settings)
        : m_layer(first_layer(drawing, settings))
        , m_layers(std::clamp(settings.layers, 1, MAX_LAYERS))
        , m_dz(std::max(settings.layer_height, 0.))
    {
        if (m_layer.has_start && m_layers > 1)
            m_transition = (m_layer.start + Vec3d(0., 0., m_dz) - m_layer.last_point()).norm() >= MIN_LENGTH;
    }

    bool           empty() const { return !m_layer.has_start; }
    const Vec3d   &start() const { return m_layer.start; }
    const Drawing &first() const { return m_layer; }
    int            layers() const { return m_layers; }
    bool           transition() const { return m_transition; }

    size_t size() const
    {
        const size_t count = m_layer.segments.size();
        return count + size_t(m_layers - 1) * (count + (m_transition ? 1 : 0));
    }

    Segment segment(size_t idx) const
    {
        const size_t count = m_layer.segments.size();
        if (idx < count)
            return m_layer.segments[idx];
        const size_t block = count + (m_transition ? 1 : 0);
        const size_t layer = 1 + (idx - count) / block;
        size_t       rank  = (idx - count) % block;
        const Vec3d  up(0., 0., double(layer) * m_dz);
        if (m_transition) {
            if (rank == 0) {
                Segment travel;
                travel.extrude = false;
                travel.end     = m_layer.start + up;
                return travel;
            }
            -- rank;
        }
        Segment segment = m_layer.segments[rank];
        segment.end += up;
        segment.mid += up;
        return segment;
    }

private:
    Drawing m_layer;
    int     m_layers;
    double  m_dz;
    bool    m_transition { false };
};

} // namespace

bool Segment::operator==(const Segment &rhs) const
{
    return type == rhs.type && extrude == rhs.extrude && end == rhs.end && mid == rhs.mid && width == rhs.width &&
           height == rhs.height && speed == rhs.speed;
}

void Drawing::clear()
{
    has_start = false;
    start     = Vec3d::Zero();
    segments.clear();
}

bool Drawing::operator==(const Drawing &rhs) const
{
    return has_start == rhs.has_start && start == rhs.start && segments == rhs.segments;
}

std::vector<PointRef> list_points(const Drawing &drawing)
{
    std::vector<PointRef> points;
    if (!drawing.has_start)
        return points;
    points.reserve(1 + 2 * drawing.segments.size());
    points.push_back({ -1, false });
    for (size_t i = 0; i < drawing.segments.size(); ++ i) {
        if (drawing.segments[i].type == SegmentType::Arc)
            points.push_back({ int(i), true });
        points.push_back({ int(i), false });
    }
    return points;
}

const Vec3d* point_position(const Drawing &drawing, const PointRef &ref)
{
    if (!drawing.has_start)
        return nullptr;
    if (ref.segment < 0)
        return &drawing.start;
    if (size_t(ref.segment) >= drawing.segments.size())
        return nullptr;
    const Segment &segment = drawing.segments[size_t(ref.segment)];
    if (ref.mid)
        return segment.type == SegmentType::Arc ? &segment.mid : nullptr;
    return &segment.end;
}

Vec3d* point_position(Drawing &drawing, const PointRef &ref)
{
    return const_cast<Vec3d*>(point_position(static_cast<const Drawing&>(drawing), ref));
}

Vec3d Arc::point_at(double t) const
{
    if (t >= 1.)
        return end;
    const Vec3d  u   = start - center;
    const Vec3d  v   = normal.cross(u);
    const double phi = t * angle;
    return center + std::cos(phi) * u + std::sin(phi) * v;
}

Arc arc_through(const Vec3d &start, const Vec3d &mid, const Vec3d &end)
{
    Arc arc;
    arc.start = start;
    arc.end   = end;
    if ((mid - start).norm() < MIN_LENGTH || (end - mid).norm() < MIN_LENGTH)
        return arc;
    const Vec3d  chord        = end - start;
    const double chord_length = chord.norm();
    if (chord_length < MIN_LENGTH) {
        // Back to the start: full horizontal circle whose diameter goes from the start to the middle point.
        if (std::abs(mid.z() - start.z()) > Z_EPSILON)
            return arc;
        arc.center     = 0.5 * (start + mid);
        arc.center.z() = start.z();
        arc.radius     = 0.5 * (mid - start).norm();
        arc.normal     = Vec3d::UnitZ();
        arc.angle      = 2. * PI;
        arc.valid      = true;
        return arc;
    }
    // Middle point on the chord: straight line.
    if ((mid - start).cross(chord).norm() / chord_length < MIN_LENGTH)
        return arc;
    // Center of the circle through the 3 points.
    const Vec3d  a   = start - end;
    const Vec3d  b   = mid - end;
    const Vec3d  axb = a.cross(b);
    const double n2  = axb.squaredNorm();
    if (n2 <= 0.)
        return arc;
    const Vec3d  center = end + (a.squaredNorm() * b - b.squaredNorm() * a).cross(axb) / (2. * n2);
    const double radius = (start - center).norm();
    if (radius > MAX_RADIUS)
        return arc;
    // The arc turns from the start to the end through the middle point.
    const Vec3d u = start - center;
    const Vec3d w = end - center;
    arc.normal = (mid - start).cross(end - mid).normalized();
    double angle = std::atan2(u.cross(w).dot(arc.normal), u.dot(w));
    if (angle <= 0.)
        angle += 2. * PI;
    arc.center = center;
    arc.radius = radius;
    arc.angle  = angle;
    arc.valid  = true;
    return arc;
}

std::vector<Vec3d> discretize_arc(const Arc &arc, double max_deviation, size_t max_points)
{
    std::vector<Vec3d> points;
    if (!arc.valid)
        return points;
    double step = arc.angle;
    if (max_deviation > 0. && max_deviation < arc.radius)
        step = 2. * std::acos(1. - max_deviation / arc.radius);
    // At least one point every quarter of turn.
    step = std::min(step, 0.5 * PI);
    size_t count = size_t(std::ceil(arc.angle / std::max(step, 1e-6)));
    count = std::min(std::max<size_t>(count, 1), std::max<size_t>(max_points, 1));
    points.reserve(count);
    for (size_t i = 1; i <= count; ++ i)
        points.emplace_back(arc.point_at(double(i) / double(count)));
    return points;
}

double extrusion_area(double width, double height)
{
    if (width <= 0. || height <= 0.)
        return 0.;
    // Rectangle with semicircles at the ends, as Flow::mm3_per_mm().
    const double area = height * (width - height * (1. - 0.25 * PI));
    // Thread narrower than high: round section.
    return area > 0. ? area : 0.25 * PI * width * width;
}

double layers_height(const Settings &settings)
{
    const double dz = std::max(settings.layer_height, 0.);
    return (std::max(settings.layers, 1) - 1) * dz + (settings.spiral ? dz : 0.);
}

Drawing stack_layers(const Drawing &drawing, const Settings &settings)
{
    const Stack stack(drawing, settings);
    Drawing     stacked;
    if (stack.empty())
        return stacked;
    stacked.has_start = true;
    stacked.start     = stack.start();
    const size_t count = stack.size();
    stacked.segments.reserve(count);
    for (size_t idx = 0; idx < count; ++ idx)
        stacked.segments.push_back(stack.segment(idx));
    return stacked;
}

size_t stacked_segments(const Drawing &drawing, const Settings &settings)
{
    return Stack(drawing, settings).size();
}

std::vector<Segment> rectangle_segments(const Vec3d &corner, const Vec3d &opposite, const Vec3d &u, const Vec3d &v)
{
    const Vec3d diagonal = opposite - corner;
    const Vec3d du       = diagonal.dot(u) * u;
    const Vec3d dv       = diagonal.dot(v) * v;
    if (du.norm() < MIN_LENGTH || dv.norm() < MIN_LENGTH)
        return {};
    std::vector<Segment> segments(4);
    segments[0].end = corner + du;
    segments[1].end = corner + du + dv;
    segments[2].end = corner + dv;
    segments[3].end = corner;
    return segments;
}

std::vector<Segment> circle_segments(const Vec3d &center, const Vec3d &start, const Vec3d &normal)
{
    const Vec3d n = normal.normalized();
    Vec3d       r = start - center;
    r -= r.dot(n) * n;
    if (r.norm() < MIN_LENGTH)
        return {};
    // A quarter of turn further, counterclockwise around the normal.
    const Vec3d          q = n.cross(r);
    std::vector<Segment> segments(2);
    for (Segment &segment : segments)
        segment.type = SegmentType::Arc;
    segments[0].mid = center + q;
    segments[0].end = center - r;
    segments[1].mid = center - q;
    segments[1].end = center + r;
    return segments;
}

Vec3d end_tangent(const Drawing &drawing, size_t idx)
{
    if (!drawing.has_start || idx >= drawing.segments.size())
        return Vec3d::Zero();
    const Segment &segment = drawing.segments[idx];
    const Vec3d   &from    = drawing.segment_start(idx);
    if (segment.type == SegmentType::Arc) {
        const Arc arc = arc_through(from, segment.mid, segment.end);
        if (arc.valid) {
            // Counterclockwise around the normal, the nozzle moves along normal x (point - center).
            const Vec3d tangent = arc.normal.cross(arc.end - arc.center);
            if (tangent.norm() > 1e-9)
                return tangent.normalized();
        }
    }
    const Vec3d chord = segment.end - from;
    if (chord.norm() < MIN_LENGTH)
        return Vec3d::Zero();
    return chord.normalized();
}

bool tangent_arc_middle(const Vec3d &start, const Vec3d &tangent, const Vec3d &end, Vec3d &middle)
{
    const Vec3d chord = end - start;
    if (chord.norm() < MIN_LENGTH || tangent.norm() < 1e-9)
        return false;
    const Vec3d t = tangent.normalized();
    // Part of the chord across the tangent, toward the center of the arc.
    const Vec3d  across = chord - chord.dot(t) * t;
    const double offset = across.norm();
    if (offset < MIN_LENGTH)
        return false;
    // The center is on the perpendicular to the tangent at the start, as far from the start as from the end.
    const double radius = chord.squaredNorm() / (2. * offset);
    if (radius > MAX_RADIUS)
        return false;
    const Vec3d p      = across / offset;
    const Vec3d center = start + radius * p;
    // Counterclockwise around this normal, the arc leaves the start along the tangent.
    const Vec3d normal = t.cross(p);
    const Vec3d u      = start - center;
    const Vec3d w      = end - center;
    double      angle  = std::atan2(u.cross(w).dot(normal), u.dot(w));
    if (angle <= 0.)
        angle += 2. * PI;
    middle = center + std::cos(0.5 * angle) * u + std::sin(0.5 * angle) * normal.cross(u);
    return true;
}

bool arc_middle_with_radius(const Vec3d &start, const Vec3d &end, const Vec3d &side, double radius, Vec3d &middle)
{
    const Vec3d  chord  = end - start;
    const double length = chord.norm();
    if (length < MIN_LENGTH)
        return false;
    const Vec3d c         = chord / length;
    const Vec3d chord_mid = 0.5 * (start + end);
    Vec3d       to_side   = side - chord_mid;
    to_side -= to_side.dot(c) * c;
    const double height = to_side.norm();
    if (height < MIN_LENGTH)
        return false;
    const double r = std::max(radius, 0.5 * length);
    // Distance from the middle of the chord to the center.
    const double d = std::sqrt(std::max(r * r - 0.25 * length * length, 0.));
    // The short arc bulges by r - d, the long one by r + d: the one closer to the side point.
    const double bulge = height > r ? r + d : r - d;
    middle = chord_mid + bulge * (to_side / height);
    return true;
}

Vec3d start_tangent(const Drawing &drawing, size_t idx)
{
    if (!drawing.has_start || idx >= drawing.segments.size())
        return Vec3d::Zero();
    const Segment &segment = drawing.segments[idx];
    const Vec3d   &from    = drawing.segment_start(idx);
    if (segment.type == SegmentType::Arc) {
        const Arc arc = arc_through(from, segment.mid, segment.end);
        if (arc.valid) {
            const Vec3d tangent = arc.normal.cross(arc.start - arc.center);
            if (tangent.norm() > 1e-9)
                return tangent.normalized();
        }
    }
    const Vec3d chord = segment.end - from;
    if (chord.norm() < MIN_LENGTH)
        return Vec3d::Zero();
    return chord.normalized();
}

Path rectangle_3_points(const Vec3d &a, const Vec3d &b, const Vec3d &c, const Vec3d &normal)
{
    Path         path;
    const Vec3d  n      = normal.normalized();
    const Vec3d  side   = b - a;
    const double length = side.norm();
    if (length < MIN_LENGTH)
        return path;
    // Across the side, in the plane.
    Vec3d across = n.cross(side / length);
    if (across.norm() < 1e-6)
        return path;
    across.normalize();
    const double width = (c - b).dot(across);
    if (std::abs(width) < MIN_LENGTH)
        return path;
    const Vec3d w = width * across;
    path.start    = a;
    path.segments = { line_to(b), line_to(b + w), line_to(a + w), line_to(a) };
    return path;
}

Path center_rectangle(const Vec3d &center, const Vec3d &corner, const Vec3d &u, const Vec3d &v)
{
    Path        path;
    const Vec3d d  = corner - center;
    const Vec3d du = d.dot(u) * u;
    const Vec3d dv = d.dot(v) * v;
    if (du.norm() < MIN_LENGTH || dv.norm() < MIN_LENGTH)
        return path;
    path.start    = center - du - dv;
    path.segments = rectangle_segments(path.start, center + du + dv, u, v);
    return path;
}

Path circle(const Vec3d &center, const Vec3d &point, const Vec3d &normal)
{
    Path path;
    path.segments = circle_segments(center, point, normal);
    if (!path.segments.empty())
        path.start = path.segments.back().end;
    return path;
}

Path circle_2_points(const Vec3d &a, const Vec3d &b, const Vec3d &normal)
{
    return circle(0.5 * (a + b), a, normal);
}

Path circle_3_points(const Vec3d &a, const Vec3d &b, const Vec3d &c)
{
    if ((c - a).norm() < MIN_LENGTH)
        return Path();
    const Arc arc = arc_through(a, b, c);
    if (!arc.valid)
        return Path();
    return circle(arc.center, a, arc.normal);
}

Path center_arc(const Vec3d &center, const Vec3d &start, double angle, const Vec3d &normal)
{
    Path        path;
    const Vec3d n = normal.normalized();
    const Vec3d r = in_plane(start - center, n);
    if (r.norm() < MIN_LENGTH || std::abs(angle) < 1e-6)
        return path;
    path.start = center + r;
    add_arc_around(path.segments, center, path.start, std::clamp(angle, -2. * PI, 2. * PI), n);
    return path;
}

Arc arc_around(const Vec3d &center, const Vec3d &start, double angle, const Vec3d &normal)
{
    Arc         arc;
    const Vec3d n = normal.normalized();
    const Vec3d r = in_plane(start - center, n);
    angle         = std::clamp(angle, -2. * PI, 2. * PI);
    if (r.norm() < MIN_LENGTH || std::abs(angle) < 1e-6)
        return arc;
    arc.center = center;
    arc.radius = r.norm();
    arc.start  = center + r;
    // Counterclockwise around its normal: the normal turned over for a negative angle.
    arc.normal = angle > 0. ? n : Vec3d(-n);
    arc.angle  = std::abs(angle);
    arc.end    = turn(center, arc.start, angle, n);
    arc.valid  = true;
    return arc;
}

Path polygon(const Vec3d &center, const Vec3d &point, int sides, bool inscribed, const Vec3d &normal)
{
    Path         path;
    const Vec3d  n        = normal.normalized();
    const Vec3d  r        = in_plane(point - center, n);
    const double distance = r.norm();
    if (sides < 3 || distance < MIN_LENGTH)
        return path;
    const double step = 2. * PI / sides;
    // The point is a vertex, or the middle of a side: its vertices are then half a step around it.
    const double radius = inscribed ? distance : distance / std::cos(0.5 * step);
    const double offset = inscribed ? 0. : 0.5 * step;
    const Vec3d  first  = center + (radius / distance) * r;
    path.start = turn(center, first, offset, n);
    for (int k = 1; k < sides; ++ k)
        path.segments.push_back(line_to(turn(center, first, offset + k * step, n)));
    path.segments.push_back(line_to(path.start));
    return path;
}

Path edge_polygon(const Vec3d &a, const Vec3d &b, const Vec3d &side, int sides, const Vec3d &normal)
{
    Path         path;
    const Vec3d  n      = normal.normalized();
    const Vec3d  edge   = in_plane(b - a, n);
    const double length = edge.norm();
    if (sides < 3 || length < MIN_LENGTH)
        return path;
    const Vec3d  left = n.cross(edge / length);
    // On the side of the point: counterclockwise around the normal when it is on the left of the side.
    const double sign   = (side - a).dot(left) >= 0. ? 1. : -1.;
    const double step   = 2. * PI / sides;
    const Vec3d  center = a + 0.5 * edge + sign * (0.5 * length / std::tan(0.5 * step)) * left;
    path.start = a;
    path.segments.push_back(line_to(a + edge));
    for (int k = 2; k < sides; ++ k)
        path.segments.push_back(line_to(turn(center, a, sign * k * step, n)));
    path.segments.push_back(line_to(a));
    return path;
}

std::vector<Segment> curve_segments(const std::function<Vec3d(double)> &f, double tolerance)
{
    std::vector<Segment> segments;
    tolerance = std::max(tolerance, 1e-4);
    // Length of the curve, roughly.
    double length   = 0.;
    Vec3d  previous = f(0.);
    for (int i = 1; i <= 64; ++ i) {
        const Vec3d p = f(double(i) / 64.);
        length += (p - previous).norm();
        previous = p;
    }
    if (length < MIN_LENGTH)
        return segments;
    // Arcs about 4 mm long, more of them until they follow the curve.
    size_t arcs = std::clamp<size_t>(size_t(std::ceil(length / 4.)), 2, MAX_CURVE_ARCS);
    for (;;) {
        std::vector<Vec3d> points(2 * arcs + 1);
        for (size_t i = 0; i <= 2 * arcs; ++ i)
            points[i] = f(double(i) / double(2 * arcs));
        segments.clear();
        double worst = 0.;
        for (size_t k = 0; k < arcs; ++ k) {
            const Vec3d &a = points[2 * k];
            const Vec3d &m = points[2 * k + 1];
            const Vec3d &b = points[2 * k + 2];
            Segment      segment = line_to(b);
            const Arc    arc     = arc_through(a, m, b);
            // Straight where the 3 points are aligned; an arc of more than half a turn between close points of a curve
            // would be a loop.
            const bool curved = arc.valid && arc.angle < PI;
            if (curved) {
                segment.type = SegmentType::Arc;
                segment.mid  = m;
            }
            // How far the curve is from it between its points.
            for (const double q : { 0.5, 1.5 }) {
                const Vec3d p = f((2. * double(k) + q) / double(2 * arcs));
                worst = std::max(worst, curved ? distance_to_circle(arc, p) : distance_to_segment(p, a, b));
            }
            segments.push_back(segment);
        }
        if (worst <= tolerance || arcs >= MAX_CURVE_ARCS)
            break;
        arcs = std::min(arcs * 2, MAX_CURVE_ARCS);
    }
    return segments;
}

Path ellipse(const Vec3d &center, const Vec3d &major, const Vec3d &minor, const Vec3d &normal, double tolerance)
{
    Path         path;
    const Vec3d  n = normal.normalized();
    const Vec3d  r = in_plane(major - center, n);
    const double a = r.norm();
    if (a < MIN_LENGTH)
        return path;
    const Vec3d  e1 = r / a;
    const Vec3d  e2 = n.cross(e1);
    const double b  = std::abs((minor - center).dot(e2));
    if (b < MIN_LENGTH)
        return path;
    path.start    = center + r;
    path.segments = curve_segments(
        [&](double t) { return Vec3d(center + a * std::cos(2. * PI * t) * e1 + b * std::sin(2. * PI * t) * e2); }, tolerance);
    if (!path.segments.empty())
        path.segments.back().end = path.start;
    return path;
}

Path slot(const Vec3d &a, const Vec3d &b, double width, const Vec3d &normal)
{
    Path         path;
    const Vec3d  n = normal.normalized();
    const double r = 0.5 * std::abs(width);
    if (r < MIN_LENGTH)
        return path;
    const Vec3d  axis   = in_plane(b - a, n);
    const double length = axis.norm();
    if (length < MIN_LENGTH) {
        // Its two ends merged: a circle.
        Vec3d e1, e2;
        plane_axes(n, e1, e2);
        return circle(a, Vec3d(a + r * e1), n);
    }
    const Vec3d d    = axis / length;
    const Vec3d left = n.cross(d);
    const Vec3d end  = a + axis;
    path.start = a - r * left;
    Segment cap_end;
    cap_end.type = SegmentType::Arc;
    cap_end.mid  = end + r * d;
    cap_end.end  = end + r * left;
    Segment cap_start;
    cap_start.type = SegmentType::Arc;
    cap_start.mid  = a - r * d;
    cap_start.end  = path.start;
    path.segments  = { line_to(end - r * left), cap_end, line_to(a + r * left), cap_start };
    return path;
}

Path arc_slot(const Arc &arc, double width)
{
    Path         path;
    const double r = 0.5 * std::abs(width);
    if (!arc.valid || r < MIN_LENGTH || r > arc.radius - MIN_LENGTH || arc.angle > 2. * PI - 1e-6)
        return path;
    const Vec3d &c         = arc.center;
    const Vec3d &n         = arc.normal;
    const Vec3d  start_dir = (arc.start - c) / arc.radius;
    const Vec3d  end_dir   = (arc.end - c) / arc.radius;
    const Vec3d  outer_start = c + (arc.radius + r) * start_dir;
    const Vec3d  outer_end   = c + (arc.radius + r) * end_dir;
    const Vec3d  inner_start = c + (arc.radius - r) * start_dir;
    const Vec3d  inner_end   = c + (arc.radius - r) * end_dir;
    path.start = outer_start;
    // The outer side along the arc, the half circle around its end, ahead of it, the inner side back, the half circle
    // around its start, behind it.
    add_arc_around(path.segments, c, outer_start, arc.angle, n);
    path.segments.back().end = outer_end;
    Segment cap;
    cap.type = SegmentType::Arc;
    cap.mid  = arc.end + r * n.cross(end_dir);
    cap.end  = inner_end;
    path.segments.push_back(cap);
    add_arc_around(path.segments, c, inner_end, -arc.angle, n);
    path.segments.back().end = inner_start;
    cap.mid = arc.start - r * n.cross(start_dir);
    cap.end = outer_start;
    path.segments.push_back(cap);
    return path;
}

Path spline(const std::vector<Vec3d> &input, bool closed, double tolerance)
{
    Path path;
    // Without the points merged with the previous one.
    std::vector<Vec3d> points;
    for (const Vec3d &p : input)
        if (points.empty() || (p - points.back()).norm() >= MIN_LENGTH)
            points.push_back(p);
    if (points.size() > 2 && (points.back() - points.front()).norm() < MIN_LENGTH)
        points.pop_back();
    if (points.size() < 3)
        closed = false;
    if (points.size() < 2)
        return path;
    const long count = long(points.size());
    // The points around a span; beyond the ends of an open curve, the reflection of the next point.
    auto at = [&](long i) -> Vec3d {
        if (closed)
            return points[size_t(((i % count) + count) % count)];
        if (i < 0)
            return 2. * points[0] - points[1];
        if (i >= count)
            return 2. * points[size_t(count - 1)] - points[size_t(count - 2)];
        return points[size_t(i)];
    };
    // Centripetal parameters: no loop nor cusp between the points.
    auto knot = [](const Vec3d &a, const Vec3d &b) { return std::max(std::sqrt((b - a).norm()), 1e-6); };
    path.start         = points.front();
    const long   spans = closed ? count : count - 1;
    for (long i = 0; i < spans; ++ i) {
        const Vec3d  p0 = at(i - 1);
        const Vec3d  p1 = at(i);
        const Vec3d  p2 = at(i + 1);
        const Vec3d  p3 = at(i + 2);
        const double t0 = 0.;
        const double t1 = t0 + knot(p0, p1);
        const double t2 = t1 + knot(p1, p2);
        const double t3 = t2 + knot(p2, p3);
        // Barry and Goldman's pyramid.
        auto point = [&](double s) {
            const double t  = t1 + s * (t2 - t1);
            const Vec3d  a1 = ((t1 - t) * p0 + (t - t0) * p1) / (t1 - t0);
            const Vec3d  a2 = ((t2 - t) * p1 + (t - t1) * p2) / (t2 - t1);
            const Vec3d  a3 = ((t3 - t) * p2 + (t - t2) * p3) / (t3 - t2);
            const Vec3d  b1 = ((t2 - t) * a1 + (t - t0) * a2) / (t2 - t0);
            const Vec3d  b2 = ((t3 - t) * a2 + (t - t1) * a3) / (t3 - t1);
            return Vec3d(((t2 - t) * b1 + (t - t1) * b2) / (t2 - t1));
        };
        std::vector<Segment> piece = curve_segments(point, tolerance);
        if (piece.empty())
            continue;
        // Exactly through the points.
        piece.back().end = p2;
        path.segments.insert(path.segments.end(), piece.begin(), piece.end());
    }
    return path;
}

Path conic(const Vec3d &start, const Vec3d &end, const Vec3d &apex, double rho, double tolerance)
{
    Path         path;
    const Vec3d  chord  = end - start;
    const double length = chord.norm();
    // The apex off the chord, or it is a straight line.
    if (length < MIN_LENGTH || (apex - start).cross(chord).norm() / length < MIN_LENGTH)
        return path;
    // Rational quadratic Bezier curve, the weight of its middle point from rho.
    rho            = std::clamp(rho, 0.01, 0.99);
    const double w = rho / (1. - rho);
    path.start     = start;
    path.segments  = curve_segments(
        [&](double t) {
            const double a = (1. - t) * (1. - t);
            const double b = 2. * w * t * (1. - t);
            const double c = t * t;
            return Vec3d((a * start + b * apex + c * end) / (a + b + c));
        },
        tolerance);
    if (!path.segments.empty())
        path.segments.back().end = end;
    return path;
}

bool circle_tangent_2(const Vec3d &a1, const Vec3d &b1, const Vec3d &a2, const Vec3d &b2, const Vec3d &point, double radius,
                      const Vec3d &normal, Vec3d &center, double &out_radius)
{
    const Vec3d n = normal.normalized();
    Vec3d       e1, e2;
    plane_axes(n, e1, e2);
    // Coordinates in the plane, from point.
    auto flat = [&](const Vec3d &p) {
        const Vec3d d = p - point;
        return Vec2d(d.dot(e1), d.dot(e2));
    };
    auto back = [&](const Vec2d &p) { return Vec3d(point + p.x() * e1 + p.y() * e2); };
    auto cross = [](const Vec2d &a, const Vec2d &b) { return a.x() * b.y() - a.y() * b.x(); };
    const Vec2d p1 = flat(a1);
    const Vec2d p2 = flat(a2);
    Vec2d       d1 = flat(b1) - p1;
    Vec2d       d2 = flat(b2) - p2;
    if (d1.norm() < MIN_LENGTH || d2.norm() < MIN_LENGTH)
        return false;
    d1.normalize();
    d2.normalize();
    const double det = cross(d1, d2);
    if (std::abs(det) < 1e-9) {
        // Parallel lines: the circle between them, centered on their middle line where point is.
        const Vec2d  across(-d1.y(), d1.x());
        const double h1 = -p1.dot(across);
        const double h2 = (p2 - p1).dot(across);
        if (std::abs(h2) < MIN_LENGTH)
            return false;
        center     = back(Vec2d((0.5 * h2 - h1) * across));
        out_radius = 0.5 * std::abs(h2);
        return true;
    }
    // Corner where the lines meet, and the sides of the angle where point (the origin) is.
    const Vec2d  corner = p1 + (cross(p2 - p1, d2) / det) * d1;
    const Vec2d  to     = -corner;
    const Vec2d  w1     = (cross(to, d2) / det >= 0. ? 1. : -1.) * d1;
    const Vec2d  w2     = (cross(d1, to) / det >= 0. ? 1. : -1.) * d2;
    Vec2d        bisector = w1 + w2;
    if (bisector.norm() < 1e-9)
        return false;
    bisector.normalize();
    const double sin_half = std::sin(0.5 * std::acos(std::clamp(w1.dot(w2), -1., 1.)));
    if (sin_half < 1e-6)
        return false;
    // Distance from the corner to the center.
    const double t = radius > 0. ? radius / sin_half : to.dot(bisector);
    if (t * sin_half < MIN_LENGTH)
        return false;
    center     = back(Vec2d(corner + t * bisector));
    out_radius = t * sin_half;
    return true;
}

bool circle_tangent_3(const std::array<Vec3d, 3> &a, const std::array<Vec3d, 3> &b, const std::array<Vec3d, 3> &picks,
                      const Vec3d &normal, Vec3d &center, double &radius)
{
    const Vec3d n = normal.normalized();
    Vec3d       e1, e2;
    plane_axes(n, e1, e2);
    const Vec3d origin = a[0];
    auto flat = [&](const Vec3d &p) {
        const Vec3d d = p - origin;
        return Vec2d(d.dot(e1), d.dot(e2));
    };
    // The lines as normal . x = offset, their normal unit.
    std::array<Vec2d, 3>  normals;
    std::array<double, 3> offsets {};
    std::array<Vec2d, 3>  touch;
    for (size_t i = 0; i < 3; ++ i) {
        const Vec2d p = flat(a[i]);
        Vec2d       d = flat(b[i]) - p;
        if (d.norm() < MIN_LENGTH)
            return false;
        d.normalize();
        normals[i] = Vec2d(-d.y(), d.x());
        offsets[i] = normals[i].dot(p);
        touch[i]   = flat(picks[i]);
    }
    // The circles on each side of each line: normal . c - side * r = offset.
    double best  = std::numeric_limits<double>::max();
    bool   found = false;
    for (int sides = 0; sides < 8; ++ sides) {
        Matrix3d m;
        Vec3d    rhs;
        for (int i = 0; i < 3; ++ i) {
            m(i, 0) = normals[size_t(i)].x();
            m(i, 1) = normals[size_t(i)].y();
            m(i, 2) = ((sides >> i) & 1) ? 1. : -1.;
            rhs(i)  = offsets[size_t(i)];
        }
        if (std::abs(m.determinant()) < 1e-12)
            continue;
        const Vec3d  x = m.inverse() * rhs;
        const double r = x.z();
        if (r < MIN_LENGTH)
            continue;
        const Vec2d c(x.x(), x.y());
        // Where it touches the lines, against where they were picked.
        double score = 0.;
        for (size_t i = 0; i < 3; ++ i)
            score += (c - (normals[i].dot(c) - offsets[i]) * normals[i] - touch[i]).norm();
        if (score < best) {
            best   = score;
            center = origin + c.x() * e1 + c.y() * e2;
            radius = r;
            found  = true;
        }
    }
    return found;
}

ShapeType shape_type(const Drawing &drawing, size_t first, size_t last)
{
    const std::vector<Segment> &segments = drawing.segments;
    if (!drawing.has_start || first > last || last >= segments.size())
        return ShapeType::None;
    for (size_t i = first + 1; i <= last; ++ i)
        if (segments[i].extrude != segments[first].extrude)
            return ShapeType::None;
    const Vec3d  start = drawing.segment_start(first);
    const size_t count = last - first + 1;
    // Back to its start.
    if ((segments[last].end - start).norm() > MIN_LENGTH)
        return ShapeType::None;

    if (count == 4) {
        Vec3d corners[5];
        corners[0] = start;
        for (size_t k = 0; k < 4; ++ k) {
            if (segments[first + k].type != SegmentType::Line)
                return ShapeType::None;
            corners[k + 1] = segments[first + k].end;
        }
        const Vec3d side1 = corners[1] - corners[0];
        const Vec3d side2 = corners[2] - corners[1];
        if (side1.norm() < MIN_LENGTH || side2.norm() < MIN_LENGTH)
            return ShapeType::None;
        // Opposite sides equal and opposed, square corners.
        if ((corners[3] - corners[2] + side1).norm() > MIN_LENGTH || (corners[4] - corners[3] + side2).norm() > MIN_LENGTH)
            return ShapeType::None;
        if (std::abs(side1.normalized().dot(side2.normalized())) > 1e-4)
            return ShapeType::None;
        return ShapeType::Rectangle;
    }
    if (count == 1 && segments[first].type == SegmentType::Arc) {
        const Arc arc = arc_through(start, segments[first].mid, segments[first].end);
        return arc.valid && arc.angle > 2. * PI - 1e-6 ? ShapeType::Circle : ShapeType::None;
    }
    if (count == 2 && segments[first].type == SegmentType::Arc && segments[last].type == SegmentType::Arc) {
        const Arc a = arc_through(start, segments[first].mid, segments[first].end);
        const Arc b = arc_through(segments[first].end, segments[last].mid, segments[last].end);
        if (!a.valid || !b.valid)
            return ShapeType::None;
        // One circle, turning one way.
        if ((a.center - b.center).norm() > MIN_LENGTH || std::abs(a.radius - b.radius) > MIN_LENGTH || a.normal.dot(b.normal) < 1. - 1e-6)
            return ShapeType::None;
        return std::abs(a.angle + b.angle - 2. * PI) < 1e-3 ? ShapeType::Circle : ShapeType::None;
    }
    return ShapeType::None;
}

ShapeType find_shape(const Drawing &drawing, size_t idx, size_t &first, size_t &last)
{
    const size_t count = drawing.segments.size();
    if (!drawing.has_start || idx >= count)
        return ShapeType::None;
    // A circle in one arc or in two, then a rectangle.
    for (const size_t length : { size_t(1), size_t(2), size_t(4) })
        for (size_t begin = idx >= length - 1 ? idx - (length - 1) : 0; begin <= idx && begin + length <= count; ++ begin) {
            const ShapeType type = shape_type(drawing, begin, begin + length - 1);
            if (type != ShapeType::None) {
                first = begin;
                last  = begin + length - 1;
                return type;
            }
        }
    return ShapeType::None;
}

void find_path(const Drawing &drawing, size_t idx, size_t &first, size_t &last)
{
    const std::vector<Segment> &segments = drawing.segments;
    first = idx;
    last  = idx;
    if (idx >= segments.size())
        return;
    const bool extrude = segments[idx].extrude;
    while (first > 0 && segments[first - 1].extrude == extrude)
        -- first;
    while (last + 1 < segments.size() && segments[last + 1].extrude == extrude)
        ++ last;
}

void remove_segments(Drawing &drawing, size_t first, size_t last)
{
    std::vector<Segment> &segments = drawing.segments;
    if (!drawing.has_start || first > last || last >= segments.size())
        return;
    const Vec3d next_start = segments[last].end;
    const bool  at_start   = first == 0;
    const bool  at_end     = last + 1 == segments.size();
    segments.erase(segments.begin() + first, segments.begin() + last + 1);
    if (at_start && !at_end) {
        // The drawing starts where the removed segments ended, on its next printed segment.
        drawing.start = next_start;
        while (!segments.empty() && !segments.front().extrude) {
            drawing.start = segments.front().end;
            segments.erase(segments.begin());
        }
    } else if (at_end) {
        // No travel move at the end.
        while (!segments.empty() && !segments.back().extrude)
            segments.pop_back();
    } else {
        // A straight travel move to the next segment, merged with the straight travel moves around it.
        size_t at = first;
        if (!segments[at - 1].extrude && segments[at - 1].type == SegmentType::Line) {
            -- at;
            segments[at].end = next_start;
        } else {
            Segment travel;
            travel.extrude = false;
            travel.end     = next_start;
            segments.insert(segments.begin() + at, travel);
        }
        if (at + 1 < segments.size() && !segments[at + 1].extrude && segments[at + 1].type == SegmentType::Line) {
            segments[at].end = segments[at + 1].end;
            segments.erase(segments.begin() + at + 1);
        }
        if ((segments[at].end - drawing.segment_start(at)).norm() < MIN_LENGTH)
            segments.erase(segments.begin() + at);
    }
    // A point alone draws nothing.
    if (segments.empty())
        drawing.clear();
}

Vec3d segment_point(const Drawing &drawing, size_t idx, double t)
{
    const Segment &segment = drawing.segments[idx];
    const Vec3d   &from    = drawing.segment_start(idx);
    if (segment.type == SegmentType::Arc) {
        const Arc arc = arc_through(from, segment.mid, segment.end);
        if (arc.valid)
            return arc.point_at(t);
    }
    return t >= 1. ? segment.end : Vec3d(from + t * (segment.end - from));
}

double segment_fraction(const Drawing &drawing, size_t idx, const Vec3d &p)
{
    const Segment &segment = drawing.segments[idx];
    const Vec3d   &from    = drawing.segment_start(idx);
    const Arc      arc     = segment.type == SegmentType::Arc ? arc_through(from, segment.mid, segment.end) : Arc();
    if (!arc.valid) {
        const Vec3d  ab = segment.end - from;
        const double l2 = ab.squaredNorm();
        return l2 > 0. ? std::clamp((p - from).dot(ab) / l2, 0., 1.) : 0.;
    }
    const Vec3d  u   = arc.start - arc.center;
    const Vec3d  w   = arc.normal.cross(u);
    const Vec3d  r   = p - arc.center;
    double       phi = std::atan2(r.dot(w), r.dot(u));
    if (phi < 0.)
        phi += 2. * PI;
    if (phi <= arc.angle)
        return phi / arc.angle;
    // Beyond its end: the closest end.
    return phi - arc.angle < 2. * PI - phi ? 1. : 0.;
}

Segment segment_part(const Drawing &drawing, size_t idx, double t0, double t1)
{
    const Segment &segment = drawing.segments[idx];
    const Vec3d   &from    = drawing.segment_start(idx);
    Segment        part    = segment;
    const Arc      arc     = segment.type == SegmentType::Arc ? arc_through(from, segment.mid, segment.end) : Arc();
    if (arc.valid) {
        const Vec3d start = arc.point_at(t0);
        part.end          = arc.point_at(t1);
        part.mid          = arc.point_at(0.5 * (t0 + t1));
        // Too short to be told from a straight line: one.
        part.type = arc_through(start, part.mid, part.end).valid ? SegmentType::Arc : SegmentType::Line;
        return part;
    }
    part.type = SegmentType::Line;
    part.end  = segment_point(drawing, idx, t1);
    return part;
}

void replace_segments(Drawing &drawing, size_t first, size_t last, const std::vector<Path> &paths)
{
    std::vector<Segment> &segments = drawing.segments;
    if (!drawing.has_start || first > last || last >= segments.size())
        return;
    const Vec3d before   = drawing.segment_start(first);
    const Vec3d after    = segments[last].end;
    const bool  at_start = first == 0;
    const bool  at_end   = last + 1 == segments.size();
    // The paths, linked by travel moves; the ranks of these travel moves in it.
    std::vector<Segment> middle;
    std::vector<size_t>  links;
    Vec3d                position     = before;
    bool                 has_position = !at_start;
    Vec3d                new_start    = drawing.start;
    auto travel_to = [&](const Vec3d &point) {
        Segment travel;
        travel.extrude = false;
        travel.end     = point;
        links.push_back(middle.size());
        middle.push_back(travel);
    };
    for (const Path &path : paths) {
        if (path.empty())
            continue;
        if (!has_position) {
            // At the start of the drawing: the drawing starts with the path.
            new_start    = path.start;
            has_position = true;
        } else if ((path.start - position).norm() >= MIN_LENGTH)
            travel_to(path.start);
        middle.insert(middle.end(), path.segments.begin(), path.segments.end());
        position = path.segments.back().end;
    }
    // Back to the rest of the drawing.
    if (!at_end) {
        if (!has_position) {
            new_start    = after;
            has_position = true;
        } else if ((after - position).norm() >= MIN_LENGTH)
            travel_to(after);
    }
    segments.erase(segments.begin() + first, segments.begin() + last + 1);
    segments.insert(segments.begin() + first, middle.begin(), middle.end());
    if (at_start)
        drawing.start = new_start;

    // The travel moves added merge with the straight travel moves around the paths (not with the ones of the paths).
    auto straight_travel = [](const Segment &segment) { return !segment.extrude && segment.type == SegmentType::Line; };
    const size_t count = middle.size();
    const size_t next  = first + count;
    if (!links.empty() && links.back() + 1 == count && next < segments.size() && straight_travel(segments[next])) {
        segments[next - 1].end = segments[next].end;
        segments.erase(segments.begin() + next);
    } else if (count == 0 && first > 0 && first < segments.size() && straight_travel(segments[first - 1]) && straight_travel(segments[first])) {
        // Nothing in their place: the travel moves around them follow each other.
        segments[first - 1].end = segments[first].end;
        segments.erase(segments.begin() + first);
    }
    if (!links.empty() && links.front() == 0 && first > 0 && first < segments.size() && straight_travel(segments[first - 1])) {
        segments[first - 1].end = segments[first].end;
        segments.erase(segments.begin() + first);
    }
    // No travel move at the start nor at the end of the drawing, none without length.
    while (!segments.empty() && !segments.front().extrude) {
        drawing.start = segments.front().end;
        segments.erase(segments.begin());
    }
    while (!segments.empty() && !segments.back().extrude)
        segments.pop_back();
    for (size_t i = 0; i < segments.size();) {
        if (straight_travel(segments[i]) && (segments[i].end - drawing.segment_start(i)).norm() < MIN_LENGTH)
            segments.erase(segments.begin() + i);
        else
            ++ i;
    }
    // A point alone draws nothing.
    if (segments.empty())
        drawing.clear();
}

void append_path(Drawing &drawing, const Path &path)
{
    if (path.empty())
        return;
    if (!drawing.has_start) {
        drawing.has_start = true;
        drawing.start     = path.start;
    } else if ((path.start - drawing.last_point()).norm() >= MIN_LENGTH) {
        Segment travel;
        travel.extrude = false;
        travel.end     = path.start;
        drawing.segments.push_back(travel);
    }
    drawing.segments.insert(drawing.segments.end(), path.segments.begin(), path.segments.end());
}

namespace {

// Two segments go on from one another without a corner when their directions differ by less than this (radians).
constexpr double SMOOTH_ANGLE  = 0.05;
// Segments at levels closer than this are in the same plane.
constexpr double LEVEL_EPSILON = 1e-3;
// Points this close to a piece are on it.
constexpr double CUT_EPSILON   = 1e-6;
// Offset: a corner is extended at most this many times the distance from it, otherwise rounded.
constexpr double MITER_LIMIT   = 4.;
// Offset: ends of the offset pieces closer than this join in their middle.
constexpr double SMOOTH_GAP    = 0.01;

double cross2(const Vec2d &a, const Vec2d &b) { return a.x() * b.y() - a.y() * b.x(); }
// a turned by a quarter of turn counterclockwise.
Vec2d  perp(const Vec2d &a) { return Vec2d(-a.y(), a.x()); }
double direction_angle(const Vec2d &v) { return std::atan2(v.y(), v.x()); }
// Angle in [0, 2 PI).
double wrap_angle(double angle)
{
    angle = std::fmod(angle, 2. * PI);
    return angle < 0. ? angle + 2. * PI : angle;
}

// Coordinates in a plane of a normal: along its two axes, and the level along the normal.
struct Frame
{
    Vec3d n;
    Vec3d e1;
    Vec3d e2;

    explicit Frame(const Vec3d &normal) : n(normal.normalized()) { plane_axes(n, e1, e2); }
    Vec2d  flat(const Vec3d &p) const { return Vec2d(p.dot(e1), p.dot(e2)); }
    double level(const Vec3d &p) const { return p.dot(n); }
    Vec3d  back(const Vec2d &q, double h) const { return Vec3d(q.x() * e1 + q.y() * e2 + h * n); }
};

// A segment seen in a plane: a line from a to b, or an arc around c of radius r from the angle a0, turning by sweep
// (counterclockwise around the normal when positive).
struct Piece
{
    bool   arc   { false };
    Vec2d  a     { Vec2d::Zero() };
    Vec2d  b     { Vec2d::Zero() };
    Vec2d  c     { Vec2d::Zero() };
    double r     { 0. };
    double a0    { 0. };
    double sweep { 0. };
    // Its box, the whole circle of an arc.
    Vec2d  lo    { Vec2d::Zero() };
    Vec2d  hi    { Vec2d::Zero() };

    Vec2d point(double t) const
    {
        if (t >= 1.)
            return b;
        if (!arc)
            return Vec2d(a + t * (b - a));
        const double angle = a0 + t * sweep;
        return Vec2d(c + r * Vec2d(std::cos(angle), std::sin(angle)));
    }

    // Unit direction at the fraction t.
    Vec2d tangent(double t) const
    {
        if (!arc)
            return Vec2d((b - a).normalized());
        const double angle = a0 + t * sweep;
        return (sweep > 0. ? 1. : -1.) * Vec2d(-std::sin(angle), std::cos(angle));
    }

    // Fraction t of p, a point of its line or of its circle, in the piece; false if p is beyond its ends by more than tol.
    bool param(const Vec2d &p, double &t, double tol = CUT_EPSILON) const
    {
        if (!arc) {
            const Vec2d  d  = b - a;
            const double l2 = d.squaredNorm();
            if (l2 <= 0.)
                return false;
            const double length = std::sqrt(l2);
            t = (p - a).dot(d) / l2;
            if (t < -tol / length || t > 1. + tol / length)
                return false;
            t = std::clamp(t, 0., 1.);
            return true;
        }
        const double span  = std::abs(sweep);
        const double phi   = wrap_angle((direction_angle(p - c) - a0) * (sweep > 0. ? 1. : -1.));
        const double slack = tol / r;
        if (phi <= span + slack) {
            t = std::min(phi / span, 1.);
            return true;
        }
        if (phi >= 2. * PI - slack) {
            t = 0.;
            return true;
        }
        return false;
    }
};

double segment_length(const Drawing &drawing, size_t idx)
{
    const Segment &segment = drawing.segments[idx];
    const Vec3d   &from    = drawing.segment_start(idx);
    const Arc      arc     = segment.type == SegmentType::Arc ? arc_through(from, segment.mid, segment.end) : Arc();
    return arc.valid ? arc.length() : (segment.end - from).norm();
}

Piece make_piece(const Drawing &drawing, size_t idx, const Frame &frame)
{
    const Segment &segment = drawing.segments[idx];
    const Vec3d   &from    = drawing.segment_start(idx);
    Piece          piece;
    piece.a = frame.flat(from);
    piece.b = frame.flat(segment.end);
    const Arc arc = segment.type == SegmentType::Arc ? arc_through(from, segment.mid, segment.end) : Arc();
    if (arc.valid) {
        piece.arc   = true;
        piece.c     = frame.flat(arc.center);
        piece.r     = arc.radius;
        piece.a0    = direction_angle(piece.a - piece.c);
        piece.sweep = arc.normal.dot(frame.n) > 0. ? arc.angle : -arc.angle;
        piece.lo    = piece.c - Vec2d::Constant(piece.r);
        piece.hi    = piece.c + Vec2d::Constant(piece.r);
    } else {
        piece.lo = piece.a.cwiseMin(piece.b);
        piece.hi = piece.a.cwiseMax(piece.b);
    }
    return piece;
}

// The segment idx lies in a plane of the normal and has a length: its level along the normal.
bool flat_level(const Drawing &drawing, size_t idx, const Frame &frame, double &level)
{
    const Segment &segment = drawing.segments[idx];
    const Vec3d   &from    = drawing.segment_start(idx);
    level = frame.level(from);
    if (std::abs(frame.level(segment.end) - level) > LEVEL_EPSILON)
        return false;
    const bool is_arc = segment.type == SegmentType::Arc && arc_through(from, segment.mid, segment.end).valid;
    if (is_arc && std::abs(frame.level(segment.mid) - level) > LEVEL_EPSILON)
        return false;
    return is_arc || (segment.end - from).norm() >= MIN_LENGTH;
}

// The segment idx lies in the plane at this level.
bool on_level(const Drawing &drawing, size_t idx, const Frame &frame, double level)
{
    double h = 0.;
    return flat_level(drawing, idx, frame, h) && std::abs(h - level) <= LEVEL_EPSILON;
}

// Points common to the supports of two pieces: their lines and their whole circles.
std::vector<Vec2d> support_points(const Piece &p, const Piece &q)
{
    std::vector<Vec2d> points;
    if (!p.arc && !q.arc) {
        const Vec2d  d1          = p.b - p.a;
        const Vec2d  d2          = q.b - q.a;
        const double denominator = cross2(d1, d2);
        if (std::abs(denominator) < 1e-12 * d1.norm() * d2.norm())
            return points;
        points.emplace_back(p.a + (cross2(q.a - p.a, d2) / denominator) * d1);
        return points;
    }
    if (p.arc != q.arc) {
        const Piece &line   = p.arc ? q : p;
        const Piece &disc   = p.arc ? p : q;
        const Vec2d  d      = line.b - line.a;
        const Vec2d  f      = line.a - disc.c;
        const double a      = d.squaredNorm();
        if (a <= 0.)
            return points;
        const double b    = 2. * d.dot(f);
        const double c    = f.squaredNorm() - disc.r * disc.r;
        double       discriminant = b * b - 4. * a * c;
        if (discriminant < 0.) {
            // Touching, within CUT_EPSILON.
            if (std::abs(cross2(d, disc.c - line.a)) / std::sqrt(a) - disc.r > CUT_EPSILON)
                return points;
            discriminant = 0.;
        }
        const double root = std::sqrt(discriminant);
        points.emplace_back(line.a + ((-b - root) / (2. * a)) * d);
        if (root > 0.)
            points.emplace_back(line.a + ((-b + root) / (2. * a)) * d);
        return points;
    }
    const Vec2d  dc       = q.c - p.c;
    const double distance = dc.norm();
    if (distance < 1e-12 || distance > p.r + q.r + CUT_EPSILON || distance < std::abs(p.r - q.r) - CUT_EPSILON)
        return points;
    const double a    = (p.r * p.r - q.r * q.r + distance * distance) / (2. * distance);
    const double h    = std::sqrt(std::max(p.r * p.r - a * a, 0.));
    const Vec2d  u    = dc / distance;
    const Vec2d  base = p.c + a * u;
    points.emplace_back(base + h * perp(u));
    if (h > 1e-9)
        points.emplace_back(base - h * perp(u));
    return points;
}

// A point where two pieces cross: its fraction on each.
struct Crossing
{
    double t;
    double u;
};

std::vector<Crossing> intersect(const Piece &p, const Piece &q)
{
    std::vector<Crossing> crossings;
    if ((p.hi.array() < q.lo.array() - CUT_EPSILON).any() || (q.hi.array() < p.lo.array() - CUT_EPSILON).any())
        return crossings;
    for (const Vec2d &point : support_points(p, q)) {
        double t = 0.;
        double u = 0.;
        if (p.param(point, t) && q.param(point, u))
            crossings.push_back({ t, u });
    }
    return crossings;
}

bool smooth_junction(const Drawing &drawing, size_t i, size_t j)
{
    const Vec3d a = end_tangent(drawing, i);
    const Vec3d b = start_tangent(drawing, j);
    return a.norm() > 0.5 && b.norm() > 0.5 && a.dot(b) > std::cos(SMOOTH_ANGLE);
}

// Point at the position s along the curve: s = k + t for the fraction t of its segment k, from its start again beyond
// its end when it is closed.
Vec3d curve_point(const Drawing &drawing, const Curve &curve, double s)
{
    const double n = double(curve.size());
    if (curve.closed) {
        s = std::fmod(s, n);
        if (s < 0.)
            s += n;
    }
    const size_t k = std::min(size_t(std::max(std::floor(s), 0.)), curve.size() - 1);
    return segment_point(drawing, curve.first + k, s - double(k));
}

// The curve from the position s0 (at least 0) to s1, beyond its end from its start again when it is closed.
Path curve_part(const Drawing &drawing, const Curve &curve, double s0, double s1)
{
    const size_t n = curve.size();
    Path         path;
    path.start = curve_point(drawing, curve, s0);
    double s   = std::max(s0, 0.);
    // Once around a closed curve at most.
    for (size_t guard = 0; s < s1 - 1e-12 && guard < 2 * n + 2; ++ guard) {
        double k  = std::floor(s);
        double t0 = s - k;
        if (t0 > 1. - 1e-12) {
            k += 1.;
            t0 = 0.;
        }
        const double end  = std::min(s1, k + 1.);
        const size_t idx  = curve.first + size_t(k) % n;
        Segment      part = segment_part(drawing, idx, t0, end - k);
        const Vec3d &from = path.segments.empty() ? path.start : path.segments.back().end;
        if (part.type == SegmentType::Arc || (part.end - from).norm() >= MIN_LENGTH)
            path.segments.push_back(part);
        else if (!path.segments.empty())
            // Too short: the previous piece goes up to its end.
            path.segments.back().end = part.end;
        s = end;
    }
    return path;
}

// Positions along the curve where the printed segments of its plane cross it, and where it crosses itself; sorted, in
// [0, size) for a closed curve.
std::vector<double> curve_cuts(const Drawing &drawing, const Curve &curve, const Vec3d &normal)
{
    const Frame  frame(normal);
    const size_t n     = curve.size();
    double       level = 0.;
    flat_level(drawing, curve.first, frame, level);
    std::vector<Piece> mine;
    mine.reserve(n);
    // Its box: the segments out of it don't cross it.
    Vec2d lo = Vec2d::Constant(std::numeric_limits<double>::max());
    Vec2d hi = -lo;
    for (size_t k = 0; k < n; ++ k) {
        mine.push_back(make_piece(drawing, curve.first + k, frame));
        lo = lo.cwiseMin(mine.back().lo);
        hi = hi.cwiseMax(mine.back().hi);
    }
    std::vector<double> cuts;
    for (size_t j = 0; j < drawing.segments.size(); ++ j) {
        if ((j >= curve.first && j <= curve.last) || !drawing.segments[j].extrude)
            continue;
        // Cheap test first: its ends far out of the box on the same side.
        const Vec2d a = frame.flat(drawing.segment_start(j));
        const Vec2d b = frame.flat(drawing.segments[j].end);
        if (drawing.segments[j].type == SegmentType::Line &&
            ((a.array() < lo.array() - CUT_EPSILON) && (b.array() < lo.array() - CUT_EPSILON)).any())
            continue;
        if (drawing.segments[j].type == SegmentType::Line &&
            ((a.array() > hi.array() + CUT_EPSILON) && (b.array() > hi.array() + CUT_EPSILON)).any())
            continue;
        if (!on_level(drawing, j, frame, level))
            continue;
        const Piece other = make_piece(drawing, j, frame);
        if ((other.hi.array() < lo.array() - CUT_EPSILON).any() || (hi.array() < other.lo.array() - CUT_EPSILON).any())
            continue;
        for (size_t k = 0; k < n; ++ k)
            for (const Crossing &crossing : intersect(mine[k], other))
                cuts.push_back(double(k) + crossing.t);
    }
    // The curve crossing itself: its pieces which don't follow each other.
    for (size_t k1 = 0; k1 < n; ++ k1)
        for (size_t k2 = k1 + 2; k2 < n; ++ k2) {
            if (curve.closed && k1 == 0 && k2 + 1 == n)
                continue;
            for (const Crossing &crossing : intersect(mine[k1], mine[k2])) {
                cuts.push_back(double(k1) + crossing.t);
                cuts.push_back(double(k2) + crossing.u);
            }
        }
    if (curve.closed)
        for (double &cut : cuts)
            if (cut >= double(n) - 1e-9)
                cut = 0.;
    std::sort(cuts.begin(), cuts.end());
    std::vector<double> unique;
    for (const double cut : cuts)
        if (unique.empty() || cut - unique.back() > 1e-9)
            unique.push_back(cut);
    return unique;
}

// The part of the curve around the position s between its cuts: from lo to hi, through its start when it is closed and
// lo < 0 or hi > its size. Without cuts: the whole curve.
void part_around(const Curve &curve, const std::vector<double> &cuts, double s, double &lo, double &hi)
{
    const double n = double(curve.size());
    if (!curve.closed) {
        lo = 0.;
        hi = n;
        for (const double cut : cuts) {
            if (cut <= 1e-6 || cut >= n - 1e-6)
                continue;
            if (cut <= s)
                lo = std::max(lo, cut);
            else
                hi = std::min(hi, cut);
        }
        return;
    }
    if (cuts.empty()) {
        lo = 0.;
        hi = n;
        return;
    }
    bool   has_below = false;
    bool   has_above = false;
    double below     = 0.;
    double above     = 0.;
    for (const double cut : cuts) {
        if (cut <= s) {
            below     = has_below ? std::max(below, cut) : cut;
            has_below = true;
        } else {
            above     = has_above ? std::min(above, cut) : cut;
            has_above = true;
        }
    }
    // The cuts are sorted.
    lo = has_below ? below : cuts.back() - n;
    hi = has_above ? above : cuts.front() + n;
}

// Trim: the curve, the parts of it kept, and the part removed (from lo, in [0, size), to hi).
struct TrimPlan
{
    Curve                                  curve;
    std::vector<std::pair<double, double>> keep;
    double                                 lo { 0. };
    double                                 hi { 0. };
};

bool trim_plan(const Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal, TrimPlan &plan)
{
    if (!find_curve(drawing, idx, normal, plan.curve))
        return false;
    const Curve               &curve = plan.curve;
    const double               n     = double(curve.size());
    const std::vector<double>  cuts  = curve_cuts(drawing, curve, normal);
    const double               s     = double(idx - curve.first) + segment_fraction(drawing, idx, at);
    part_around(curve, cuts, s, plan.lo, plan.hi);
    if (plan.hi - plan.lo < 1e-9)
        return false;
    plan.keep.clear();
    if (!curve.closed) {
        if (plan.lo > 1e-9)
            plan.keep.emplace_back(0., plan.lo);
        if (plan.hi < n - 1e-9)
            plan.keep.emplace_back(plan.hi, n);
    } else if (cuts.size() >= 2) {
        // From the cut after the part around to the one before it.
        double s0 = plan.hi;
        double s1 = plan.lo + n;
        if (s0 >= n) {
            s0 -= n;
            s1 -= n;
        }
        plan.keep.emplace_back(s0, s1);
    }
    if (plan.lo < 0.) {
        plan.lo += n;
        plan.hi += n;
    }
    return true;
}

// Break: the curve and the positions where it is split, inside its segments.
bool break_plan(const Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal, Curve &curve, std::vector<double> &positions)
{
    positions.clear();
    if (!find_curve(drawing, idx, normal, curve))
        return false;
    const double              n    = double(curve.size());
    const std::vector<double> cuts = curve_cuts(drawing, curve, normal);
    const double              s    = double(idx - curve.first) + segment_fraction(drawing, idx, at);
    double                    lo   = 0.;
    double                    hi   = 0.;
    part_around(curve, cuts, s, lo, hi);
    bool crossed = curve.closed && !cuts.empty();
    for (const double cut : cuts)
        if (cut > 1e-6 && cut < n - 1e-6)
            crossed = true;
    std::vector<double> candidates;
    if (crossed) {
        for (double p : { lo, hi }) {
            if (curve.closed) {
                p = std::fmod(p, n);
                if (p < 0.)
                    p += n;
            }
            candidates.push_back(p);
        }
    } else
        candidates.push_back(s);
    std::sort(candidates.begin(), candidates.end());
    for (const double p : candidates) {
        const double k = std::floor(p);
        const double t = p - k;
        if (k >= n)
            continue;
        // Inside a segment, its two parts long enough: its ends already split the curve.
        const double length = segment_length(drawing, curve.first + size_t(k));
        if (t * length >= MIN_LENGTH && (1. - t) * length >= MIN_LENGTH && (positions.empty() || p - positions.back() > 1e-9))
            positions.push_back(p);
    }
    return !positions.empty();
}

// Extend: the segment whose end goes on (its start if !at_end), to the point; for an arc, by the angle delta.
struct ExtendPlan
{
    size_t segment { 0 };
    bool   at_end  { true };
    Vec3d  point   { Vec3d::Zero() };
    double delta   { 0. };
};

bool extend_plan(const Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal, ExtendPlan &plan)
{
    Curve curve;
    if (!find_curve(drawing, idx, normal, curve) || curve.closed)
        return false;
    const std::vector<Segment> &segments = drawing.segments;
    const Frame                 frame(normal);
    const bool                  extrude  = segments[curve.first].extrude;
    // A free end: a travel move, or nothing, comes after it (before it for its start).
    auto free_end = [&](bool end) {
        return end ? (curve.last + 1 == segments.size() || segments[curve.last + 1].extrude != extrude) :
                     (curve.first == 0 || segments[curve.first - 1].extrude != extrude);
    };
    // The end the closest to the click, otherwise the other one.
    const bool end_first = (segments[curve.last].end - at).norm() <= (drawing.segment_start(curve.first) - at).norm();
    bool       at_end    = end_first;
    if (!free_end(end_first)) {
        if (!free_end(!end_first))
            return false;
        at_end = !end_first;
    }
    const size_t k = at_end ? curve.last : curve.first;
    double level = 0.;
    flat_level(drawing, k, frame, level);
    const Piece piece = make_piece(drawing, k, frame);
    // What it can meet: the printed segments of its plane, but itself.
    std::vector<Piece> others;
    for (size_t j = 0; j < segments.size(); ++ j)
        if (j != k && segments[j].extrude && on_level(drawing, j, frame, level))
            others.push_back(make_piece(drawing, j, frame));
    bool   found = false;
    double best  = 0.;
    Vec2d  best_point(Vec2d::Zero());
    const Vec2d origin = at_end ? piece.b : piece.a;
    if (!piece.arc) {
        // Straight on.
        const Vec2d direction = at_end ? piece.tangent(1.) : Vec2d(-piece.tangent(0.));
        Piece       ray;
        ray.a = origin;
        ray.b = origin + direction;
        for (const Piece &other : others)
            for (const Vec2d &p : support_points(ray, other)) {
                double       u     = 0.;
                const double along = (p - origin).dot(direction);
                if (along > CUT_EPSILON && other.param(p, u) && (!found || along < best)) {
                    found      = true;
                    best       = along;
                    best_point = p;
                }
            }
    } else {
        // Along its circle, before it closes.
        const double way   = (piece.sweep > 0. ? 1. : -1.) * (at_end ? 1. : -1.);
        const double room  = 2. * PI - std::abs(piece.sweep);
        const double from  = direction_angle(origin - piece.c);
        Piece        disc;
        disc.arc = true;
        disc.c   = piece.c;
        disc.r   = piece.r;
        for (const Piece &other : others)
            for (const Vec2d &p : support_points(disc, other)) {
                double       u     = 0.;
                const double delta = wrap_angle((direction_angle(p - piece.c) - from) * way);
                if (other.param(p, u) && delta > CUT_EPSILON / piece.r && delta < room - 1e-6 && (!found || delta < best)) {
                    found      = true;
                    best       = delta;
                    best_point = p;
                }
            }
    }
    if (!found)
        return false;
    plan.segment = k;
    plan.at_end  = at_end;
    plan.point   = frame.back(best_point, level);
    plan.delta   = piece.arc ? best : 0.;
    return true;
}

// Fillet and chamfer: the segment in cut back to its fraction t_in, then the segments joining it to out, cut from its
// fraction t_out.
struct CornerPlan
{
    double               t_in  { 1. };
    std::vector<Segment> join;
    double               t_out { 0. };
};

// The ends of a corner, flat in the same plane: their pieces and their level.
bool corner_pieces(const Drawing &drawing, const Corner &corner, const Frame &frame, Piece &a, Piece &b, double &level)
{
    if (!drawing.has_start || corner.in >= drawing.segments.size() || corner.out >= drawing.segments.size() || corner.in == corner.out)
        return false;
    double h = 0.;
    if (!flat_level(drawing, corner.in, frame, level) || !flat_level(drawing, corner.out, frame, h) || std::abs(h - level) > LEVEL_EPSILON)
        return false;
    a = make_piece(drawing, corner.in, frame);
    b = make_piece(drawing, corner.out, frame);
    return true;
}

// The line or the circle of a piece moved by distance on both sides.
std::vector<Piece> offset_supports(const Piece &piece, double distance)
{
    std::vector<Piece> supports;
    for (const double side : { 1., -1. }) {
        Piece support;
        if (!piece.arc) {
            const Vec2d d = (piece.b - piece.a).normalized();
            support.a     = piece.a + side * distance * perp(d);
            support.b     = support.a + d;
        } else {
            support.arc = true;
            support.c   = piece.c;
            support.r   = piece.r + side * distance;
            if (support.r <= MIN_LENGTH)
                continue;
        }
        supports.push_back(support);
    }
    return supports;
}

// Point of the line or of the circle of a piece the closest to p.
Vec2d foot(const Piece &piece, const Vec2d &p)
{
    if (!piece.arc) {
        const Vec2d d = piece.b - piece.a;
        return piece.a + ((p - piece.a).dot(d) / d.squaredNorm()) * d;
    }
    const Vec2d q = p - piece.c;
    return q.norm() > 0. ? Vec2d(piece.c + (piece.r / q.norm()) * q) : Vec2d(piece.c + Vec2d(piece.r, 0.));
}

bool fillet_plan(const Drawing &drawing, const Corner &corner, double radius, const Vec3d &normal, CornerPlan &plan)
{
    const Frame frame(normal);
    Piece       a;
    Piece       b;
    double      level = 0.;
    if (radius < MIN_LENGTH || !corner_pieces(drawing, corner, frame, a, b, level))
        return false;
    const Vec2d corner_at = a.b;
    // Centers at the radius from both: on the lines or the circles moved by the radius.
    bool   found = false;
    double best  = 0.;
    double best_in = 0., best_out = 0., best_sweep = 0.;
    Vec2d  best_center(Vec2d::Zero());
    for (const Piece &support_a : offset_supports(a, radius))
        for (const Piece &support_b : offset_supports(b, radius))
            for (const Vec2d &center : support_points(support_a, support_b)) {
                const Vec2d touch_a = foot(a, center);
                const Vec2d touch_b = foot(b, center);
                double      t_in    = 0.;
                double      t_out   = 0.;
                if (!a.param(touch_a, t_in, 1e-7) || !b.param(touch_b, t_out, 1e-7))
                    continue;
                if (std::abs((touch_a - center).norm() - radius) > 1e-6 || std::abs((touch_b - center).norm() - radius) > 1e-6)
                    continue;
                // Leaving a along it, reaching b along it.
                const double way = perp(touch_a - center).dot(a.tangent(t_in)) > 0. ? 1. : -1.;
                if ((way * perp(touch_b - center) / radius).dot(b.tangent(t_out)) < 0.999)
                    continue;
                const double sweep = wrap_angle((direction_angle(touch_b - center) - direction_angle(touch_a - center)) * way);
                if (sweep < 1e-9 || sweep > PI + 1e-6)
                    continue;
                const double score = (touch_a - corner_at).norm() + (touch_b - corner_at).norm();
                if (!found || score < best) {
                    found       = true;
                    best        = score;
                    best_in     = t_in;
                    best_out    = t_out;
                    best_center = center;
                    best_sweep  = way * sweep;
                }
            }
    if (!found)
        return false;
    const Segment &model  = drawing.segments[corner.in];
    const double   from   = direction_angle(a.point(best_in) - best_center);
    const double   middle = from + 0.5 * best_sweep;
    Segment        arc    = model;
    arc.type = SegmentType::Arc;
    arc.mid  = frame.back(Vec2d(best_center + radius * Vec2d(std::cos(middle), std::sin(middle))), level);
    arc.end  = frame.back(b.point(best_out), level);
    plan.t_in  = best_in;
    plan.t_out = best_out;
    plan.join  = { arc };
    return true;
}

bool chamfer_plan(const Drawing &drawing, const Corner &corner, double distance1, double distance2, double angle, const Vec3d &normal,
                  CornerPlan &plan)
{
    const Frame frame(normal);
    Piece       a;
    Piece       b;
    double      level = 0.;
    // Between two lines.
    if (!corner_pieces(drawing, corner, frame, a, b, level) || a.arc || b.arc)
        return false;
    const double length_a = (a.b - a.a).norm();
    const double length_b = (b.b - b.a).norm();
    const Vec2d  da       = (a.b - a.a) / length_a;
    const Vec2d  db       = (b.b - b.a) / length_b;
    const Vec2d  corner_at = a.b;
    if (distance1 < MIN_LENGTH || distance1 > length_a + 1e-9)
        return false;
    const Vec2d touch_a = corner_at - distance1 * da;
    double      along_b = distance2;
    if (angle > 0.) {
        // Turning from a the way the corner turns.
        const double side        = cross2(da, db) > 0. ? 1. : -1.;
        const Vec2d  w           = std::cos(angle) * da + side * std::sin(angle) * perp(da);
        const double denominator = cross2(w, db);
        if (std::abs(denominator) < 1e-12)
            return false;
        const double lambda = cross2(corner_at - touch_a, db) / denominator;
        along_b             = cross2(corner_at - touch_a, w) / denominator;
        if (lambda <= MIN_LENGTH)
            return false;
    }
    if (along_b < MIN_LENGTH || along_b > length_b + 1e-9)
        return false;
    Segment line = drawing.segments[corner.in];
    line.type    = SegmentType::Line;
    line.end     = frame.back(Vec2d(corner_at + along_b * db), level);
    plan.t_in    = std::max(1. - distance1 / length_a, 0.);
    plan.t_out   = std::min(along_b / length_b, 1.);
    plan.join    = { line };
    return true;
}

// The corner replaced by the plan.
void apply_corner(Drawing &drawing, const Corner &corner, const CornerPlan &plan)
{
    std::vector<Segment> &segments  = drawing.segments;
    // Parts shorter than MIN_LENGTH go.
    const bool            keep_in   = plan.t_in * segment_length(drawing, corner.in) >= MIN_LENGTH;
    const bool            keep_out  = (1. - plan.t_out) * segment_length(drawing, corner.out) >= MIN_LENGTH;
    const Segment         in_part   = segment_part(drawing, corner.in, 0., plan.t_in);
    const Segment         out_part  = segment_part(drawing, corner.out, plan.t_out, 1.);
    const Vec3d           out_start = segment_point(drawing, corner.out, plan.t_out);
    std::vector<Segment>  pieces;
    if (keep_in)
        pieces.push_back(in_part);
    pieces.insert(pieces.end(), plan.join.begin(), plan.join.end());
    if (corner.out == corner.in + 1) {
        if (keep_out)
            pieces.push_back(out_part);
        segments.erase(segments.begin() + corner.in, segments.begin() + corner.out + 1);
        segments.insert(segments.begin() + corner.in, pieces.begin(), pieces.end());
        return;
    }
    // Corner closing a loop: in is its last segment, out its first one, before it.
    const bool extrude = segments[corner.in].extrude;
    segments.erase(segments.begin() + corner.in);
    segments.insert(segments.begin() + corner.in, pieces.begin(), pieces.end());
    const Vec3d new_start = keep_out ? out_start : segments[corner.out].end;
    if (keep_out)
        segments[corner.out] = out_part;
    else
        segments.erase(segments.begin() + corner.out);
    if (corner.out > 0 && segments[corner.out - 1].extrude == extrude) {
        // A printed segment ends where the loop started: it stays, a travel move leads from it to the new start.
        Segment travel;
        travel.extrude = false;
        travel.end     = new_start;
        segments.insert(segments.begin() + corner.out, travel);
    } else if (corner.out == 0)
        drawing.start = new_start;
    else
        segments[corner.out - 1].end = new_start;
}

// Offset of a piece: its line moved, or its circle grown or shrunk; then its ends, where it meets the next ones.
struct OffsetPiece
{
    Piece  original;
    size_t index     { 0 };
    // Line: a point of it and its direction; arc: its center, radius and way (1 counterclockwise).
    Vec2d  point     { Vec2d::Zero() };
    Vec2d  direction { Vec2d::Zero() };
    double radius    { 0. };
    double way       { 1. };
    Vec2d  raw_start { Vec2d::Zero() };
    Vec2d  raw_end   { Vec2d::Zero() };
    Vec2d  start     { Vec2d::Zero() };
    Vec2d  end       { Vec2d::Zero() };
    bool   valid     { true };

    OffsetPiece(const Piece &piece, double distance, size_t idx) : original(piece), index(idx)
    {
        if (!piece.arc) {
            direction = (piece.b - piece.a).normalized();
            point     = piece.a + distance * perp(direction);
            raw_start = point;
            raw_end   = piece.b + distance * perp(direction);
        } else {
            // Its left is its center when it turns counterclockwise.
            way             = piece.sweep > 0. ? 1. : -1.;
            radius          = piece.r - distance * way;
            valid           = radius > MIN_LENGTH;
            const double to = piece.a0 + piece.sweep;
            raw_start       = piece.c + radius * Vec2d(std::cos(piece.a0), std::sin(piece.a0));
            raw_end         = piece.c + radius * Vec2d(std::cos(to), std::sin(to));
        }
        start = raw_start;
        end   = raw_end;
    }

    Piece support() const
    {
        Piece result;
        if (!original.arc) {
            result.a = point;
            result.b = point + direction;
        } else {
            result.arc = true;
            result.c   = original.c;
            result.r   = radius;
        }
        return result;
    }

    // Arc: the angle from its start to its end, its way.
    double span() const { return wrap_angle((direction_angle(end - original.c) - direction_angle(start - original.c)) * way); }

    bool full_circle() const { return original.arc && std::abs(original.sweep) > 2. * PI - 1e-6; }

    // Turned back by its neighbours, or without length.
    bool degenerate() const
    {
        if (!original.arc) {
            const Vec2d v = end - start;
            return v.norm() < MIN_LENGTH || v.dot(direction) <= 0.;
        }
        if (full_circle())
            return false;
        const double angle = span();
        return angle * radius < MIN_LENGTH || std::abs(angle - std::abs(original.sweep)) > PI;
    }
};

} // namespace

bool find_curve(const Drawing &drawing, size_t idx, const Vec3d &normal, Curve &curve)
{
    if (!drawing.has_start || idx >= drawing.segments.size())
        return false;
    const Frame frame(normal);
    double      level = 0.;
    if (!flat_level(drawing, idx, frame, level))
        return false;
    const bool extrude = drawing.segments[idx].extrude;
    auto joins  = [&](size_t k) { return drawing.segments[k].extrude == extrude && on_level(drawing, k, frame, level); };
    auto is_arc = [&drawing](size_t k) {
        const Segment &segment = drawing.segments[k];
        return segment.type == SegmentType::Arc && arc_through(drawing.segment_start(k), segment.mid, segment.end).valid;
    };
    // Arcs going on from one another without a corner: one circle, ellipse, spline...
    auto goes_on = [&](size_t i, size_t j) { return is_arc(i) && is_arc(j) && smooth_junction(drawing, i, j); };
    curve.first = idx;
    curve.last  = idx;
    while (curve.first > 0 && joins(curve.first - 1) && goes_on(curve.first - 1, curve.first))
        -- curve.first;
    while (curve.last + 1 < drawing.segments.size() && joins(curve.last + 1) && goes_on(curve.last, curve.last + 1))
        ++ curve.last;
    curve.closed = (drawing.segments[curve.last].end - drawing.segment_start(curve.first)).norm() < MIN_LENGTH &&
                   goes_on(curve.last, curve.first);
    return true;
}

bool find_loop(const Drawing &drawing, size_t idx, size_t &first, size_t &last)
{
    if (!drawing.has_start || idx >= drawing.segments.size())
        return false;
    size_t path_first = idx;
    size_t path_last  = idx;
    find_path(drawing, idx, path_first, path_last);
    // The starts of the segments up to it by cells of MIN_LENGTH, sorted: an end meets only the starts of the cells around
    // its own (called at each move of the mouse, on paths of thousands of segments).
    using Cell   = std::array<int64_t, 3>;
    auto cell_of = [](const Vec3d &p) {
        Cell cell;
        for (int axis = 0; axis < 3; ++ axis)
            cell[axis] = int64_t(std::clamp(std::floor(p[axis] / MIN_LENGTH), -1e15, 1e15));
        return cell;
    };
    std::vector<std::pair<Cell, size_t>> starts;
    starts.reserve(idx - path_first + 1);
    for (size_t j = path_first; j <= idx; ++ j)
        starts.emplace_back(cell_of(drawing.segment_start(j)), j);
    std::sort(starts.begin(), starts.end());
    // The first segment after it ending where a segment before it starts, the closest one before it.
    for (size_t l = idx; l <= path_last; ++ l) {
        const Vec3d &end    = drawing.segments[l].end;
        const Cell   center = cell_of(end);
        bool         found  = false;
        for (int64_t dx = -1; dx <= 1; ++ dx)
            for (int64_t dy = -1; dy <= 1; ++ dy)
                for (int64_t dz = -1; dz <= 1; ++ dz) {
                    const Cell cell { center[0] + dx, center[1] + dy, center[2] + dz };
                    for (auto it = std::lower_bound(starts.begin(), starts.end(), std::make_pair(cell, size_t(0)));
                         it != starts.end() && it->first == cell; ++ it) {
                        const size_t j = it->second;
                        if ((!found || j > first) && (end - drawing.segment_start(j)).norm() < MIN_LENGTH &&
                            (l > j || segment_length(drawing, j) >= MIN_LENGTH)) {
                            found = true;
                            first = j;
                        }
                    }
                }
        if (found) {
            last = l;
            return true;
        }
    }
    return false;
}

bool trim_part(const Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal, Path &removed)
{
    TrimPlan plan;
    if (!trim_plan(drawing, idx, at, normal, plan))
        return false;
    removed = curve_part(drawing, plan.curve, plan.lo, plan.hi);
    return true;
}

bool trim(Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal)
{
    TrimPlan plan;
    if (!trim_plan(drawing, idx, at, normal, plan))
        return false;
    std::vector<Path> kept;
    for (const auto &[s0, s1] : plan.keep)
        kept.push_back(curve_part(drawing, plan.curve, s0, s1));
    replace_segments(drawing, plan.curve.first, plan.curve.last, kept);
    return true;
}

bool break_points(const Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal, std::vector<Vec3d> &points)
{
    Curve               curve;
    std::vector<double> positions;
    points.clear();
    if (!break_plan(drawing, idx, at, normal, curve, positions))
        return false;
    for (const double p : positions)
        points.push_back(curve_point(drawing, curve, p));
    return true;
}

bool break_curve(Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal)
{
    Curve               curve;
    std::vector<double> positions;
    if (!break_plan(drawing, idx, at, normal, curve, positions))
        return false;
    // By segment, from the last one (the segments before it keep their index): each one split once, into all its parts,
    // from the original.
    for (size_t i = positions.size(); i > 0;) {
        const size_t        k = size_t(std::floor(positions[i - 1]));
        std::vector<double> fractions { 1. };
        while (i > 0 && size_t(std::floor(positions[i - 1])) == k) {
            fractions.push_back(positions[i - 1] - double(k));
            -- i;
        }
        fractions.push_back(0.);
        std::reverse(fractions.begin(), fractions.end());
        const size_t         split = curve.first + k;
        std::vector<Segment> parts;
        for (size_t p = 0; p + 1 < fractions.size(); ++ p)
            parts.push_back(segment_part(drawing, split, fractions[p], fractions[p + 1]));
        drawing.segments.erase(drawing.segments.begin() + split);
        drawing.segments.insert(drawing.segments.begin() + split, parts.begin(), parts.end());
    }
    return true;
}

bool extend_part(const Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal, Path &extension)
{
    ExtendPlan plan;
    if (!extend_plan(drawing, idx, at, normal, plan))
        return false;
    const Segment &segment = drawing.segments[plan.segment];
    const Vec3d   &from    = drawing.segment_start(plan.segment);
    extension              = Path();
    extension.start        = plan.at_end ? segment.end : from;
    Segment piece          = segment;
    piece.end              = plan.point;
    piece.type             = SegmentType::Line;
    if (plan.delta > 0.) {
        const Arc arc = arc_through(from, segment.mid, segment.end);
        if (arc.valid) {
            // Along the circle, beyond the end or before the start.
            piece.type = SegmentType::Arc;
            piece.mid  = turn(arc.center, extension.start, (plan.at_end ? 0.5 : -0.5) * plan.delta, arc.normal);
            if (!arc_through(extension.start, piece.mid, piece.end).valid)
                piece.type = SegmentType::Line;
        }
    }
    extension.segments.push_back(piece);
    return true;
}

bool extend(Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal)
{
    ExtendPlan plan;
    if (!extend_plan(drawing, idx, at, normal, plan))
        return false;
    Segment &segment = drawing.segments[plan.segment];
    Vec3d   &start   = plan.segment == 0 ? drawing.start : drawing.segments[plan.segment - 1].end;
    if (plan.delta > 0.) {
        const Arc arc = arc_through(start, segment.mid, segment.end);
        if (!arc.valid)
            return false;
        const double angle = arc.angle + plan.delta;
        if (plan.at_end)
            segment.mid = turn(arc.center, arc.start, 0.5 * angle, arc.normal);
        else
            segment.mid = turn(arc.center, plan.point, 0.5 * angle, arc.normal);
    } else
        // Printed straight (an arc through aligned points too): it stays straight.
        segment.type = SegmentType::Line;
    if (plan.at_end)
        segment.end = plan.point;
    else
        start = plan.point;
    return true;
}

bool find_corner(const Drawing &drawing, size_t idx, const Vec3d &at, Corner &corner)
{
    const std::vector<Segment> &segments = drawing.segments;
    if (!drawing.has_start || idx >= segments.size())
        return false;
    const Segment &segment = segments[idx];
    const Vec3d    start   = drawing.segment_start(idx);
    // The closest end first.
    const bool end_first = (segment.end - at).norm() <= (start - at).norm();
    // The loop the segment belongs to: the corner where it closes.
    size_t     loop_first = idx;
    size_t     loop_last  = idx;
    const bool in_loop    = find_loop(drawing, idx, loop_first, loop_last) && loop_first != loop_last;
    for (const bool at_end : { end_first, !end_first }) {
        Corner candidate;
        bool   has = false;
        if (at_end) {
            if (in_loop && loop_last == idx) {
                candidate = { idx, loop_first };
                has       = true;
            } else if (idx + 1 < segments.size() && segments[idx + 1].extrude == segment.extrude) {
                candidate = { idx, idx + 1 };
                has       = true;
            }
        } else {
            if (in_loop && loop_first == idx) {
                candidate = { loop_last, idx };
                has       = true;
            } else if (idx > 0 && segments[idx - 1].extrude == segment.extrude) {
                candidate = { idx - 1, idx };
                has       = true;
            }
        }
        if (has && !smooth_junction(drawing, candidate.in, candidate.out)) {
            corner = candidate;
            return true;
        }
    }
    return false;
}

Vec3d corner_point(const Drawing &drawing, const Corner &corner)
{
    return corner.in < drawing.segments.size() ? drawing.segments[corner.in].end : drawing.last_point();
}

bool fillet_shape(const Drawing &drawing, const Corner &corner, double radius, const Vec3d &normal, Path &shape)
{
    CornerPlan plan;
    if (!fillet_plan(drawing, corner, radius, normal, plan))
        return false;
    shape          = Path();
    shape.start    = segment_point(drawing, corner.in, plan.t_in);
    shape.segments = plan.join;
    return true;
}

bool fillet(Drawing &drawing, const Corner &corner, double radius, const Vec3d &normal)
{
    CornerPlan plan;
    if (!fillet_plan(drawing, corner, radius, normal, plan))
        return false;
    apply_corner(drawing, corner, plan);
    return true;
}

bool chamfer_shape(const Drawing &drawing, const Corner &corner, double distance1, double distance2, double angle, const Vec3d &normal,
                   Path &shape)
{
    CornerPlan plan;
    if (!chamfer_plan(drawing, corner, distance1, distance2, angle, normal, plan))
        return false;
    shape          = Path();
    shape.start    = segment_point(drawing, corner.in, plan.t_in);
    shape.segments = plan.join;
    return true;
}

bool chamfer(Drawing &drawing, const Corner &corner, double distance1, double distance2, double angle, const Vec3d &normal)
{
    CornerPlan plan;
    if (!chamfer_plan(drawing, corner, distance1, distance2, angle, normal, plan))
        return false;
    apply_corner(drawing, corner, plan);
    return true;
}

Path offset_path(const Drawing &drawing, size_t first, size_t last, double distance, const Vec3d &normal)
{
    Path path;
    if (!drawing.has_start || first > last || last >= drawing.segments.size() || std::abs(distance) < MIN_LENGTH)
        return path;
    const Frame frame(normal);
    double      level = 0.;
    if (!flat_level(drawing, first, frame, level))
        return path;
    std::vector<OffsetPiece> pieces;
    for (size_t k = first; k <= last; ++ k) {
        if (!on_level(drawing, k, frame, level))
            return path;
        OffsetPiece piece(make_piece(drawing, k, frame), distance, k);
        // An arc shrinking to nothing goes.
        if (piece.valid)
            pieces.push_back(piece);
    }
    const bool closed = (drawing.segments[last].end - drawing.segment_start(first)).norm() < MIN_LENGTH;
    // Rounded corners: after the piece of this rank, around this corner, turning this way.
    std::vector<std::pair<size_t, std::pair<Vec2d, double>>> rounds;
    bool stable = false;
    for (size_t pass = 0; pass < 2 * pieces.size() + 2 && !pieces.empty(); ++ pass) {
        rounds.clear();
        for (OffsetPiece &piece : pieces) {
            piece.start = piece.raw_start;
            piece.end   = piece.raw_end;
        }
        const size_t m = pieces.size();
        for (size_t k = 0; k < m; ++ k) {
            if (k + 1 == m && !(closed && m >= 2))
                break;
            OffsetPiece &a  = pieces[k];
            OffsetPiece &b  = pieces[(k + 1) % m];
            const Vec2d  ta = a.original.tangent(1.);
            const Vec2d  tb = b.original.tangent(0.);
            if (ta.dot(tb) > std::cos(SMOOTH_ANGLE) && (a.raw_end - b.raw_start).norm() < SMOOTH_GAP) {
                // Going on smoothly: they meet in the middle.
                const Vec2d middle = 0.5 * (a.raw_end + b.raw_start);
                a.end   = middle;
                b.start = middle;
                continue;
            }
            const Vec2d  corner    = a.original.b;
            const bool   turn_left = cross2(ta, tb) > 0.;
            // The offset is on the inside of the turn: the pieces cross; outside, they are extended to meet.
            const bool   inside = turn_left == (distance > 0.);
            const Vec2d  middle = 0.5 * (a.raw_end + b.raw_start);
            bool         met    = false;
            Vec2d        meet   = middle;
            for (const Vec2d &p : support_points(a.support(), b.support()))
                if (!met || (p - middle).norm() < (meet - middle).norm()) {
                    meet = p;
                    met  = true;
                }
            if (met && !inside && (meet - corner).norm() > MITER_LIMIT * std::abs(distance))
                met = false;
            if (met || inside) {
                a.end   = meet;
                b.start = meet;
            } else
                rounds.push_back({ k, { corner, turn_left ? 1. : -1. } });
        }
        // The pieces turned back by their neighbours go, their neighbours meet.
        std::vector<OffsetPiece> kept;
        for (const OffsetPiece &piece : pieces)
            if (!piece.degenerate())
                kept.push_back(piece);
        if (kept.size() == pieces.size()) {
            stable = true;
            break;
        }
        pieces = std::move(kept);
    }
    if (!stable || pieces.empty())
        return path;
    if (closed && pieces.size() == 1 && !pieces.front().full_circle())
        return path;

    path.start = frame.back(pieces.front().start, level);
    for (size_t k = 0; k < pieces.size(); ++ k) {
        const OffsetPiece &piece   = pieces[k];
        Segment            segment = drawing.segments[piece.index];
        segment.end                = frame.back(piece.end, level);
        segment.type               = SegmentType::Line;
        if (piece.original.arc) {
            const double angle  = piece.full_circle() && pieces.size() == 1 ? 2. * PI : piece.span();
            const double middle = direction_angle(piece.start - piece.original.c) + 0.5 * angle * piece.way;
            segment.type = SegmentType::Arc;
            segment.mid  = frame.back(Vec2d(piece.original.c + piece.radius * Vec2d(std::cos(middle), std::sin(middle))), level);
        }
        path.segments.push_back(segment);
        for (const auto &[rank, rounded] : rounds) {
            if (rank != k)
                continue;
            // Round corner around the corner of the path, from this piece to the next one.
            const OffsetPiece &next   = pieces[(k + 1) % pieces.size()];
            const Vec2d       &center = rounded.first;
            const double       way    = rounded.second;
            const Vec2d        from   = piece.end - center;
            const double       angle  = wrap_angle((direction_angle(next.start - center) - direction_angle(from)) * way);
            const double       middle = direction_angle(from) + 0.5 * angle * way;
            Segment            arc    = drawing.segments[piece.index];
            arc.type = SegmentType::Arc;
            arc.mid  = frame.back(Vec2d(center + from.norm() * Vec2d(std::cos(middle), std::sin(middle))), level);
            arc.end  = frame.back(next.start, level);
            path.segments.push_back(arc);
        }
    }
    // Arcs too flat to be told from lines: lines.
    Vec3d from = path.start;
    for (Segment &segment : path.segments) {
        if (segment.type == SegmentType::Arc && !arc_through(from, segment.mid, segment.end).valid)
            segment.type = SegmentType::Line;
        from = segment.end;
    }
    return path;
}

bool distance_to_path(const Drawing &drawing, size_t first, size_t last, const Vec3d &p, const Vec3d &normal, double &distance)
{
    if (!drawing.has_start || first > last || last >= drawing.segments.size())
        return false;
    const Frame frame(normal);
    double      level = 0.;
    if (!flat_level(drawing, first, frame, level))
        return false;
    const Vec2d        q = frame.flat(p);
    std::vector<Piece> pieces;
    for (size_t k = first; k <= last; ++ k) {
        if (!on_level(drawing, k, frame, level))
            return false;
        pieces.push_back(make_piece(drawing, k, frame));
    }
    const bool closed = (drawing.segments[last].end - drawing.segment_start(first)).norm() < MIN_LENGTH;
    // The closest point of the path.
    double best   = std::numeric_limits<double>::max();
    size_t best_k = 0;
    double best_t = 0.;
    for (size_t k = 0; k < pieces.size(); ++ k) {
        const Piece &piece = pieces[k];
        double       t     = 0.;
        if (!piece.arc) {
            const Vec2d d = piece.b - piece.a;
            t             = std::clamp((q - piece.a).dot(d) / d.squaredNorm(), 0., 1.);
        } else {
            const double span  = std::abs(piece.sweep);
            const double angle = wrap_angle((direction_angle(q - piece.c) - piece.a0) * (piece.sweep > 0. ? 1. : -1.));
            t = angle <= span ? angle / span : (angle - span < 2. * PI - angle ? 1. : 0.);
        }
        const double length = (q - piece.point(t)).norm();
        if (length < best) {
            best   = length;
            best_k = k;
            best_t = t;
        }
    }
    const size_t m       = pieces.size();
    const Vec2d  closest = pieces[best_k].point(best_t);
    Vec2d        along   = pieces[best_k].tangent(best_t);
    // At the vertex between two pieces: the side seen from both directions.
    if (best_t >= 1. - 1e-9 && (best_k + 1 < m || closed))
        along = pieces[best_k].tangent(1.) + pieces[(best_k + 1) % m].tangent(0.);
    else if (best_t <= 1e-9 && (best_k > 0 || closed))
        along = pieces[(best_k + m - 1) % m].tangent(1.) + pieces[best_k].tangent(0.);
    if (along.norm() < 1e-9)
        along = pieces[best_k].tangent(best_t);
    // On its left: positive.
    distance = cross2(along, q - closest) >= 0. ? best : -best;
    return true;
}

void transform_segments(Drawing &drawing, size_t first, size_t last, const Transform3d &transformation)
{
    if (!drawing.has_start || first > last || last >= drawing.segments.size())
        return;
    Vec3d      &start = first == 0 ? drawing.start : drawing.segments[first - 1].end;
    const Vec3d moved = transformation * start;
    start             = moved;
    for (size_t idx = first; idx <= last; ++ idx) {
        Segment    &segment = drawing.segments[idx];
        const Vec3d end     = transformation * segment.end;
        const Vec3d mid     = transformation * segment.mid;
        segment.end         = end;
        segment.mid         = mid;
    }
}

Path transformed_path(const Drawing &drawing, size_t first, size_t last, const Transform3d &transformation)
{
    Path path;
    if (!drawing.has_start || first > last || last >= drawing.segments.size())
        return path;
    path.start = transformation * drawing.segment_start(first);
    for (size_t idx = first; idx <= last; ++ idx) {
        Segment segment = drawing.segments[idx];
        segment.end     = transformation * drawing.segments[idx].end;
        segment.mid     = transformation * drawing.segments[idx].mid;
        path.segments.push_back(segment);
    }
    return path;
}

Transform3d mirror_transformation(const Vec3d &a, const Vec3d &direction, const Vec3d &normal)
{
    Transform3d transformation = Transform3d::Identity();
    const Vec3d n              = normal.normalized();
    const Vec3d d              = in_plane(direction, n);
    if (d.norm() < 1e-9)
        return transformation;
    // Across the plane of the line and of the normal.
    const Vec3d m                 = n.cross(d.normalized());
    transformation.linear()       = Matrix3d::Identity() - 2. * m * m.transpose();
    transformation.translation()  = 2. * m.dot(a) * m;
    return transformation;
}

Transform3d rotation_around(const Vec3d &center, double angle, const Vec3d &normal)
{
    Transform3d    transformation = Transform3d::Identity();
    const Matrix3d rotation       = Eigen::AngleAxisd(angle, normal.normalized()).toRotationMatrix();
    transformation.linear()       = rotation;
    transformation.translation()  = center - rotation * center;
    return transformation;
}

Transform3d scale_in_plane(const Vec3d &base, double factor, const Vec3d &normal)
{
    Transform3d    transformation = Transform3d::Identity();
    const Vec3d    n              = normal.normalized();
    const Matrix3d linear         = factor * Matrix3d::Identity() + (1. - factor) * n * n.transpose();
    transformation.linear()       = linear;
    transformation.translation()  = base - linear * base;
    return transformation;
}

std::vector<Issue> check(const Drawing &drawing, const Machine &machine, double extra_height)
{
    std::vector<Issue>          issues;
    const std::vector<PointRef> points      = list_points(drawing);
    const bool                  has_bed     = machine.bed_shape.size() >= 3;
    const bool                  has_exclude = machine.exclude_area.size() >= 3;
    for (size_t i = 0; i < points.size(); ++ i) {
        const Vec3d &p = *point_position(drawing, points[i]);
        const Vec2d  xy(p.x(), p.y());
        if (has_bed && !inside_polygon(machine.bed_shape, xy) && distance_to_outline(machine.bed_shape, xy) > MIN_LENGTH)
            issues.push_back({ IssueType::OutsideBed, i });
        else if (has_exclude && inside_polygon(machine.exclude_area, xy) && distance_to_outline(machine.exclude_area, xy) > MIN_LENGTH)
            issues.push_back({ IssueType::InExcludeArea, i });
        if (p.z() < -Z_EPSILON)
            issues.push_back({ IssueType::BelowBed, i });
        else if (machine.max_height > 0. && p.z() + extra_height > machine.max_height + Z_EPSILON)
            issues.push_back({ IssueType::AboveMaxHeight, i });
        if (points[i].mid) {
            const size_t   idx     = size_t(points[i].segment);
            const Segment &segment = drawing.segments[idx];
            if (!arc_through(drawing.segment_start(idx), segment.mid, segment.end).valid)
                issues.push_back({ IssueType::AlignedArc, i });
        }
    }
    return issues;
}

std::string generate(const Drawing &base, const Settings &settings, const Machine &machine, const ExportOptions &options, Stats *stats_out)
{
    const Stack stack(base, settings);
    Writer      writer(machine.relative_e, options.comments);
    for (const std::string &text : options.header)
        writer.comment(text);

    if (options.standalone) {
        writer.line("M140 S" + std::to_string(options.bed_temp), "bed temperature");
        writer.line("M104 S" + std::to_string(options.nozzle_temp), "nozzle temperature");
        writer.line("G28", "home all axes");
        writer.line("M190 S" + std::to_string(options.bed_temp), "wait for the bed temperature");
        writer.line("M109 S" + std::to_string(options.nozzle_temp), "wait for the nozzle temperature");
    }
    writer.line("G90", "absolute coordinates");
    if (machine.relative_e)
        writer.line("M83", "relative extrusion");
    else {
        writer.line("M82", "absolute extrusion");
        writer.line("G92 E0", "reset the extruder");
    }
    if (options.fan_percent > 0)
        writer.line("M106 S" + std::to_string(std::clamp(int(std::lround(options.fan_percent * 2.55)), 0, 255)), "part cooling fan");

    if (!stack.empty()) {
        const Vec3d &start = stack.start();
        if (options.safe_approach && options.approach_lift > 0.) {
            writer.line("G91", "relative coordinates");
            writer.line("G1 Z" + num(options.approach_lift, 3) + " F" + num(feedrate(Z_SPEED), 0), "lift the nozzle");
            writer.line("G90", "absolute coordinates");
            writer.line("G1 X" + num(start.x(), 3) + " Y" + num(start.y(), 3) + " F" + num(feedrate(settings.travel_speed), 0), "move above the start");
            writer.line("G1 Z" + num(start.z(), 3) + " F" + num(feedrate(Z_SPEED), 0), "lower onto the start");
            writer.set_feedrate(feedrate(Z_SPEED));
        } else {
            writer.line("G1 X" + num(start.x(), 3) + " Y" + num(start.y(), 3) + " Z" + num(start.z(), 3) + " F" +
                        num(feedrate(settings.travel_speed), 0), "move to the start");
            writer.set_feedrate(feedrate(settings.travel_speed));
        }
        writer.set_position(start);
    }

    // The segments of all the layers, each one computed once, with the next one to know where the travel moves end.
    const size_t count            = stack.empty() ? 0 : stack.size();
    Vec3d        from             = stack.start();
    bool         previous_extrude = true;
    Segment      segment          = count > 0 ? stack.segment(0) : Segment();
    for (size_t idx = 0; idx < count; ++ idx) {
        const bool    has_next = idx + 1 < count;
        const Segment next     = has_next ? stack.segment(idx + 1) : Segment();
        const Arc     arc      = segment.type == SegmentType::Arc ? arc_through(from, segment.mid, segment.end) : Arc();
        if (segment.extrude) {
            writer.unretract(settings.retract_speed);
            write_path(writer, from, segment, arc, 0., e_per_mm(segment, settings), print_speed(segment, settings), options);
        } else {
            writer.retract(settings.retract_length, settings.retract_speed);
            // The nozzle is lifted along the travel moves and lowered at the end of the last one.
            const double hop          = std::max(settings.z_hop, 0.);
            const bool   first_travel = idx == 0 || previous_extrude;
            const bool   last_travel  = !has_next || next.extrude;
            if (hop > 0. && first_travel)
                writer.move_to(from + Vec3d(0., 0., hop), 0., Z_SPEED, "lift");
            write_path(writer, from, segment, arc, hop, 0., travel_speed(segment, settings), options);
            if (hop > 0. && last_travel)
                writer.move_to(segment.end, 0., Z_SPEED, "lower");
        }
        previous_extrude = segment.extrude;
        from             = segment.end;
        segment          = next;
    }

    if (options.standalone) {
        writer.retract(settings.retract_length, settings.retract_speed);
        writer.line("G91", "relative coordinates");
        writer.line("G1 Z5 F" + num(feedrate(Z_SPEED), 0), "lift the nozzle");
        writer.line("G90", "absolute coordinates");
        writer.line("M104 S0", "nozzle heater off");
        writer.line("M140 S0", "bed heater off");
        writer.line("M107", "part cooling fan off");
        writer.line("M84", "motors off");
    }

    if (stats_out != nullptr)
        *stats_out = compute_stats(base, settings);
    return writer.gcode();
}

Stats compute_stats(const Drawing &base, const Settings &settings)
{
    // Every layer is the first one, higher: its statistics times the layers, plus the travel moves between them.
    const Stack    stack(base, settings);
    const Drawing &drawing = stack.first();
    Stats          stats;
    for (size_t idx = 0; drawing.has_start && idx < drawing.segments.size(); ++ idx) {
        const Segment &segment = drawing.segments[idx];
        const Vec3d   &from    = drawing.segment_start(idx);
        const Arc      arc     = segment.type == SegmentType::Arc ? arc_through(from, segment.mid, segment.end) : Arc();
        const double   length  = arc.valid ? arc.length() : (segment.end - from).norm();
        if (segment.extrude) {
            stats.extrude_length  += length;
            stats.filament_length += length * e_per_mm(segment, settings);
            stats.time            += length / print_speed(segment, settings);
        } else {
            stats.travel_length += length;
            stats.time          += length / travel_speed(segment, settings);
        }
    }
    const double layers = double(stack.layers());
    stats.extrude_length *= layers;
    stats.travel_length *= layers;
    stats.filament_length *= layers;
    stats.time *= layers;
    if (stack.transition()) {
        const double length = (drawing.start + Vec3d(0., 0., std::max(settings.layer_height, 0.)) - drawing.last_point()).norm();
        stats.travel_length += (layers - 1.) * length;
        stats.time += (layers - 1.) * length / std::max(settings.travel_speed, 1.);
    }
    // mm³ x g/cm³ = 1/1000 g
    stats.filament_weight = stats.filament_length * filament_area(settings) * settings.filament_density * 1e-3;
    return stats;
}

std::string serialize(const Drawing &drawing, const Settings &settings)
{
    using nlohmann::json;
    auto point = [](const Vec3d &p) { return json::array({ p.x(), p.y(), p.z() }); };

    json root;
    root["version"] = 1;
    if (drawing.has_start)
        root["start"] = point(drawing.start);
    json segments = json::array();
    for (const Segment &segment : drawing.segments) {
        json item;
        item["type"]    = segment.type == SegmentType::Arc ? "arc" : "line";
        item["extrude"] = segment.extrude;
        if (segment.type == SegmentType::Arc)
            item["mid"] = point(segment.mid);
        item["end"] = point(segment.end);
        if (segment.width > 0.)
            item["width"] = segment.width;
        if (segment.height > 0.)
            item["height"] = segment.height;
        if (segment.speed > 0.)
            item["speed"] = segment.speed;
        segments.push_back(std::move(item));
    }
    root["segments"] = std::move(segments);
    root["settings"] = json{ { "line_width", settings.line_width },
                             { "layer_height", settings.layer_height },
                             { "print_speed", settings.print_speed },
                             { "travel_speed", settings.travel_speed },
                             { "flow_ratio", settings.flow_ratio },
                             { "filament_diameter", settings.filament_diameter },
                             { "filament_density", settings.filament_density },
                             { "retract_length", settings.retract_length },
                             { "retract_speed", settings.retract_speed },
                             { "z_hop", settings.z_hop },
                             { "layers", settings.layers },
                             { "spiral", settings.spiral } };

    const std::string text = root.dump();
    std::string       out  = std::string(DRAWING_BEGIN) + "\n";
    // Short lines: some printers limit the length of the lines they read.
    for (size_t i = 0; i < text.size(); i += 76)
        out += "; " + text.substr(i, 76) + "\n";
    out += std::string(DRAWING_END) + "\n";
    return out;
}

bool deserialize(const std::string &gcode, Drawing &drawing, Settings &settings)
{
    using nlohmann::json;
    const size_t begin = gcode.find(DRAWING_BEGIN);
    if (begin == std::string::npos)
        return false;
    const size_t end = gcode.find(DRAWING_END, begin);
    if (end == std::string::npos)
        return false;

    std::string        text;
    std::istringstream lines(gcode.substr(begin, end - begin));
    std::string        line;
    // The first line is DRAWING_BEGIN.
    std::getline(lines, line);
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("; ", 0) == 0)
            text += line.substr(2);
        else if (line.rfind(";", 0) == 0)
            text += line.substr(1);
    }

    try {
        const json root  = json::parse(text);
        auto       point = [](const json &item) {
            return Vec3d(item.at(0).get<double>(), item.at(1).get<double>(), item.at(2).get<double>());
        };
        Drawing read;
        if (root.contains("start")) {
            read.has_start = true;
            read.start     = point(root.at("start"));
        }
        if (root.contains("segments"))
            for (const json &item : root.at("segments")) {
                Segment segment;
                segment.type    = item.value("type", std::string("line")) == "arc" ? SegmentType::Arc : SegmentType::Line;
                segment.extrude = item.value("extrude", true);
                segment.end     = point(item.at("end"));
                if (segment.type == SegmentType::Arc)
                    segment.mid = point(item.at("mid"));
                segment.width  = item.value("width", 0.);
                segment.height = item.value("height", 0.);
                segment.speed  = item.value("speed", 0.);
                read.segments.push_back(segment);
            }
        if (!read.has_start && !read.segments.empty())
            return false;

        Settings read_settings = settings;
        if (root.contains("settings")) {
            const json &s = root.at("settings");
            read_settings.line_width        = s.value("line_width", read_settings.line_width);
            read_settings.layer_height      = s.value("layer_height", read_settings.layer_height);
            read_settings.print_speed       = s.value("print_speed", read_settings.print_speed);
            read_settings.travel_speed      = s.value("travel_speed", read_settings.travel_speed);
            read_settings.flow_ratio        = s.value("flow_ratio", read_settings.flow_ratio);
            read_settings.filament_diameter = s.value("filament_diameter", read_settings.filament_diameter);
            read_settings.filament_density  = s.value("filament_density", read_settings.filament_density);
            read_settings.retract_length    = s.value("retract_length", read_settings.retract_length);
            read_settings.retract_speed     = s.value("retract_speed", read_settings.retract_speed);
            read_settings.z_hop             = s.value("z_hop", read_settings.z_hop);
            read_settings.layers            = std::clamp(s.value("layers", 1), 1, MAX_LAYERS);
            read_settings.spiral            = s.value("spiral", false);
        }
        drawing  = std::move(read);
        settings = read_settings;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool import_moves(const std::string &gcode, Drawing &drawing)
{
    Drawing            read;
    bool               absolute   = true;
    bool               relative_e = false;
    Vec3d              position   = Vec3d::Zero();
    bool               known[3]   = { false, false, false };
    double             e          = 0.;
    // mm/s
    double             speed      = 0.;
    std::istringstream input(gcode);
    std::string        raw;
    while (std::getline(input, raw) && read.segments.size() < MAX_IMPORTED) {
        std::istringstream words(raw.substr(0, raw.find(';')));
        words.imbue(std::locale::classic());
        std::string command;
        if (!(words >> command))
            continue;
        for (char &c : command)
            c = char(std::toupper((unsigned char) c));

        // Parameters by letter.
        std::array<double, 26> values {};
        std::array<bool, 26>   given {};
        std::string            word;
        while (words >> word) {
            const int letter = std::toupper((unsigned char) word[0]) - 'A';
            if (letter < 0 || letter >= 26 || word.size() < 2)
                continue;
            std::istringstream number(word.substr(1));
            number.imbue(std::locale::classic());
            double value = 0.;
            if (number >> value) {
                values[letter] = value;
                given[letter]  = true;
            }
        }
        auto has   = [&given](char c) { return given[c - 'A']; };
        auto value = [&values](char c) { return values[c - 'A']; };

        if (command == "G90")
            absolute = true;
        else if (command == "G91")
            absolute = false;
        else if (command == "M82")
            relative_e = false;
        else if (command == "M83")
            relative_e = true;
        else if (command == "G92") {
            for (int axis = 0; axis < 3; ++ axis)
                if (has("XYZ"[axis])) {
                    position[axis] = value("XYZ"[axis]);
                    known[axis]    = true;
                }
            if (has('E'))
                e = value('E');
        } else {
            const int code = (command == "G0" || command == "G00") ? 0 : (command == "G1" || command == "G01") ? 1 :
                             (command == "G2" || command == "G02") ? 2 : (command == "G3" || command == "G03") ? 3 : -1;
            if (code < 0)
                continue;
            const Vec3d from       = position;
            const bool  from_known = known[0] && known[1] && known[2];
            for (int axis = 0; axis < 3; ++ axis) {
                const char c = "XYZ"[axis];
                if (!has(c))
                    continue;
                if (absolute) {
                    position[axis] = value(c);
                    known[axis]    = true;
                } else if (known[axis])
                    position[axis] += value(c);
            }
            double de = 0.;
            if (has('E')) {
                de = relative_e ? value('E') : value('E') - e;
                e  = relative_e ? e + value('E') : value('E');
            }
            if (has('F') && value('F') > 0.)
                speed = value('F') / 60.;
            if (!(known[0] && known[1] && known[2]))
                continue;
            if (!from_known || !read.has_start) {
                // The first position completely known starts the drawing.
                read.has_start = true;
                read.start     = position;
                continue;
            }
            const bool extrude = de > 1e-9;
            if (code <= 1 || !(has('I') || has('J') || has('R'))) {
                if ((position - from).norm() < MIN_LENGTH)
                    continue;
                Segment segment;
                segment.type    = SegmentType::Line;
                segment.extrude = extrude;
                segment.end     = position;
                segment.speed   = speed;
                read.segments.push_back(segment);
                continue;
            }

            // Arc in the XY plane, helical if Z changes.
            const bool  ccw = code == 3;
            const Vec2d s(from.x(), from.y());
            const Vec2d t(position.x(), position.y());
            Vec2d       center;
            if (has('I') || has('J'))
                center = s + Vec2d(has('I') ? value('I') : 0., has('J') ? value('J') : 0.);
            else {
                // Radius form: center on the bisector of the chord, the arc shorter than half a turn for a positive R.
                const double r      = value('R');
                const Vec2d  chord  = t - s;
                const double length = chord.norm();
                if (length < MIN_LENGTH || std::abs(r) < 0.5 * length - MIN_LENGTH) {
                    Segment segment;
                    segment.type    = SegmentType::Line;
                    segment.extrude = extrude;
                    segment.end     = position;
                    segment.speed   = speed;
                    read.segments.push_back(segment);
                    continue;
                }
                const double h    = std::sqrt(std::max(0., r * r - 0.25 * length * length));
                const Vec2d  left = Vec2d(-chord.y(), chord.x()) / length;
                center = 0.5 * (s + t) + ((ccw == (r > 0.)) ? h : -h) * left;
            }
            const double radius = (s - center).norm();
            if (radius < MIN_LENGTH)
                continue;
            const double a0    = std::atan2(s.y() - center.y(), s.x() - center.x());
            const double a1    = std::atan2(t.y() - center.y(), t.x() - center.x());
            double       sweep = ccw ? a1 - a0 : a0 - a1;
            while (sweep <= 1e-9)
                sweep += 2. * PI;
            while (sweep > 2. * PI)
                sweep -= 2. * PI;
            auto at = [&](double f) {
                const double angle = a0 + (ccw ? f : -f) * sweep;
                return Vec3d(center.x() + radius * std::cos(angle), center.y() + radius * std::sin(angle),
                             from.z() + f * (position.z() - from.z()));
            };
            // More than half a turn: two arcs, so that the middle points keep the direction.
            const int pieces = sweep > PI ? 2 : 1;
            for (int k = 0; k < pieces; ++ k) {
                Segment segment;
                segment.type    = SegmentType::Arc;
                segment.extrude = extrude;
                segment.mid     = at((k + 0.5) / pieces);
                segment.end     = k + 1 == pieces ? position : at(double(k + 1) / pieces);
                segment.speed   = speed;
                read.segments.push_back(segment);
            }
        }
    }
    if (!read.has_start)
        return false;
    drawing = std::move(read);
    return true;
}

} // namespace GCodeSketch
} // namespace Slic3r
