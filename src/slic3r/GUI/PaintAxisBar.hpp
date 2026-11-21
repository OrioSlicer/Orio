#ifndef slic3r_GUI_PaintAxisBar_hpp_
#define slic3r_GUI_PaintAxisBar_hpp_

// Orio: the bar of the tools painting along an axis of an object, laid along the bottom of the 3D scene to
// paint along the X or Y axis of the object (libslic3r/PaintAxis.hpp). Same look and gestures as the vertical bar the
// tools keep on the right of the scene for Z: from left to right the extent of the object along its axis, colored by
// the painted value. Used by "Variable infill and walls"; meant to be reused by the other tools painting along an axis.
//
// The bar sits in a framed panel at the bottom of the scene, to the right of the navigation cube and about as high:
// a header (the title of the tool, the X / Y switch of the edited axis, hints), the bar, then its graduations in mm.
// The bar runs from right to left when the axis points to the left on the screen, so that it always matches the
// object under it. Along Z (is_xy() false), the same panel stands at the right of the scene, below the main toolbar:
// the title, then the bar going up, its graduations on its left.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/PaintAxis.hpp"
#include "libslic3r/Point.hpp"
#include "GLModel.hpp"
#include "Gizmos/GLGizmosManager.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class GLShaderProgram;
class GLVolume;
class ModelObject;
class ModelVolume;
class TriangleMesh;

namespace GUI {

class GLCanvas3D;
class MeshRaycaster;
struct PaintAxisBarLayout;

class PaintAxisBar
{
public:
    // Thickness of the bar before the screen scale, so that its panel is about as high as the navigation cube.
    static constexpr float THICKNESS = 56.0f;

    // Painting along X / Y (the bar at the bottom), or along Z (the vertical bar of the tool).
    bool      is_xy() const { return m_xy; }
    void      set_xy(bool xy) { m_xy = xy; }

    // Extents of the object along its own X and Y axes (paint_axis_object_extent()). Call it when attaching to an
    // object or when the object may have changed.
    void      set_object(const ModelObject *object);
    std::pair<double, double> extent(PaintAxis axis) const { return axis == paY ? m_extent_y : m_extent_x; }

    // The edited axis (X or Y, chosen with the switch of render_panel()), and its extent along the bar; along Z, from 0
    // to the height given by set_height() (to call at every frame, the object may change).
    PaintAxis axis() const { return m_axis; }
    void      set_axis(PaintAxis axis) { if (axis == paX || axis == paY) m_axis = axis; }
    void      set_height(double height) { m_height = height; }
    double    origin() const { return m_xy ? this->extent(m_axis).first : 0.; }
    double    length() const;
    // How the edited axis of the object appears on the screen: pointing to the left (the bar then runs from right to
    // left, matching the object), and how horizontal it looks. To call at every frame, the view or the object may turn.
    void      update_orientation(const GLCanvas3D &canvas, const ModelObject &object, int instance_idx);
    bool      flipped() const { return m_xy && m_flipped; }
    // How horizontal the axis looks on the screen, from 0 (looking along the axis) to 1.
    float     alignment() const { return m_alignment; }
    // Margins at the start and at the end of the axis, in mm, hatched ("Variable infill and walls": the walls at both
    // ends of the object along X / Y, its bottom and top shells along Z, which hold no infill). Along X / Y they are
    // parts of the axis that cannot be painted, keeping their place on the bar, at most 40 % of the axis each; a
    // position there goes to the nearest one that can be painted. Along Z they are drawn outside the bar instead, under
    // it and above it, at its scale but at least a padding high: the bar keeps the whole axis, all of it paintable, and
    // a position in them goes to the end of the bar.
    void      set_margins(double start, double end);
    double    margin_start() const { return m_margin_start; }
    double    margin_end() const { return m_margin_end; }
    // The part of the axis that can be painted (all of it along Z), from 0 (the origin of the axis) to 1 (its end), and
    // a position along the axis brought into it.
    std::pair<float, float> paintable_range() const;
    float     paintable(float relative) const;

    // Thickness of the bar on the screen, in pixels.
    static float thickness(const GLCanvas3D &canvas);
    // The bar in screen coordinates (y down), and in the viewport coordinates of the overlays (centered, y up).
    Rect      rect_screen(const GLCanvas3D &canvas) const;
    Rect      rect_viewport(const GLCanvas3D &canvas) const;
    // Whether a screen point is over the bar, or beside its ends in the panel: a stroke may start just past them.
    bool      contains(const GLCanvas3D &canvas, float x, float y) const;
    // Height kept at the bottom of the scene for the panel and the value bubble over it, margins included: what is
    // shown at the bottom of the scene (notifications, buttons) goes above them. 0 along Z.
    float     reserved_height(const GLCanvas3D &canvas) const;
    // Along Z, the width kept at the right of the scene for the panel, margin included: the notifications go on its
    // left. 0 along X / Y.
    float     reserved_width(const GLCanvas3D &canvas) const;
    // Position along the axis, 0 at its origin and 1 at its end, of a screen point (not clamped; a drag may leave the
    // bar) or abscissa (along X / Y only), of a screen point over the bar (in the part that can be painted; -1000 off
    // the bar), of the mouse (same).
    float     relative_along(const GLCanvas3D &canvas, float x, float y) const;
    float     relative_along(const GLCanvas3D &canvas, float x) const;
    float     relative_at(const GLCanvas3D &canvas, float x, float y) const;
    float     cursor_relative(const GLCanvas3D &canvas) const;

    // GL, before the bar: the background of the panel, a rounded rectangle of the color of the ImGui windows.
    void      render_panel_background(const GLCanvas3D &canvas) const;
    // Draws the bar with the variable_layer_height shader, bound and given the texture uniforms by the caller.
    void      render_background(const GLCanvas3D &canvas, GLShaderProgram &shader) const;
    // Matrix to pass as "volume_world_matrix" to the variable_layer_height shader: the "z" it computes for a vertex of
    // the volume is then its coordinate along the edited axis, from the origin of the axis.
    Transform3d volume_axis_matrix(const GLVolume &volume) const;
    // ImGui, after the bar: the outline of the panel; its header, with the title of the tool in bold, the X / Y switch
    // of the edited axis, where its coordinate grows on the screen and, on the right, a hint when looking along the
    // axis and whether the other axis is painted too; the graduations under the bar. Along Z, the header holds the title
    // only, over as many lines as needed, and the graduations go on the left of the bar. The header and the graduations
    // keep the mouse off the scene. Returns the axis the user clicked to edit, paZ if none.
    PaintAxis render_panel(const GLCanvas3D &canvas, const std::string &title, bool x_painted, bool y_painted) const;
    // ImGui: the value under the mouse, above the panel (on its left along Z) rather than as a tooltip next to the
    // mouse, which would hide the bar. In front of the other windows (notifications). relative: where the value is along
    // the bar (0 at its start, 1 at its end) while the mouse is off the panel, over the object: the bubble goes there;
    // out of [0, 1] for none.
    void      render_value_tip(const GLCanvas3D &canvas, const std::string &text, float relative = -1.f) const;

    // Painting on the object itself: the coordinate along the edited axis (0 at its origin, as along the bar) of the point
    // of the object under a point of the screen, as the object is colored: along Z its height in the scene, along X / Y
    // its coordinate in the frame of the object (volume_axis_matrix()); NaN off its solid parts. The nearest solid part
    // hit counts, on the selected instance. Their ray casters are kept while their meshes stay the same; release_parts()
    // frees them (the tool closes).
    float     part_coordinate_at(const GLCanvas3D &canvas, const ModelObject &object, float x, float y) const;
    void      release_parts() { m_part_raycasters.clear(); }

    // What render_bounds() did: a bound follows the mouse (Dragging), or a change for the tool to commit, one undo step
    // each (the others).
    enum class BoundsEdit { None, Dragging, Moved, Added, Deleted, DeletedAll };
    // Between two bounds, and from the ends of the bar, in mm.
    static constexpr double BOUND_MIN_GAP = 0.05;
    // ImGui, after render_panel(): the bounds the painting does not cross, along the bar (0 at its start), sorted; the
    // tool keeps them and applies them. Over the graduations (left of the bar along Z, under it along X / Y), a right
    // click opens their menu (place a bound there, delete the one there, delete them all) and a left click on one drags
    // it between its neighbours; along Z a bound goes to the top of a layer of the object, sliced as config says. Drawn
    // over the bar; the one under the mouse or dragged is labelled with its layer and its height (Z) or its coordinate
    // (X / Y).
    BoundsEdit render_bounds(const GLCanvas3D &canvas, const ModelObject &object, const DynamicPrintConfig *config, std::vector<double> &bounds);

    // ImGui, in a tool window: the "Z / X Y" toggle. z_edited / xy_edited mark the other choice when it has painted
    // curves. Returns true when the user changed it.
    static bool render_axis_toggle(bool &xy, bool z_edited, bool xy_edited);

private:
    PaintAxisBarLayout layout(const GLCanvas3D &canvas) const;

    bool      m_xy        { false };
    PaintAxis m_axis      { paX };
    bool      m_flipped   { false };
    float     m_alignment { 1.0f };
    std::pair<double, double> m_extent_x { 0., 0. };
    std::pair<double, double> m_extent_y { 0., 0. };
    double    m_margin_start { 0. };
    double    m_margin_end   { 0. };
    double    m_height       { 0. };
    // Along Z, the height of the title, measured by render_panel(), which the layout follows from the next frame.
    mutable float m_header_height { 0.f };

    // The solid parts of the object painted on (part_coordinate_at()).
    struct PartRaycaster
    {
        const ModelVolume                  *volume { nullptr };
        std::shared_ptr<const TriangleMesh> mesh;
        std::shared_ptr<MeshRaycaster>      raycaster;
    };
    mutable std::vector<PartRaycaster> m_part_raycasters;
    // The bound dragged (render_bounds(), its index), -1 for none. Where the menu of the bounds was opened: the coordinate
    // along the bar, and the bound there (-1 for none).
    int    m_bound_drag             { -1 };
    double m_bounds_menu_coordinate { 0. };
    int    m_bounds_menu_bound      { -1 };

    mutable GLModel m_background;
    mutable bool    m_background_flipped  { false };
    mutable bool    m_background_vertical { false };
    mutable GLModel m_panel;
    // Width, height and corner radius m_panel was made for.
    mutable Vec3f   m_panel_shape { 0.f, 0.f, 0.f };
};

} // namespace GUI
} // namespace Slic3r

#endif /* slic3r_GUI_PaintAxisBar_hpp_ */
