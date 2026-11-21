#ifndef slic3r_GCodeSketch_hpp_
#define slic3r_GCodeSketch_hpp_

// Orio: engine of the "G-code drawing" tab. A drawing is a chain of straight lines and of arcs through 3 points,
// drawn in the build volume of a printer, each segment printing (extruding) or travelling. The engine computes the arcs,
// checks the points against the printer, writes the G-code and saves the drawing into it; it doesn't depend on the GUI.

#include "Point.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace GCodeSketch {

enum class SegmentType : unsigned char { Line, Arc };

struct Segment
{
    SegmentType type    { SegmentType::Line };
    // False: travel move, nothing is extruded.
    bool        extrude { true };
    Vec3d       end     { Vec3d::Zero() };
    // Arc: point the arc passes through, between the start and the end of the segment.
    Vec3d       mid     { Vec3d::Zero() };
    // Settings of this segment, 0 to use the ones of the drawing.
    double      width   { 0. };
    double      height  { 0. };
    // mm/s
    double      speed   { 0. };

    bool operator==(const Segment &rhs) const;
    bool operator!=(const Segment &rhs) const { return !(*this == rhs); }
};

// Chain of segments from 'start', in the coordinates of the G-code of the printer.
struct Drawing
{
    bool                 has_start { false };
    Vec3d                start     { Vec3d::Zero() };
    std::vector<Segment> segments;

    bool         empty() const { return !has_start; }
    void         clear();
    // Point where the segment idx starts.
    const Vec3d &segment_start(size_t idx) const { return idx == 0 ? start : segments[idx - 1].end; }
    // Last point of the chain.
    const Vec3d &last_point() const { return segments.empty() ? start : segments.back().end; }

    bool operator==(const Drawing &rhs) const;
    bool operator!=(const Drawing &rhs) const { return !(*this == rhs); }
};

// One point of a drawing: its start (segment -1), or the end or the middle point of the arc of a segment.
struct PointRef
{
    int  segment { -1 };
    bool mid     { false };

    bool operator==(const PointRef &rhs) const { return segment == rhs.segment && mid == rhs.mid; }
    bool operator!=(const PointRef &rhs) const { return !(*this == rhs); }
};

// Points of the drawing in their order along the chain: the start, then for each segment the middle point of its arc
// and its end.
std::vector<PointRef> list_points(const Drawing &drawing);
// Position of a point, nullptr if the drawing has no such point.
const Vec3d*          point_position(const Drawing &drawing, const PointRef &ref);
Vec3d*                point_position(Drawing &drawing, const PointRef &ref);

// Settings of the segments which don't set their own.
struct Settings
{
    double line_width        { 0.42 };
    double layer_height      { 0.2 };
    // mm/s
    double print_speed       { 30. };
    double travel_speed      { 150. };
    // Flow multiplier, 1 = 100 %.
    double flow_ratio        { 1. };
    double filament_diameter { 1.75 };
    // g/cm³, for the weight of the filament used.
    double filament_density  { 1.24 };
    // 0: no retraction before travelling.
    double retract_length    { 0.8 };
    // mm/s
    double retract_speed     { 30. };
    // 0: the nozzle isn't lifted while travelling.
    double z_hop             { 0.4 };
    // The drawing is printed on this number of layers, one layer height above the other: an object.
    int    layers            { 1 };
    // The height rises continuously along the drawing instead of layer by layer (spiral, as the vase mode).
    bool   spiral            { false };
};

// Build volume of the printer, in the coordinates of its G-code.
struct Machine
{
    Pointfs bed_shape;
    Pointfs exclude_area;
    double  max_height { 250. };
    bool    relative_e { true };
};

// Arc through 3 points.
struct Arc
{
    // False if the 3 points are aligned or merged: the segment is then a straight line from its start to its end.
    bool   valid  { false };
    Vec3d  start  { Vec3d::Zero() };
    Vec3d  end    { Vec3d::Zero() };
    Vec3d  center { Vec3d::Zero() };
    // Unit axis around which the arc turns counterclockwise (right-hand rule) from its start to its end.
    Vec3d  normal { Vec3d::UnitZ() };
    double radius { 0. };
    // Angle swept from the start to the end, in ]0, 2 PI]. 2 PI: full circle, back to the start.
    double angle  { 0. };

    // Point at the fraction t in [0, 1] of the arc.
    Vec3d  point_at(double t) const;
    double length() const { return radius * angle; }
};

// Arc from start to end passing through mid. When end is back on start, full horizontal circle whose diameter goes
// from start to mid.
Arc arc_through(const Vec3d &start, const Vec3d &mid, const Vec3d &end);

// Points along the arc with a chordal error up to max_deviation, without its start, with its end, at most max_points.
std::vector<Vec3d> discretize_arc(const Arc &arc, double max_deviation, size_t max_points = 4096);

// Cross-section in mm² of an extrusion of the given width and height (model of Flow::mm3_per_mm()).
double extrusion_area(double width, double height);

// Most layers of a drawing.
constexpr int MAX_LAYERS = 10000;

// The drawing on its settings.layers layers, settings.layer_height apart, linked by travel moves; in spiral mode the
// height rises along the drawing (its arcs split in quarters of turn), continuously from one layer to the next when
// the drawing is closed. generate() and compute_stats() don't store it: this is for showing it.
Drawing stack_layers(const Drawing &drawing, const Settings &settings);
// Number of segments of stack_layers(), without building it.
size_t  stacked_segments(const Drawing &drawing, const Settings &settings);
// Height the layers add above the drawing.
double  layers_height(const Settings &settings);

// The 4 sides of a rectangle from a corner to the opposite one, its sides along the unit axes u and v, in the plane
// of u and v through the corner. The rectangle ends back on the corner. Empty if it is flat.
std::vector<Segment> rectangle_segments(const Vec3d &corner, const Vec3d &opposite, const Vec3d &u, const Vec3d &v);
// A circle around center, in the plane of the given normal, starting and ending at the projection of start on it:
// two half arcs, counterclockwise around the normal. Empty if the radius is null.
std::vector<Segment> circle_segments(const Vec3d &center, const Vec3d &start, const Vec3d &normal);

// Unit direction of the nozzle at the end of the segment idx, zero if the segment has no length.
Vec3d end_tangent(const Drawing &drawing, size_t idx);
// Unit direction of the nozzle at the start of the segment idx, zero if the segment has no length.
Vec3d start_tangent(const Drawing &drawing, size_t idx);
// Middle point of the arc from start to end which leaves start along the unit vector tangent. False if end is on the
// line of the tangent or on start: the arc is then a straight line.
bool  tangent_arc_middle(const Vec3d &start, const Vec3d &tangent, const Vec3d &end, Vec3d &middle);
// Middle point of the arc of the given radius (at least half the chord) from start to end, on the side of the point
// side, in the plane of the 3 points: the short arc, or the long one when side is farther from the chord than the
// radius. False if the 3 points are aligned or the chord null.
bool  arc_middle_with_radius(const Vec3d &start, const Vec3d &end, const Vec3d &side, double radius, Vec3d &middle);

// A shape to add to a drawing: its start, then its segments.
struct Path
{
    Vec3d                start { Vec3d::Zero() };
    std::vector<Segment> segments;

    bool empty() const { return segments.empty(); }
};

// Shapes of the sketch tools, in the plane of the unit normal through their first point. Empty when they are flat or
// their points merged. The closed ones end back on their start.
// Rectangle with the side from a to b, its other side as long as c is far from the line of that side.
Path rectangle_3_points(const Vec3d &a, const Vec3d &b, const Vec3d &c, const Vec3d &normal);
// Rectangle around center with the corner corner, its sides along the unit axes u and v.
Path center_rectangle(const Vec3d &center, const Vec3d &corner, const Vec3d &u, const Vec3d &v);
// Circle around center through the projection of point, counterclockwise around the normal.
Path circle(const Vec3d &center, const Vec3d &point, const Vec3d &normal);
// Circle of diameter from a to b.
Path circle_2_points(const Vec3d &a, const Vec3d &b, const Vec3d &normal);
// Circle through 3 points, in their plane.
Path circle_3_points(const Vec3d &a, const Vec3d &b, const Vec3d &c);
// Arc around center from the projection of start, turning by angle radians, counterclockwise around the normal when
// positive, a full turn at most.
Path center_arc(const Vec3d &center, const Vec3d &start, double angle, const Vec3d &normal);
// The same arc as an Arc, not valid without radius or angle.
Arc  arc_around(const Vec3d &center, const Vec3d &start, double angle, const Vec3d &normal);
// Regular polygon around center: point is one of its vertices (inscribed in the circle through point) or the middle of
// one of its sides (circumscribed about that circle).
Path polygon(const Vec3d &center, const Vec3d &point, int sides, bool inscribed, const Vec3d &normal);
// Regular polygon with the side from a to b, on the side of the point side.
Path edge_polygon(const Vec3d &a, const Vec3d &b, const Vec3d &side, int sides, const Vec3d &normal);
// Ellipse around center, half its first axis up to major, its other half axis as long as minor is far from the first
// one; at most tolerance from the true ellipse.
Path ellipse(const Vec3d &center, const Vec3d &major, const Vec3d &minor, const Vec3d &normal, double tolerance);
// Straight slot of the given width, its ends half circles around a and b.
Path slot(const Vec3d &a, const Vec3d &b, double width, const Vec3d &normal);
// Slot of the given width along an arc (its middle line), its ends half circles around the ends of the arc.
Path arc_slot(const Arc &arc, double width);
// Smooth curve through the points (centripetal Catmull-Rom spline), back to the first one if closed.
Path spline(const std::vector<Vec3d> &points, bool closed, double tolerance);
// Conic from start to end, tangent there to the lines to apex; rho in ]0, 1[: below 0.5 an arc of ellipse, 0.5 a
// parabola, above an hyperbola.
Path conic(const Vec3d &start, const Vec3d &end, const Vec3d &apex, double rho, double tolerance);
// Arcs each through 3 points of the curve f(t), t from 0 to 1, at most tolerance from it, without the start f(0);
// straight lines where the curve is straight.
std::vector<Segment> curve_segments(const std::function<Vec3d(double)> &f, double tolerance);

// Circle tangent to the lines through a1 and b1 and through a2 and b2, in the plane of the unit normal through point
// (the lines projected on it): in the angle of the lines where point is, centered on the projection of point on the
// bisector of that angle, or of the given radius when it is positive. Between parallel lines, its diameter is their
// distance. False if there is none.
bool circle_tangent_2(const Vec3d &a1, const Vec3d &b1, const Vec3d &a2, const Vec3d &b2, const Vec3d &point, double radius,
                      const Vec3d &normal, Vec3d &center, double &out_radius);
// Circle tangent to the 3 lines through a[i] and b[i], in the plane of the unit normal through a[0]: of the circles
// tangent to the 3 lines, the one touching them the closest to the points picks. False if there is none.
bool circle_tangent_3(const std::array<Vec3d, 3> &a, const std::array<Vec3d, 3> &b, const std::array<Vec3d, 3> &picks,
                      const Vec3d &normal, Vec3d &center, double &radius);

// Shapes recognized in a drawing, whatever drew them.
enum class ShapeType : unsigned char {
    None,
    // 4 straight sides with square corners, back to their start.
    Rectangle,
    // 1 or 2 arcs of one circle, back to their start.
    Circle,
};
// Shape of the segments first to last, all printing or all travelling.
ShapeType shape_type(const Drawing &drawing, size_t first, size_t last);
// The rectangle or the circle the segment idx belongs to: its first and last segments.
ShapeType find_shape(const Drawing &drawing, size_t idx, size_t &first, size_t &last);
// The consecutive segments printing as the segment idx does (or travelling as it does): its path between two travel
// moves.
void      find_path(const Drawing &drawing, size_t idx, size_t &first, size_t &last);
// Remove the segments first to last. The next segments stay in place: a travel move leads to them.
void      remove_segments(Drawing &drawing, size_t first, size_t last);

// Point of the segment idx at the fraction t of it (of its angle for an arc).
Vec3d   segment_point(const Drawing &drawing, size_t idx, double t);
// Fraction of the segment idx the closest to p.
double  segment_fraction(const Drawing &drawing, size_t idx, const Vec3d &p);
// The part of the segment idx from the fraction t0 to t1, starting at segment_point(t0), with the settings of the segment.
Segment segment_part(const Drawing &drawing, size_t idx, double t0, double t1);
// Replace the segments first to last with the paths. The rest of the drawing stays in place: travel moves lead from it
// to the paths and back, merged with the straight travel moves next to them.
void    replace_segments(Drawing &drawing, size_t first, size_t last, const std::vector<Path> &paths);
// Adds a path at the end of the drawing, after a travel move to its start; its segments keep their settings.
void    append_path(Drawing &drawing, const Path &path);

// The modify tools of Fusion 360 work in the working plane: on the segments lying in a plane of its normal.
// Curve of a segment: the segments around it which go on from it without a corner, printing or travelling as it does, in
// the same plane. Closed when it goes back to its start without a corner.
struct Curve
{
    size_t first  { 0 };
    size_t last   { 0 };
    bool   closed { false };

    size_t size() const { return last - first + 1; }
};
// False if the segment idx doesn't lie in a plane of the normal. Only arcs go on from one another (circles, ellipses,
// splines): a line is a curve of its own, as the entities of Fusion 360.
bool find_curve(const Drawing &drawing, size_t idx, const Vec3d &normal, Curve &curve);
// The smallest closed loop of the path of the segment idx holding it: its first and last segments. False if none.
bool find_loop(const Drawing &drawing, size_t idx, size_t &first, size_t &last);
// Trim: the part of the curve of the segment idx around the point at, between the printed segments crossing it, goes;
// the whole curve if none crosses it. removed: that part, to show it. False if there is nothing to remove.
bool trim_part(const Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal, Path &removed);
bool trim(Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal);
// Break: the curve of the segment idx is split where the printed segments crossing it bound its part around at, or at at
// itself if none crosses it. points: where, to show them. False if it is already split there.
bool break_points(const Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal, std::vector<Vec3d> &points);
bool break_curve(Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal);
// Extend: the free end of the curve of the segment idx the closest to at (followed by a travel move or by nothing) goes
// on, straight or along its circle, up to the first printed segment it meets. extension: what it adds, to show it.
bool extend_part(const Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal, Path &extension);
bool extend(Drawing &drawing, size_t idx, const Vec3d &at, const Vec3d &normal);

// Corner of a path: the segment in ends where the segment out starts, not tangent to it; out is the first segment of a
// closed loop whose last segment is in.
struct Corner
{
    size_t in  { 0 };
    size_t out { 0 };
};
// The corner at the end of the segment idx the closest to at, otherwise at its other end. False if there is none.
bool find_corner(const Drawing &drawing, size_t idx, const Vec3d &at, Corner &corner);
// Point of the corner.
Vec3d corner_point(const Drawing &drawing, const Corner &corner);
// Fillet: an arc of the radius tangent to the two segments of the corner (lines or arcs in a plane of the normal), which
// are cut back to it. shape: the arc, to show it.
bool fillet_shape(const Drawing &drawing, const Corner &corner, double radius, const Vec3d &normal, Path &shape);
bool fillet(Drawing &drawing, const Corner &corner, double radius, const Vec3d &normal);
// Chamfer of a corner between two lines in a plane of the normal: a line from the first one, distance1 before the
// corner, to the second one, distance2 after it; or leaving the first one with the angle (radians) when it is positive.
bool chamfer_shape(const Drawing &drawing, const Corner &corner, double distance1, double distance2, double angle, const Vec3d &normal,
                   Path &shape);
bool chamfer(Drawing &drawing, const Corner &corner, double distance1, double distance2, double angle, const Vec3d &normal);

// Offset: the path of the segments first to last at the distance, on its left around the normal when positive: lines
// parallel, arcs concentric, the corners extended or cut where they meet, rounded beyond 4 times the distance. Empty if
// it vanishes or if the path isn't in a plane of the normal.
Path   offset_path(const Drawing &drawing, size_t first, size_t last, double distance, const Vec3d &normal);
// Signed distance from p to the path of the segments first to last, in a plane of the normal: positive on its left.
// False if the path isn't in such a plane.
bool   distance_to_path(const Drawing &drawing, size_t first, size_t last, const Vec3d &p, const Vec3d &normal, double &distance);

// Moves of the segments: mirror, patterns, scale, move, rotation.
// The segments first to last transformed in place, with their start (the end of the segment before them).
void        transform_segments(Drawing &drawing, size_t first, size_t last, const Transform3d &transformation);
// A copy of the segments first to last transformed, as a path.
Path        transformed_path(const Drawing &drawing, size_t first, size_t last, const Transform3d &transformation);
// Mirror across the line through a along the direction, in the plane of the normal.
Transform3d mirror_transformation(const Vec3d &a, const Vec3d &direction, const Vec3d &normal);
// Rotation by the angle (radians, counterclockwise around the normal when positive) around the axis of the normal through
// center.
Transform3d rotation_around(const Vec3d &center, double angle, const Vec3d &normal);
// Scale by the factor from base, in the plane of the normal: the distances along the normal don't change.
Transform3d scale_in_plane(const Vec3d &base, double factor, const Vec3d &normal);

// Problems of a drawing for a printer.
enum class IssueType : unsigned char {
    // Point outside of the bed.
    OutsideBed,
    // Point in the excluded area of the bed.
    InExcludeArea,
    // Point below the bed (Z < 0).
    BelowBed,
    // Point above the printable height.
    AboveMaxHeight,
    // Arc through 3 aligned points, printed as a straight line.
    AlignedArc,
};

struct Issue
{
    IssueType type;
    // Index of the point in list_points().
    size_t    point;
};

// extra_height: height the layers add above the points (layers_height()).
std::vector<Issue> check(const Drawing &drawing, const Machine &machine, double extra_height = 0.);

struct ExportOptions
{
    // Full program (heating, homing, end) run alone by the printer; otherwise a block to insert into G-code.
    bool        standalone    { false };
    int         nozzle_temp   { 220 };
    int         bed_temp      { 60 };
    // Part cooling fan, 0 to leave it as it is.
    int         fan_percent   { 0 };
    // Lift the nozzle before moving to the start of the drawing, then lower it onto it.
    bool        safe_approach { true };
    double      approach_lift { 2. };
    // Write the horizontal arcs as G2 / G3; otherwise every arc is made of straight moves.
    bool        arcs          { true };
    // Chordal error of the arcs made of straight moves.
    double      arc_deviation { 0.01 };
    bool        comments      { true };
    // Comment lines written at the top, e.g. the printer and the filament.
    std::vector<std::string> header;
};

struct Stats
{
    // mm
    double extrude_length  { 0. };
    double travel_length   { 0. };
    // mm of filament
    double filament_length { 0. };
    // g
    double filament_weight { 0. };
    // s, without accelerations
    double time            { 0. };
};

// G-code of the drawing.
std::string generate(const Drawing &drawing, const Settings &settings, const Machine &machine, const ExportOptions &options,
                     Stats *stats = nullptr);
// Length, filament and time of the drawing.
Stats       compute_stats(const Drawing &drawing, const Settings &settings);

// The drawing and its settings, as comments to write at the end of its G-code to edit it again later.
std::string serialize(const Drawing &drawing, const Settings &settings);
// Read the drawing written by serialize() in a G-code. False if the G-code has none.
bool        deserialize(const std::string &gcode, Drawing &drawing, Settings &settings);
// Read the moves (G0, G1, G2, G3) of any G-code as a drawing. False if it has none.
bool        import_moves(const std::string &gcode, Drawing &drawing);

} // namespace GCodeSketch
} // namespace Slic3r

#endif // slic3r_GCodeSketch_hpp_
