#ifndef slic3r_GCodeSketchPanel_hpp_
#define slic3r_GCodeSketchPanel_hpp_

// Orio: "G-code drawing" tab, to the right of the Filament Manager. A printer is chosen, then lines, arcs,
// rectangles, circles, polygons, ellipses, slots, splines, conics, points and texts are drawn in its build volume
// (GCodeSketchView) with the toolbox on the left of the view, with the sketch tools of Fusion 360 and their variants;
// they are mirrored, copied in patterns, filleted, chamfered, offset, trimmed, extended, broken, scaled, moved, copied
// and rotated with its modify tools.
// The points are placed on a working plane (horizontal, front or side) which follows the drawing through its last
// point and moves with the bar on the right of the view, Alt or the middle button. The Select tool selects a segment,
// a rectangle, a circle or a whole path, whose values are edited on the right. The G-code is exported as a .gcode file,
// or shown to be copied or sent to the printer through the G-code console (GCodeSketchDialog). The drawing is saved in
// the .gcode as comments, so that the file opens again for editing; any other G-code opens as the drawing of its
// moves.

#include "libslic3r/GCodeSketch.hpp"

#include <wx/panel.h>

#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class Button;
class wxCheckBox;
class wxChoice;
class wxScrolledWindow;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;
class wxWindow;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

class GCodeSketchView;
class GCodeSketchToolbox;
class GCodeSketchPlaneBar;

// Tools of the toolbox: the sketch tools of Fusion 360.
enum class SketchTool : unsigned char {
    Select,
    Line,
    // 3-point arc: its start, its end, then a point of it.
    Arc,
    // 2-point rectangle: two opposite corners.
    Rectangle,
    // Center and diameter circle: its center, then a point of it.
    Circle,
    // A side, then the other side.
    Rectangle3Points,
    // Its center, then a corner.
    RectangleCenter,
    // The ends of a diameter.
    Circle2Points,
    Circle3Points,
    // Two lines picked, then its center along their bisector.
    CircleTangent2,
    // Three lines picked.
    CircleTangent3,
    // Its center, its start, then its end.
    ArcCenter,
    // From the end of a line or of an arc, along it, then its end.
    ArcTangent,
    // Its center, then the middle of a side.
    PolygonCircumscribed,
    // Its center, then a vertex.
    PolygonInscribed,
    // A side, then the side where it is.
    PolygonEdge,
    // Its center, the end of an axis, then the other axis.
    Ellipse,
    // The centers of its ends, then its width.
    SlotCenters,
    // Its ends, then its width.
    SlotOverall,
    // Its center, the center of an end, then its width.
    SlotCenter,
    // The centers of its ends, a point of its arc, then its width.
    SlotArc3Points,
    // The center of its arc, the centers of its ends, then its width.
    SlotArcCenter,
    // Fit point spline: its points, a double click or Enter to end it.
    Spline,
    // Its ends, its apex; rho typed.
    Conic,
    // A point the nozzle goes to.
    Point,
    // Where it starts, then its text, its font and its size, in a dialog.
    Text,
    // The objects, then a line of the drawing or two points: mirrored copies.
    Mirror,
    // The objects, then the center; the number of copies and the angle typed.
    CircularPattern,
    // The objects, then the spacing with the mouse or typed; the numbers of copies typed.
    RectangularPattern,
    // Corners clicked, rounded with the radius typed.
    Fillet,
    // Corners between two lines clicked, cut: the same distance on both, a distance and an angle, two distances.
    ChamferEqual,
    ChamferDistanceAngle,
    ChamferTwoDistances,
    // A path, then the distance with the mouse or typed: a parallel copy.
    Offset,
    // A part of a curve between the lines crossing it, removed.
    Trim,
    // The free end of a curve, going on up to the next line.
    Extend,
    // A curve, split where the lines cross it.
    Break,
    // The objects, the base point, then the factor.
    Scale,
    // The objects, the base point, then where it goes.
    Move,
    Copy,
    // The objects, the center, a point turned, then where it goes.
    Rotate,
};
// Name of a tool for the menus, with its shortcut if it has one.
wxString sketch_tool_name(SketchTool tool);
// Working plane: horizontal (its normal is Z), front (Y) or side (X).
enum class SketchPlane : unsigned char { Horizontal, Front, Side };

// What a click of the current tool does.
enum class SketchInput : unsigned char {
    // Places a point: the cursor, with its snaps and the values typed.
    Point,
    // Picks a straight line of the drawing (tangent circles).
    Line,
    // Adds a shape (a whole path with a double click) to the objects of the tool, or removes it.
    Object,
    // Picks a whole path (offset).
    Path,
    // Picks a curve at a point of it (trim, extend, break).
    Curve,
    // Picks a corner (fillet, chamfers).
    Corner,
    // Picks a straight line of the drawing, or places the first point of the mirror line.
    MirrorLine,
};

// What the view shows of the shape being drawn or of what a tool would change.
struct SketchPreview
{
    // New shapes.
    std::vector<GCodeSketch::Path>       paths;
    // Construction lines.
    std::vector<std::pair<Vec3d, Vec3d>> guides;
    // What the tool removes or replaces.
    std::vector<GCodeSketch::Path>       removed;
    // Points, where a curve is split.
    std::vector<Vec3d>                   marks;
};

// Colour of a working plane, in the view and on its bar.
std::array<float, 3> sketch_plane_rgb(SketchPlane plane);

// What the Select tool selected: a point, or segments (one segment, a rectangle, a circle, a path).
struct SketchSelection
{
    enum class Kind : unsigned char { None, Point, Segments };
    Kind                  kind  { Kind::None };
    GCodeSketch::PointRef point;
    size_t                first { 0 };
    size_t                last  { 0 };

    bool operator==(const SketchSelection &rhs) const
    {
        return kind == rhs.kind && point == rhs.point && first == rhs.first && last == rhs.last;
    }
    bool operator!=(const SketchSelection &rhs) const { return !(*this == rhs); }
};

class GCodeSketchPanel : public wxPanel
{
public:
    explicit GCodeSketchPanel(wxWindow *parent);
    ~GCodeSketchPanel() override;

    // The tab is shown: reload the printers and the filaments, which may have changed.
    void on_activate();
    // Release the OpenGL resources while the OpenGL context is still valid (closing of the application).
    void release_gl();

    // State shown by the view.
    const GCodeSketch::Drawing            &drawing() const { return m_drawing; }
    // The drawing on all its layers when it has several, otherwise empty.
    const GCodeSketch::Drawing            &stacked() const { return m_stacked; }
    const GCodeSketch::Settings           &settings() const { return m_settings; }
    const GCodeSketch::Machine            &machine() const { return m_machine; }
    const std::vector<GCodeSketch::Issue> &issues() const { return m_issues; }

    SketchTool   tool() const { return m_tool; }
    // New segments extrude, otherwise they travel.
    bool         extrude() const { return m_extrude; }
    // What the Extrusion button shows and changes: the selected segments if there are some, otherwise the new ones.
    bool         extrude_shown() const;
    SketchPlane  plane() const { return m_plane; }
    // Index of the coordinate along the normal of the working plane: 2 (Z), 1 (Y) or 0 (X).
    int          plane_axis() const;
    // Position of the working plane along its normal.
    double       plane_offset() const { return m_plane_offset; }
    // Range of the working plane along its normal: the build volume.
    double       plane_min() const;
    double       plane_max() const;
    // Step of the working plane: the layer height along Z, the grid (1 mm without grid) along X and Y.
    double       plane_step() const;
    // Step of the heights: the layer height.
    double       z_step() const;
    // 0: no grid.
    double       grid_step() const;
    // Snap to the ends, middles and centers of the drawing.
    bool         snap_to_points() const;
    // Snap to the X, Y and Z axes through the last point.
    bool         snap_to_axes() const;
    // Perpendicular, parallel and tangent to the last segment, lined up with the points.
    bool         constraints() const;
    // Points clicked of the shape being drawn: the start then the end of an arc, the first corner of a rectangle, the
    // center of a circle...
    const std::vector<Vec3d> &clicks() const { return m_clicks; }
    // Clicks the current tool takes to make its shape, 0 for as many as wanted (line, spline).
    size_t       clicks_needed() const;
    // The shape of the current tool lies in the working plane through its first point: its points move with the plane.
    bool         flat_tool() const;
    // The next click picks a line (circles tangent to lines).
    bool         picking() const;
    // What a click does with the current tool.
    SketchInput  input() const;
    // The current tool is a modify tool (text, mirror and patterns included): no travel move shown to its shapes.
    bool         modifying() const { return modify_tool(m_tool); }
    // Lines picked by the tangent circles, from one end to the other.
    std::vector<std::pair<Vec3d, Vec3d>> picked_lines() const;
    // What the current tool would make or change: with the cursor as its next point (nullptr if the mouse isn't over
    // the plane), or for the segment hover under the mouse (-1 if none) at its point at.
    void         preview(const Vec3d *cursor, int hover, const Vec3d &at, SketchPreview &out) const;
    // Segments the mirror, the patterns, scale, move, copy and rotate work on, and the path of the offset: sorted.
    const std::vector<size_t> &objects() const { return m_objects; }
    // The objects are chosen: the tool asks for its points.
    bool         objects_chosen() const { return m_objects_done; }
    // Middle of the box of the objects.
    Vec3d        objects_center() const;
    // Distance in the working plane from base to the farthest point of the objects: the scale factor 1.
    double       objects_reach(const Vec3d &base) const;
    // Signed distance in the working plane from p to the path of the offset, positive on its left. False if none.
    bool         offset_distance_at(const Vec3d &p, double &distance) const;
    // Values of the modify tools, the last ones typed: copies of the circular pattern and their angle in degrees
    // (360: all around), copies and spacing of the rectangular pattern along the axes of the plane, fillet radius,
    // chamfer distances and angle in degrees, offset distance (positive on the left of the path), scale factor.
    int          pattern_count() const { return m_pattern_count; }
    void         set_pattern_count(int count);
    double       pattern_angle() const { return m_pattern_angle; }
    void         set_pattern_angle(double degrees);
    int          grid_count(int axis) const { return m_grid_counts[axis == 0 ? 0 : 1]; }
    void         set_grid_count(int axis, int count);
    void         set_grid_spacing(double along_u, double along_v);
    double       fillet_radius() const { return m_fillet_radius; }
    void         set_fillet_radius(double radius);
    double       chamfer_distance(int which) const { return m_chamfer[which == 0 ? 0 : 1]; }
    void         set_chamfer_distance(int which, double distance);
    double       chamfer_angle() const { return m_chamfer_angle; }
    void         set_chamfer_angle(double degrees);
    double       offset_distance() const { return m_offset_distance; }
    void         set_offset_distance(double distance);
    double       scale_factor() const { return m_scale_factor; }
    void         set_scale_factor(double factor);
    // Sides of the polygons and rho of the conics: the last values typed.
    int          polygon_sides() const { return m_polygon_sides; }
    void         set_polygon_sides(int sides);
    double       conic_rho() const { return m_conic_rho; }
    void         set_conic_rho(double rho);
    // Center point arcs and arc slots: the cursor turns around their center from their start; the angle it swept,
    // counterclockwise around the normal of the plane when positive, follows the way the mouse turned.
    bool         sweeping() const;
    double       sweep() const { return m_sweep; }
    void         track_sweep(const Vec3d &cursor);
    void         set_sweep(double angle);
    // Tangent arc: direction of the drawing at its start.
    const Vec3d &arc_tangent() const { return m_tangent; }
    // A line is being drawn: each click adds a segment to it.
    bool         chain_active() const { return m_chain; }
    // The line being drawn has segments, the last ones of the drawing; otherwise only its start is clicked.
    bool         chain_has_segments() const { return m_chain && m_chain_count > 0; }
    // The next point is measured from a point (anchor()): the last point of the line being drawn, the start of an arc,
    // the first click of a rectangle or of a circle.
    bool         has_anchor() const;
    Vec3d        anchor() const;
    // Direction at the end of the line being drawn, zero if it has no segment: the constraints of the next segment.
    Vec3d        chain_tangent() const;
    // A line or a shape is being drawn, or a modify tool has its objects.
    bool         drawing_in_progress() const { return m_chain || !m_clicks.empty() || !m_picks.empty() || !m_objects.empty(); }
    // Changes whenever a point is placed or the tool starts again: the view empties the typed values.
    unsigned     step() const { return m_step; }
    const SketchSelection &selection() const { return m_selection; }
    // Segments a click on the segment idx selects: its rectangle or its circle, otherwise itself.
    void         run_at(size_t idx, size_t &first, size_t &last) const;
    // Coordinates of the points along the normal of the working plane, marked on its bar.
    std::vector<double> plane_marks() const;

    // Edition from the view, the toolbox and the plane bar.
    // Click of the current tool at this point.
    void click(const Vec3d &point);
    // Click on the segment idx at its point at: a line of a tangent circle or of the mirror, an object (double click:
    // its whole path), the path of the offset, a curve to trim, extend or break, a corner to fillet or chamfer.
    void pick(size_t idx, const Vec3d &at, bool double_click = false);
    // Line tool dragged from the end of its line: an arc tangent to the line, to end.
    void add_tangent_arc(const Vec3d &end);
    // Double click: the line ends, the spline is made; the tool stays.
    void end_chain();
    // Enter, OK: the objects being chosen are taken and the tool goes on; otherwise the spline being drawn is made, the
    // pattern, the offset or the scale shown is applied, anything else in progress is dropped, and the Select tool comes
    // back, as in Fusion 360.
    void finish();
    // Escape: the shape in progress, a spline too, is dropped and the Select tool comes back; with the Select tool,
    // nothing is selected any more.
    void escape();
    void set_tool(SketchTool tool);
    void set_plane(SketchPlane plane);
    void next_plane();
    void set_plane_offset(double offset);
    void step_plane(int steps);
    void toggle_extrude();
    void frame_bed();
    // Give the keyboard back to the view, after a click on the toolbox or the plane bar.
    void focus_view();
    void select_point(const GCodeSketch::PointRef &ref);
    void select_segments(size_t first, size_t last);
    // Click on the segment idx: its rectangle or its circle, otherwise itself; path: its whole path.
    void select_at(size_t idx, bool path);
    void clear_selection();
    void delete_selection();
    // A point or segments are dragged: one undo step for the whole move.
    void begin_move(const GCodeSketch::PointRef &ref);
    void move_point(const GCodeSketch::PointRef &ref, const Vec3d &position);
    void begin_move_segments(size_t first, size_t last);
    // Offset from where the segments were when the move began.
    void move_segments(const Vec3d &offset);
    void end_move();
    // Escape during a move: back to where it began.
    void cancel_move();
    void undo();
    void redo();
    // Save the G-code of the drawing as a .gcode file.
    void export_gcode();
    // Show the G-code: options, copy, save, send to the printer.
    void show_gcode();
    // Position of the cursor in the view, nullptr when the mouse isn't over the working plane.
    void show_cursor(const Vec3d *position, const wxString &snap);

private:
    // Fields of the selection in the side panel, and how to read their values.
    struct NumberField
    {
        wxSpinCtrlDouble        *control;
        std::function<double()>  get;
    };
    struct CheckField
    {
        wxCheckBox             *check;
        std::function<bool()>   get;
    };
    // A label and its value, wrapped in the column.
    struct TextField
    {
        wxStaticText              *text;
        wxString                   label;
        std::function<wxString()>  get;
        wxString                   shown;
    };
    // Line picked by a tangent circle.
    struct Pick
    {
        size_t segment;
        Vec3d  a;
        Vec3d  b;
        Vec3d  at;
    };
    // Text tool: what the dialog asks.
    struct TextSettings
    {
        wxString text { "Text" };
        // Empty: the font of the interface.
        wxString font;
        bool     bold    { false };
        bool     italic  { false };
        // Height of the capitals, mm.
        double   height  { 10. };
        // Degrees, counterclockwise in the working plane.
        double   angle   { 0. };
        // Space added between the letters, mm.
        double   spacing { 0. };
    };

    void build_ui();
    // Button of the column, readable in the dark mode too.
    ::Button *make_button(wxWindow *parent, const wxString &label, std::function<void()> action);
    // The texts of the column wrapped at its width, again when it changes.
    void rewrap();
    void load_printers();
    void load_filaments();
    // Configuration of the chosen printer or filament, nullptr if it no longer exists.
    const DynamicPrintConfig *printer_config() const;
    const DynamicPrintConfig *filament_config() const;
    // Build volume of the chosen printer.
    void apply_machine();
    // Settings of a newly chosen printer or filament.
    void apply_printer_settings();
    void apply_filament_settings();
    // Controls to settings and back.
    void read_settings();
    void write_settings();
    // Keep the drawing for undo, before changing it.
    void snapshot();
    // Same, for a change which may leave the drawing as it was (a field, a move dragged back): then end_change() drops
    // its undo step, and Ctrl + Y still redoes what it did.
    void begin_change();
    void end_change();
    // The oldest undo steps beyond MAX_UNDO go.
    void trim_undo();
    // The pen goes to this point without drawing: start of the drawing, otherwise a travel move.
    void move_pen_to(const Vec3d &point);
    // The working plane goes through this point.
    void follow(const Vec3d &point);
    // The tool starts again: the view empties the typed values.
    void next_step();
    // The drawing changed: checks, layers, statistics, selection and view.
    void on_drawing_changed();
    // The tool, the plane or the line changed: toolbox, plane bar, hint and view.
    void on_tool_changed();
    void on_selection_changed();
    void update_status();
    void update_hint();
    void update_export_tooltip();
    void new_drawing();
    void open_drawing();
    // Header of the G-code: the printer and the filament.
    std::vector<std::string> export_header() const;

    // Shapes of the tools.
    // Unit normal of the working plane and its two axes.
    void  plane_frame(Vec3d &normal, Vec3d &u, Vec3d &v) const;
    // Shape of the current tool through these points, as many as it takes. False if they make none.
    bool  shape_path(const std::vector<Vec3d> &points, GCodeSketch::Path &path) const;
    // Shape a create tool would make with the cursor as its next point, empty if it isn't known yet, and its construction
    // lines.
    void  shape_preview(const Vec3d &cursor, GCodeSketch::Path &path, std::vector<std::pair<Vec3d, Vec3d>> &guides) const;
    // Adds a shape to the drawing, after a travel move to its start: one undo step.
    void  add_path(const GCodeSketch::Path &path);
    // Spline through the points clicked, back to its first one if closed: added if there are at least 2 points.
    bool  add_spline(bool closed);
    // Direction of the drawing leaving point, where a segment ends (or the drawing starts). False if none.
    bool  tangent_at(const Vec3d &point, Vec3d &tangent) const;

    // Modify tools.
    // The tool works on objects: the mirror, the patterns, scale, move, copy, rotate.
    static bool takes_objects(SketchTool tool);
    // The tools after the shapes of the create tools: text, mirror, patterns and the modify tools.
    static bool modify_tool(SketchTool tool);
    // Clicks of these tools placing a point.
    void  click_modify(const Vec3d &point);
    // Line of a tangent circle picked.
    void  pick_line(size_t idx, const Vec3d &at);
    // Adds the shape clicked (or its whole path) to the objects, or removes it.
    void  toggle_object(size_t idx, bool whole_path);
    // The tool starts again from its objects to choose.
    void  restart_objects();
    // Ranges of consecutive objects.
    std::vector<std::pair<size_t, size_t>> object_ranges() const;
    // The objects moved by each transformation, as new paths; empty if there would be too many segments.
    std::vector<GCodeSketch::Path> object_copies(const std::vector<Transform3d> &transformations) const;
    // Transformations of the patterns, but the identity.
    std::vector<Transform3d> circular_transformations(const Vec3d &center) const;
    std::vector<Transform3d> rectangular_transformations() const;
    // Copies of the objects added at the end of the drawing, or the objects moved in place: one undo step. False if
    // nothing changed.
    bool  add_copies(const std::vector<Transform3d> &transformations);
    bool  transform_objects(const Transform3d &transformation);
    // The pattern, the offset or the scale shown, applied by OK.
    bool  apply_ready();
    // A change of the drawing which may do nothing: one undo step if it did something.
    bool  change_drawing(const std::function<bool(GCodeSketch::Drawing &)> &change);
    // Fillet or chamfer of the current tool at the corner: its shape, or the drawing changed. False if there is none.
    bool  corner_shape(const GCodeSketch::Corner &corner, GCodeSketch::Path &shape) const;
    bool  change_corner(GCodeSketch::Drawing &drawing, const GCodeSketch::Corner &corner) const;
    // Text tool: the outlines of the text in mm from the start of its baseline (x along the line, y up), and its paths
    // from point in the working plane.
    bool  text_outlines(const TextSettings &settings, std::vector<std::vector<Vec2d>> &outlines, wxString &error) const;
    std::vector<GCodeSketch::Path> text_paths(const Vec3d &point) const;
    // Asks the text and adds it at m_text_point.
    void  place_text();

    // Edition of the drawing.
    // Start of the segment idx, to change it: the start of the drawing or the end of the previous segment.
    Vec3d &start_of(size_t idx);
    GCodeSketch::Arc arc_of(size_t idx) const;
    void  delete_point(const GCodeSketch::PointRef &ref);
    void  translate_segments(size_t first, size_t last, const Vec3d &offset);
    // Side 0 or 1 of the rectangle starting with the segment first.
    Vec3d rectangle_side(size_t first, int side) const;
    void  resize_rectangle(size_t first, int side, double length);
    void  set_circle_diameter(size_t first, size_t last, double diameter);
    // First selected segment, nullptr if none.
    const GCodeSketch::Segment *first_selected() const;
    // Change every selected segment.
    void  for_selected(const std::function<void(GCodeSketch::Segment &)> &change);
    // A change of the drawing from the fields of the selection: one undo step.
    void  edit(const std::function<void()> &change);
    bool  selection_valid() const;
    // What the fields of the selection show: they are built again when it changes.
    std::string selection_signature() const;
    // Build the fields of the selection, after the event being processed (it may come from one of them).
    void  request_selection_ui();
    void  build_selection_ui();
    // Values of the fields of the selection, after a change of the drawing.
    void  update_selection_values();

    GCodeSketchView      *m_view          { nullptr };
    GCodeSketchToolbox   *m_toolbox       { nullptr };
    GCodeSketchPlaneBar  *m_plane_bar     { nullptr };
    wxScrolledWindow     *m_side          { nullptr };
    wxPanel              *m_selection_box { nullptr };
    wxStaticText         *m_hint_text     { nullptr };
    wxStaticText         *m_cursor_text   { nullptr };
    wxStaticText         *m_status_text   { nullptr };
    wxChoice             *m_printer       { nullptr };
    wxChoice             *m_filament      { nullptr };
    wxChoice             *m_grid          { nullptr };
    wxCheckBox           *m_snap_points   { nullptr };
    wxCheckBox           *m_snap_axes     { nullptr };
    wxCheckBox           *m_constraints   { nullptr };
    wxSpinCtrlDouble     *m_line_width    { nullptr };
    wxSpinCtrlDouble     *m_layer_height  { nullptr };
    wxSpinCtrlDouble     *m_print_speed   { nullptr };
    wxSpinCtrlDouble     *m_travel_speed  { nullptr };
    wxSpinCtrlDouble     *m_flow          { nullptr };
    wxSpinCtrlDouble     *m_retract       { nullptr };
    wxSpinCtrlDouble     *m_z_hop         { nullptr };
    wxSpinCtrl           *m_layers        { nullptr };
    wxCheckBox           *m_spiral        { nullptr };
    ::Button             *m_export_button { nullptr };

    std::vector<std::string> m_printer_names;
    std::vector<std::string> m_filament_names;
    std::string              m_printer_name;
    std::string              m_filament_name;

    GCodeSketch::Drawing              m_drawing;
    GCodeSketch::Drawing              m_stacked;
    GCodeSketch::Settings             m_settings;
    GCodeSketch::Machine              m_machine;
    GCodeSketch::ExportOptions        m_export;
    std::vector<GCodeSketch::Issue>   m_issues;
    std::vector<GCodeSketch::Drawing> m_undo;
    std::vector<GCodeSketch::Drawing> m_redo;
    // The redo steps before begin_change(), back if the change leaves the drawing as it was.
    std::vector<GCodeSketch::Drawing> m_change_redo;
    SketchTool                        m_tool          { SketchTool::Line };
    SketchPlane                       m_plane         { SketchPlane::Horizontal };
    bool                              m_extrude       { true };
    double                            m_plane_offset  { 0.2 };
    // Line being drawn: its first point, and the number of its segments, the last ones of the drawing.
    bool                              m_chain         { false };
    Vec3d                             m_chain_start   { Vec3d::Zero() };
    size_t                            m_chain_count   { 0 };
    std::vector<Vec3d>                m_clicks;
    std::vector<Pick>                 m_picks;
    int                               m_polygon_sides { 6 };
    double                            m_conic_rho     { 0.5 };
    // Modify tools: their objects (segments, sorted) and whether they are chosen.
    std::vector<size_t>               m_objects;
    bool                              m_objects_done  { false };
    int                               m_pattern_count { 6 };
    double                            m_pattern_angle { 360. };
    int                               m_grid_counts[2] { 3, 1 };
    double                            m_grid_spacing[2] { 20., 20. };
    double                            m_fillet_radius { 2. };
    double                            m_chamfer[2]    { 2., 2. };
    double                            m_chamfer_angle { 45. };
    double                            m_offset_distance { 1. };
    double                            m_scale_factor  { 1. };
    // Text tool: the last settings, the outlines of the last text (shown at the cursor), where the next one starts.
    TextSettings                      m_text_settings;
    std::vector<std::vector<Vec2d>>   m_text_outlines;
    Vec3d                             m_text_point    { Vec3d::Zero() };
    double                            m_sweep         { 0. };
    bool                              m_sweep_valid   { false };
    // Tangent arc: direction of the drawing at its start.
    Vec3d                             m_tangent       { Vec3d::Zero() };
    unsigned                          m_step          { 1 };
    SketchSelection                   m_selection;
    // Fields of the selection.
    std::vector<NumberField>          m_number_fields;
    std::vector<CheckField>           m_check_fields;
    std::vector<TextField>            m_text_fields;
    std::string                       m_selection_signature;
    bool                              m_selection_ui_pending { false };
    // The controls are being filled: their events are ignored.
    bool                              m_updating      { false };
    // Move in progress: the positions of the moved segments when it began.
    bool                              m_moving        { false };
    size_t                            m_move_first    { 0 };
    size_t                            m_move_last     { 0 };
    std::vector<Vec3d>                m_move_origin;
    // Last folder and file of the .gcode drawings.
    wxString                          m_last_dir;
    wxString                          m_last_file;
    // Texts of the column wrapped at its width, with their whole text: built once, and with the selection.
    std::vector<std::pair<wxStaticText*, wxString>> m_wrapped;
    std::vector<std::pair<wxStaticText*, wxString>> m_selection_wrapped;
    int                                             m_wrap_width { 0 };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GCodeSketchPanel_hpp_
