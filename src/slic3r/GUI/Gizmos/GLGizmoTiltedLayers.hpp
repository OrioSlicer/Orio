#ifndef slic3r_GLGizmoTiltedLayers_hpp_
#define slic3r_GLGizmoTiltedLayers_hpp_

// Orio: tool of the tilted layers (libslic3r/TiltedLayers.hpp).
// The zone printed with tilted layers is placed on the object with two planes, its first and its last tilted layers,
// as the planes of the cut tool. The transitions below and above the zone are computed and shown on the object. The
// overhangs made printable by the tilt are shown in green, the ones still needing support in red. The settings are
// applied to the object as soon as they are edited.

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TiltedLayers.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class ModelObject;

namespace GUI {

enum class SLAGizmoEventType : unsigned char;

class GLGizmoTiltedLayers : public GLGizmoBase
{
public:
    GLGizmoTiltedLayers(GLCanvas3D &parent, unsigned int sprite_id);

    std::string   get_icon_filename(bool is_dark_mode) const override;
    bool          gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;
    void          data_changed(bool is_serializing) override;
    BoundingBoxf3 get_bounding_box() const override;

    bool          wants_enter_leave_snapshots() const override { return true; }
    std::string   get_gizmo_entering_text() const override;
    std::string   get_gizmo_leaving_text() const override;

protected:
    bool               on_init() override;
    std::string        on_get_name() const override;
    bool               on_is_activable() const override;
    CommonGizmosDataID on_get_requirements() const override;
    void               on_set_state() override;
    void               on_start_dragging() override;
    void               on_stop_dragging() override;
    void               on_update(const UpdateData &data) override;
    void               on_render() override;
    void               on_render_for_picking() override;
    void               on_render_input_window(float x, float y, float bottom_limit) override;

private:
    enum GrabberId { StartGrabber = 0, EndGrabber = 1 };

    // Follow the selected object: reload its settings, its mesh and the printer settings when needed.
    void   update_object();
    void   read_printer_settings();
    void   default_settings();
    // Direction and zone of the overhangs of the object which need support with flat layers, false if it has none.
    bool   fit_overhangs();
    void   update_params();
    void   update_overlays();
    void   update_hover_model();
    void   update_grabbers() const;
    // Write the settings into the object configuration (or remove them if not applied).
    void   commit();
    // Undo snapshot taken once per edit of the panel.
    void   begin_edit(const std::string &snapshot);
    // Height of the layer (in the sliced part) under the mouse, false if the mouse is not over the object.
    bool   pick_layer(const Vec2d &mouse_position, double &zw) const;
    // Downhill direction of the layers (direction of the overhangs) on the bed.
    Vec2d  bed_direction() const;
    void   set_bed_direction(const Vec2d &dir);
    // Steepest tilt allowed by the print head in the current direction (degrees), 0 if unknown.
    double max_angle() const;
    // Angle of the preview: the one set, reduced to the clearance of the print head when it is known.
    double preview_angle() const;
    // Direction of the overhangs on the bed for which the print head clears the most, the closest to 'current' among
    // the equivalent ones. Zero if the clearance of the print head is unknown on every side.
    Vec2d  best_direction(const Vec2d &current) const;
    void   turn_to_best_side();
    // Heights of the first and of the last tilted layers of the zone, as previewed.
    double zone_start() const;
    double zone_end() const;
    // Point of a plane of the zone in the middle of the object, where its height is measured.
    Vec3d  plane_point(double height) const;
    void   render_overlays();
    void   render_planes();

    ModelObject         *m_object       { nullptr };
    ObjectID             m_object_id;
    // Timestamp of the configuration of the object read or written by the tool: it changes with undo / redo.
    uint64_t             m_config_timestamp { 0 };
    int                  m_instance_idx { -1 };
    Transform3d          m_instance_matrix { Transform3d::Identity() };
    size_t               m_volumes_count { 0 };
    // Model parts of the object in world coordinates and their bounding box.
    indexed_triangle_set m_mesh;
    BoundingBoxf3        m_mesh_box;
    // Settings asked in the tool, enabled if applied to the object.
    TiltedLayersSettings m_settings;
    // Parameters of the settings, computed even if they are not applied, to preview them.
    TiltedLayersParams   m_params;

    // Printer and print settings.
    HeadClearance        m_clearance;
    double               m_first_layer_height { 0.2 };
    double               m_layer_height       { 0.2 };
    double               m_max_layer_height   { 0. };
    double               m_support_angle      { 30. };

    // Overlays drawn on the object.
    bool                 m_overlays_dirty { true };
    std::vector<float>   m_vertex_zw;
    GLModel              m_transition_model;
    GLModel              m_fixed_model;
    GLModel              m_unsupported_model;
    GLModel              m_hover_model;
    GLModel              m_start_plane;
    GLModel              m_end_plane;
    double               m_fixed_area       { 0. };
    double               m_unsupported_area { 0. };
    double               m_hover_zw       { -1. };
    double               m_hover_model_zw { -1. };
    // "Fit to the overhangs" found none.
    bool                 m_no_overhangs { false };

    // Dragging of the planes.
    Vec3d                m_drag_start_point  { Vec3d::Zero() };
    double               m_drag_start_height { 0. };
    // An undo snapshot was taken for the current edit in the panel.
    bool                 m_edit_snapshot { false };
    float                m_last_y { 0.f };
    float                m_last_h { 0.f };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoTiltedLayers_hpp_
