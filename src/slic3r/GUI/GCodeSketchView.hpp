#ifndef slic3r_GCodeSketchView_hpp_
#define slic3r_GCodeSketchView_hpp_

// Orio: 3D view of the "G-code drawing" tab (GCodeSketchPanel). It shows the bed and the build volume of the
// chosen printer, the drawing and its other layers, and the working plane (horizontal, front or side). The tools work
// as the sketch tools of Fusion 360: each click places a point where the mouse meets the working plane, held by the
// constraints the cursor meets: the ends, middles and centers of the drawing, the X, Y and Z axes of the last point,
// the alignment with the other points, perpendicular, parallel or tangent to the last segment; Ctrl frees it from all
// of them. While drawing, the dimensions of the shape show on it, as in Fusion 360, and the typed numbers go into them
// (length and angle, sides, diameter, radius, width...): Tab goes to the next one and locks the value typed, Enter
// places the point. Shift moves the point along the normal of the plane; Alt goes to the next plane, the middle button
// dragged on its grid moves it. Each point has a stem down to the bed and the drawing casts its shadow on it, so that
// the heights read without turning the camera. The modify tools show what they would do under the mouse: the part a trim
// removes in red, the fillet or the chamfer of the corner, the extension, the points of a break, the copies of a mirror
// or of a pattern, the offset, the shapes moved, rotated or scaled.

#include "Camera.hpp"
#include "GLModel.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/GCodeSketch.hpp"

#include <wx/string.h>

#include <string>
#include <vector>

class wxGLCanvas;
class wxGLContext;
class wxWindow;
class wxMouseEvent;
class wxKeyEvent;
class wxPaintEvent;
class wxSizeEvent;
class wxMouseCaptureLostEvent;
class wxFocusEvent;
class wxPoint;
struct ImDrawList;

namespace Slic3r {
namespace GUI {

class GCodeSketchPanel;

class GCodeSketchView
{
public:
    GCodeSketchView(GCodeSketchPanel &panel, wxWindow *parent);
    ~GCodeSketchView();

    // Window of the view, nullptr if OpenGL is unavailable.
    wxWindow *window() const;

    // The printer changed: rebuild the bed and frame it.
    void on_machine_changed();
    // The drawing or its settings changed.
    void on_drawing_changed();
    // The selection changed: its highlight.
    void on_selection_changed();
    void refresh();
    void zoom_to_bed();
    // "iso", "top", "front", "left", "right"...
    void select_view(const std::string &direction);
    void toggle_camera_type();
    // Update the cursor where the mouse is, after a change of the tool, of the plane or of the drawing.
    void update_cursor_here();
    // Release the OpenGL resources while the OpenGL context is still valid.
    void release_gl();

private:
    // Plane: the working plane dragged along its normal with the middle button.
    enum class Drag { None, Rotate, Pan, MovePoint, MoveSegments, Arc, Plane };
    // What holds the cursor.
    enum class Snap { None, Grid, End, Passage, Middle, Center, Axis, Perpendicular, Parallel, Tangent, Normal };
    // Values typed while drawing. Sweep: angle swept by an arc, a circular pattern or a rotation; Sides: of a polygon; Rho:
    // of a conic; Instances: copies of a pattern (along the first axis of the plane), InstancesV along the second one, and
    // their spacings; Distance, Distance2: of a chamfer, of an offset; Factor: of a scale.
    enum class FieldType {
        Length,
        Angle,
        SideU,
        SideV,
        Diameter,
        Radius,
        Width,
        Sweep,
        Sides,
        Rho,
        Instances,
        InstancesV,
        SpacingU,
        SpacingV,
        Distance,
        Distance2,
        Factor,
        Count
    };
    // Segments highlighted under the mouse: those a click selects there (a shape), the segment alone, its whole path.
    enum class HoverSpan { Shape, Alone, Path };
    // How a dimension is drawn on the shape, as in Fusion 360.
    enum class DimensionKind {
        // Its box by the cursor.
        None,
        // From a to b, its line beside them.
        Linear,
        // At a, from the direction of b, turning by angle around axis.
        Angular,
        // From the center a to b.
        Radial,
        // Through the center a, from the opposite of b to b.
        Diametral,
        // Its box at a.
        Label,
    };
    struct Dimension
    {
        DimensionKind kind  { DimensionKind::None };
        Vec3d         a     { Vec3d::Zero() };
        Vec3d         b     { Vec3d::Zero() };
        // Angular: radians, counterclockwise around axis when positive.
        double        angle { 0. };
        Vec3d         axis  { Vec3d::UnitZ() };
    };

    // Point of the drawing the cursor snaps to.
    struct Target
    {
        Vec3d position;
        Snap  kind;
    };
    // The cursor has the coordinate 'coordinate' of the point 'from': a dashed line joins them.
    struct Alignment
    {
        Vec3d from;
        int   coordinate;
    };
    struct Field
    {
        FieldType type;
        // Typed, empty to follow the mouse.
        wxString  text;
        // Kept when going to another field (Tab).
        bool      locked { false };
        // Value of the cursor, shown while nothing is typed.
        double    value  { 0. };
        // The value means nothing for this cursor (angle of a point out of the plane).
        bool      unknown { false };
        // Where it is drawn.
        Dimension dimension;
    };
    // Screen position of points, in pixels from the top left of the view.
    struct Projector
    {
        Matrix4d matrix;
        double   x { 0. };
        double   y { 0. };
        double   width { 1. };
        double   height { 1. };
        // False if the point is behind the camera.
        bool operator()(const Vec3d &point, Vec2d &screen) const;
    };

    void on_paint(wxPaintEvent &evt);
    void on_size(wxSizeEvent &evt);
    void on_mouse(wxMouseEvent &evt);
    void on_wheel(wxMouseEvent &evt);
    void on_key_down(wxKeyEvent &evt);
    void on_key_up(wxKeyEvent &evt);
    void on_char(wxKeyEvent &evt);
    void on_capture_lost(wxMouseCaptureLostEvent &evt);
    void on_kill_focus(wxFocusEvent &evt);
    // Right click: the commands of the tool and the tools, as the menus of Fusion 360.
    void show_menu(const wxPoint &position);

    void render();
    void render_overlay(int width, int height);
    // The dimensions of the shape being drawn, on it, with their values.
    void draw_fields(ImDrawList &draw, const Projector &project, const Vec2d &cursor, int width, int height) const;
    void draw_glyph(ImDrawList &draw, const Vec2d &cursor) const;
    bool make_current();
    void build_machine_models();
    void build_plane_models(int axis);
    void build_drawing_models();
    void build_highlights();
    void build_previews();
    void build_targets();
    void reset_models();

    Projector projector() const;
    Vec2d mouse_position(const wxMouseEvent &evt) const;
    // Point where the ray under the mouse meets the plane of normal axis at this offset.
    bool  mouse_on_plane(const Vec2d &mouse, int axis, double offset, Vec3d &hit) const;
    // Parameter of the point of the line origin + t * direction the closest to the ray under the mouse.
    bool  mouse_on_line(const Vec2d &mouse, const Vec3d &origin, const Vec3d &direction, double &t) const;
    // Point of an axis through the anchor that the cursor follows when the mouse is near its image: the axes of the
    // plane (normal to the axis of the working plane, at plane_offset) when it goes through the anchor, and its normal
    // unless in_plane_only.
    bool  infer_axis(const Vec2d &mouse, const Vec3d &anchor, double plane_offset, bool in_plane_only, Vec3d &point, int &axis) const;
    // Direction from the anchor perpendicular, parallel or tangent to the last segment of the line being drawn.
    bool  infer_direction(const Vec2d &mouse, const Vec3d &anchor, Vec3d &point, Snap &kind) const;
    // Third point of an arc on the arc tangent to the segment ending at its start.
    bool  infer_tangent_arc(const Vec2d &mouse, Vec3d &point) const;
    // Coordinates of the cursor taken from the points of the drawing it lines up with.
    void  infer_alignments(const Vec2d &mouse, const Vec3d &anchor, bool has_anchor, Vec3d &point);
    // Index in list_points() of the point drawn under the mouse, -1 if none.
    int   point_under_mouse(const Vec2d &mouse) const;
    // Index in m_targets of the point the cursor snaps to under the mouse, -1 if none.
    int   target_under_mouse(const Vec2d &mouse) const;
    // Index of the segment drawn under the mouse, -1 if none; at: its point there.
    int   segment_under_mouse(const Vec2d &mouse, Vec3d *at = nullptr) const;
    // Straight line of the drawing under the mouse, for the tangent circles, -1 if none; at: its point under the mouse.
    int   line_under_mouse(const Vec2d &mouse, Vec3d &at) const;
    // The mouse is over the grid of the working plane, in the build volume: hit, the point of the plane there.
    bool  on_plane_grid(const Vec2d &mouse, Vec3d &hit) const;
    // Coordinate snapped to the grid (X, Y) or to the layer height (Z).
    double snap_along(double value, int axis) const;
    // Point snapped along the axes of the plane of normal axis.
    Vec3d snap_in_plane(const Vec3d &point, int axis) const;
    Vec3d clamp_to_volume(const Vec3d &point) const;
    // Follow the mouse with the cursor. Shift: along the normal of the plane; unconstrained: without any snap or constraint (Ctrl).
    void  update_cursor(const Vec2d &mouse, bool shift, bool unconstrained);
    void  hide_cursor();
    // Segment under the mouse, highlighted with the segments a click selects there, alone, or with its path.
    void  set_hover_segment(int segment, HoverSpan span = HoverSpan::Shape);
    // Size in mm of a size in pixels in the view.
    double pixels_to_mm(double pixels) const;

    // Typed values.
    // The fields of the step of the current tool, emptied when the step changes.
    void  sync_fields();
    std::vector<FieldType> field_types() const;
    // The typed values move the cursor; the others show where it is.
    void  apply_fields(Vec3d &point);
    bool  field_value(const Field &field, double &value) const;
    bool  has_typed_value() const;
    void  type_char(wxChar c);
    void  erase_char();
    void  next_field(int direction);
    wxString snap_text() const;

    GCodeSketchPanel &m_panel;
    wxGLCanvas       *m_canvas  { nullptr };
    wxGLContext      *m_context { nullptr };
    Camera            m_camera;
    bool              m_rendered      { false };
    bool              m_zoom_pending  { true };
    // Box of the near and far planes of the camera: the build volume and the drawing.
    BoundingBoxf3     m_projection_box;
    bool              m_machine_dirty   { true };
    bool              m_drawing_dirty   { true };
    bool              m_highlight_dirty { true };

    // Bed and build volume, in the coordinates of the G-code.
    Vec2d   m_bed_min { Vec2d::Zero() };
    Vec2d   m_bed_max { Vec2d(256., 256.) };
    GLModel m_bed;
    GLModel m_bed_grid;
    GLModel m_bed_outline;
    GLModel m_exclude;
    GLModel m_volume;
    // Working plane at 0 along its normal, moved to its position when rendered; built for the normal m_plane_model_axis.
    GLModel m_plane;
    GLModel m_plane_grid;
    int     m_plane_model_axis { -1 };
    // Drawing, and its other layers.
    GLModel m_beads;
    GLModel m_print_lines;
    GLModel m_travel_lines;
    GLModel m_layer_lines;
    GLModel m_shadow;
    // Selected segments, and the ones a click selects under the mouse.
    GLModel m_selected_lines;
    GLModel m_hover_lines;
    // Unit shapes placed by their matrix.
    GLModel m_sphere;
    GLModel m_disc;
    GLModel m_unit_x;
    GLModel m_unit_y;
    GLModel m_unit_z;
    // Segment or shape being drawn, and the travel move to its start and its construction lines.
    GLModel m_rubber;
    GLModel m_ghost;
    // Lines picked by the tangent circles.
    GLModel m_picked;
    // What a modify tool removes, and the points where a curve is split.
    GLModel            m_removed;
    std::vector<Vec3d> m_marks;

    // Mouse.
    Drag                  m_drag            { Drag::None };
    Vec2d                 m_down_position   { Vec2d::Zero() };
    Vec2d                 m_last_position   { Vec2d::Zero() };
    bool                  m_moved           { false };
    int                   m_down_button     { 0 };
    // The button was pressed for the second click of a double click.
    bool                  m_double_click    { false };
    // Point or segment pressed by the Select tool, moved if the mouse is dragged.
    int                   m_pressed_point   { -1 };
    int                   m_pressed_segment { -1 };
    // The Line tool pressed the end of its line: dragging draws a tangent arc.
    bool                  m_arc_armed       { false };
    GCodeSketch::PointRef m_moved_point;
    // Moved segments: the point grabbed, on the plane parallel to the working plane through it.
    Vec3d                 m_grab            { Vec3d::Zero() };
    // Middle button pressed on the grid of the working plane: dragging moves the plane. The point grabbed, and the
    // position of the plane then.
    bool                  m_plane_armed       { false };
    Vec3d                 m_plane_grab        { Vec3d::Zero() };
    double                m_plane_grab_offset { 0. };
    // Alt pressed alone: the next plane when it is released.
    bool                  m_alt_alone         { false };
    // Digit of the key of the top row just pressed, typed when the character it gives belongs to no number (é, &...
    // of a French keyboard without Shift); -1 for another key.
    int                   m_digit_key         { -1 };

    // Cursor: where a click places the next point.
    bool    m_has_cursor     { false };
    Vec3d   m_cursor         { Vec3d::Zero() };
    Snap    m_snap           { Snap::None };
    // Cursor out of the bed or of its height.
    bool    m_cursor_outside { false };
    // Axis through m_anchor followed by the cursor, -1 if none.
    int     m_inferred_axis  { -1 };
    Vec3d   m_anchor         { Vec3d::Zero() };
    std::vector<Alignment> m_alignments;
    // Last position of the mouse in the view: typed values place the point from there.
    bool    m_has_mouse      { false };
    Vec2d   m_mouse          { Vec2d::Zero() };
    // Last point of the working plane under the mouse: Shift moves the cursor along the normal of the plane from there.
    bool    m_has_base       { false };
    Vec3d   m_base           { Vec3d::Zero() };
    int     m_hover_point    { -1 };
    int     m_hover_segment  { -1 };
    // Segment a click of a modify tool picks (an object, a path, a curve, a corner, a mirror line), -1 if none, and its
    // point under the mouse; the values typed are then shown by the mouse.
    int     m_pick_segment   { -1 };
    Vec3d   m_pick_at        { Vec3d::Zero() };
    bool    m_fields_at_mouse { false };
    // Step of the panel when an object was last clicked: the second click of a double click adds its whole path only if
    // the first one chose an object too (not when it applied the tool).
    unsigned m_object_click_step { 0 };
    // m_cursor was placed once: Enter places the values typed from it when the mouse is off the plane.
    bool     m_cursor_known      { false };
    size_t  m_hover_first    { 0 };
    size_t  m_hover_last     { 0 };
    // Points the cursor snaps to, made from the drawing.
    std::vector<Target> m_targets;
    bool                m_targets_dirty { true };

    // Typed values, for the step m_fields_step of the panel.
    std::vector<Field> m_fields;
    int                m_active_field { 0 };
    unsigned           m_fields_step  { 0 };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GCodeSketchView_hpp_
