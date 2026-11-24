#ifndef slic3r_GLCanvas3D_hpp_
#define slic3r_GLCanvas3D_hpp_

#include <stddef.h>
#include <memory>
#include <chrono>
#include <cstdint>
#include <set>
#include <stack>
#include <vector>

#include "Event.hpp"
#include "Selection.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "GUI_ObjectLayers.hpp"
#include "GLSelectionRectangle.hpp"
#include "MeshUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "Camera.hpp"
#include "IMToolbar.hpp"
#include "slic3r/GUI/3DBed.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/InfillWallsProfile.hpp"
#include "PaintAxisBar.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "GLEnums.hpp"
#include "PrepareCut.hpp"

#include <float.h>

#include <wx/timer.h>

class wxSizeEvent;
class wxIdleEvent;
class wxKeyEvent;
class wxMouseEvent;
class wxTimerEvent;
class wxPaintEvent;
class wxGLCanvas;
class wxGLContext;

// Support for Retina OpenGL on Mac OS.
// wxGTK3 seems to simulate OSX behavior in regard to HiDPI scaling support, enable it as well.
#define ENABLE_RETINA_GL (__APPLE__ || __WXGTK3__)

namespace Slic3r {

class BackgroundSlicingProcess;
class BuildVolume;
struct ThumbnailData;
struct ThumbnailsParams;
class ModelObject;
class ModelInstance;
struct TextInfo;
class PrintObject;
class Print;
class SLAPrint;
class PresetBundle;
namespace CustomGCode { struct Item; }

namespace GUI {

namespace gcode {
    class GCodeViewer;
};
class PartPlateList;
class OpenGLManager;
class GLToolbar;
class GLToolbarItem;

#if ENABLE_RETINA_GL
class RetinaHelper;
#endif

class Size
{
    int m_width;
    int m_height;
    float m_scale_factor;

public:
    Size();
    Size(int width, int height, float scale_factor = 1.0);

    int get_width() const;
    void set_width(int width);

    int get_height() const;
    void set_height(int height);

    int get_scale_factor() const;
    void set_scale_factor(int height);
};


class RenderTimerEvent : public wxEvent
{
public:
    RenderTimerEvent(wxEventType type, wxTimer& timer)
        : wxEvent(timer.GetId(), type),
        m_timer(&timer)
    {
        SetEventObject(timer.GetOwner());
    }
    int GetInterval() const { return m_timer->GetInterval(); }
    wxTimer& GetTimer() const { return *m_timer; }

    virtual wxEvent* Clone() const { return new RenderTimerEvent(*this); }
    virtual wxEventCategory GetEventCategory() const  { return wxEVT_CATEGORY_TIMER; }
private:
    wxTimer* m_timer;
};

class  ToolbarHighlighterTimerEvent : public wxEvent
{
public:
    ToolbarHighlighterTimerEvent(wxEventType type, wxTimer& timer)
        : wxEvent(timer.GetId(), type),
        m_timer(&timer)
    {
        SetEventObject(timer.GetOwner());
    }
    int GetInterval() const { return m_timer->GetInterval(); }
    wxTimer& GetTimer() const { return *m_timer; }

    virtual wxEvent* Clone() const { return new ToolbarHighlighterTimerEvent(*this); }
    virtual wxEventCategory GetEventCategory() const { return wxEVT_CATEGORY_TIMER; }
private:
    wxTimer* m_timer;
};


class  GizmoHighlighterTimerEvent : public wxEvent
{
public:
    GizmoHighlighterTimerEvent(wxEventType type, wxTimer& timer)
        : wxEvent(timer.GetId(), type),
        m_timer(&timer)
    {
        SetEventObject(timer.GetOwner());
    }
    int GetInterval() const { return m_timer->GetInterval(); }
    wxTimer& GetTimer() const { return *m_timer; }

    virtual wxEvent* Clone() const { return new GizmoHighlighterTimerEvent(*this); }
    virtual wxEventCategory GetEventCategory() const { return wxEVT_CATEGORY_TIMER; }
private:
    wxTimer* m_timer;
};


wxDECLARE_EVENT(EVT_GLCANVAS_OBJECT_SELECT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_PLATE_NAME_CHANGE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_MOVE_PLATE, SimpleEvent);
//BBS: declare EVT_GLCANVAS_PLATE_SELECT
wxDECLARE_EVENT(EVT_GLCANVAS_PLATE_SELECT, SimpleEvent);

using Vec2dEvent = Event<Vec2d>;
// _bool_ value is used as a indicator of selection in the 3DScene
using RBtnEvent = Event<std::pair<Vec2d, bool>>;
using RBtnPlateEvent = Event<std::pair<Vec2d, int>>;
template <size_t N> using Vec2dsEvent = ArrayEvent<Vec2d, N>;

using Vec3dEvent = Event<Vec3d>;
template <size_t N> using Vec3dsEvent = ArrayEvent<Vec3d, N>;

using HeightProfileSmoothEvent = Event<HeightProfileSmoothingParams>;

wxDECLARE_EVENT(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RIGHT_CLICK, RBtnEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_PLATE_RIGHT_CLICK, RBtnPlateEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_REMOVE_OBJECT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ARRANGE, SimpleEvent);
//BBS: add arrange and orient event
wxDECLARE_EVENT(EVT_GLCANVAS_ARRANGE_PARTPLATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ARRANGE_OUTPLATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ORIENT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ORIENT_PARTPLATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SELECT_CURR_PLATE_ALL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SELECT_ALL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_QUESTION_MARK, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_INCREASE_INSTANCES, Event<int>); // data: +1 => increase, -1 => decrease
wxDECLARE_EVENT(EVT_GLCANVAS_INSTANCE_MOVED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_FORCE_UPDATE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_INSTANCE_ROTATED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_INSTANCE_SCALED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, Event<bool>);
wxDECLARE_EVENT(EVT_GLCANVAS_UPDATE_GEOMETRY, Vec3dsEvent<2>);
wxDECLARE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_STARTED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_UPDATE_BED_SHAPE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_TAB, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RESETGIZMOS, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_MOVE_SLIDERS, wxKeyEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_EDIT_COLOR_CHANGE, wxKeyEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_JUMP_TO, wxKeyEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_UNDO, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_REDO, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SWITCH_TO_OBJECT, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SWITCH_TO_GLOBAL, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_COLLAPSE_SIDEBAR, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RELOAD_FROM_DISK, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RENDER_TIMER, wxTimerEvent/*RenderTimerEvent*/);
wxDECLARE_EVENT(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_UPDATE, SimpleEvent);
wxDECLARE_EVENT(EVT_CUSTOMEVT_TICKSCHANGED, wxCommandEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, Event<float>);
wxDECLARE_EVENT(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, HeightProfileSmoothEvent);
//BBS: variable print speed along Z.
wxDECLARE_EVENT(EVT_GLCANVAS_RESET_SPEED_PROFILE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SMOOTH_SPEED_PROFILE, HeightProfileSmoothEvent);
//Orio: variable sparse infill density / wall count along Z.
wxDECLARE_EVENT(EVT_GLCANVAS_RESET_INFILL_WALLS_PROFILE, SimpleEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_SMOOTH_INFILL_WALLS_PROFILE, HeightProfileSmoothEvent);

class GLCanvas3D
{
    static const double DefaultCameraZoomToBoxMarginFactor;
    static const double DefaultCameraZoomToBedMarginFactor;
    static const double DefaultCameraZoomToPlateMarginFactor;


    static float DEFAULT_BG_LIGHT_COLOR[3];
    static float ERROR_BG_LIGHT_COLOR[3];
    static float DEFAULT_BG_LIGHT_COLOR_LIGHT[3];
    static float ERROR_BG_LIGHT_COLOR_LIGHT[3];
    static float DEFAULT_BG_LIGHT_COLOR_DARK[3];
    static float ERROR_BG_LIGHT_COLOR_DARK[3];

    static void update_render_colors();
    static void load_render_colors();

    class LayersEditing
    {
    public:
        enum EState : unsigned char
        {
            Unknown,
            Editing,
            Completed,
            Num_States
        };

    private:
        bool                        m_enabled{ false };
        unsigned int                m_z_texture_id{ 0 };
        // Not owned by LayersEditing.
        const DynamicPrintConfig* m_config{ nullptr };
        // ModelObject for the currently selected object (Model::objects[last_object_id]).
        const ModelObject* m_model_object{ nullptr };
        // Maximum z of the currently selected object (Model::objects[last_object_id]).
        float                       m_object_max_z{ 0.0f };
        // Owned by LayersEditing.
        SlicingParameters* m_slicing_parameters{ nullptr };
        std::vector<double>         m_layer_height_profile;

        mutable float               m_adaptive_quality{ 0.5f };
        mutable HeightProfileSmoothingParams m_smooth_params;
        // Orio: "Hide the other objects" (hides()), remembered in the app config.
        bool                        m_hide_other_objects{ false };

        static float                s_overlay_window_width;

        struct LayersTexture
        {
            // Texture data
            std::vector<char>   data;
            // Width of the texture, top level.
            size_t              width{ 0 };
            // Height of the texture, top level.
            size_t              height{ 0 };
            // For how many levels of detail is the data allocated?
            size_t              levels{ 0 };
            // Number of texture cells allocated for the height texture.
            size_t              cells{ 0 };
            // Does it need to be refreshed?
            bool                valid{ false };
        };
        LayersTexture   m_layers_texture;

        // Orio: the bar in its framed panel at the right of the scene (PaintAxisBar, always vertical here), as
        // the variable speed tool.
        PaintAxisBar    m_axis_bar;

        mutable GLModel m_baseline;
        mutable GLModel m_profile_curve;
        mutable bool m_profile_dirty{ true };

        // Orio: painting on the part itself, as SpeedEditing. A ray caster per solid part of the edited object,
        // kept while its mesh stays the same.
        struct PartRaycaster
        {
            const ModelVolume*                  volume{ nullptr };
            std::shared_ptr<const TriangleMesh> mesh;
            std::shared_ptr<MeshRaycaster>      raycaster;
        };
        mutable std::vector<PartRaycaster> m_part_raycasters;

        // Orio: the bound dragged over the graduations (index in active_bounds()) and the bounds while it is dragged,
        // -1 otherwise; where the menu of the bounds was opened: the height, and the bound there (-1 for none).
        int                 m_bound_drag{ -1 };
        std::vector<double> m_drag_bounds;
        double              m_bounds_menu_coordinate{ 0. };
        int                 m_bounds_menu_bound{ -1 };

    public:
        EState state{ Unknown };
        float band_width{ 2.0f };
        float strength{ 0.005f };
        int last_object_id{ -1 };
        // Orio: NaN while a stroke started on the part is off it (the stroke pauses).
        float last_z{ 0.0f };
        // Orio: the stroke started on the part, not on the bar.
        bool part_stroke{ false };
        LayerHeightEditActionType last_action{ LAYER_HEIGHT_EDIT_ACTION_INCREASE };

        LayersEditing(GLCanvas3D &canvas) : m_canvas(canvas) {}
        ~LayersEditing();

        void init();

        void set_config(const DynamicPrintConfig* config);
        void select_object(const Model& model, int object_id);

        bool is_allowed() const;

        bool is_enabled() const;
        void set_enabled(bool enabled);

        void show_tooltip_information(const GLCanvas3D& canvas, std::map<wxString, wxString> captions_texts, float x, float y);
        void render_variable_layer_height_dialog(const GLCanvas3D& canvas);
        void render_overlay(const GLCanvas3D& canvas);
        void render_volumes(const GLCanvas3D& canvas, const GLVolumeCollection& volumes);

        void adjust_layer_height_profile();
        void accept_changes(GLCanvas3D& canvas);
        void reset_layer_height_profile(GLCanvas3D& canvas);
        void adaptive_layer_height_profile(GLCanvas3D& canvas, float quality_factor);
        void smooth_layer_height_profile(GLCanvas3D& canvas, const HeightProfileSmoothingParams& smoothing_params);

        // Orio: the bar, in its panel (PaintAxisBar), or beside its ends in the panel.
        bool bar_rect_contains(const GLCanvas3D& canvas, float x, float y) const;
        Rect get_bar_rect_screen(const GLCanvas3D& canvas) const;
        Rect get_bar_rect_viewport(const GLCanvas3D& canvas) const;
        // Height of a point of the screen, the mouse painting the bar, or the part when the stroke started on it.
        float edit_coordinate_at(const GLCanvas3D& canvas, float x, float y) const;
        // Height in the scene of the point of the part under a point of the screen, NaN off the part.
        float part_coordinate_at(const GLCanvas3D& canvas, float x, float y) const;
        // Width kept at the right of the scene for the panel of the bar, 0 when the tool is closed.
        float right_bar_width(const GLCanvas3D& canvas) const;
        static float get_overlay_window_width() { return LayersEditing::s_overlay_window_width; }

        float object_max_z() const { return m_object_max_z; }
        // Orio: "Hide the other objects" ticked and the volume not of the edited object: neither drawn nor picked.
        bool hides(const GLVolume& volume) const;

        std::string get_tooltip(const GLCanvas3D& canvas) const;

    private:
        bool is_initialized() const;
        void generate_layer_height_texture();

        void render_background_texture(const GLCanvas3D& canvas);
        void render_curve(const Rect& bar_rect);

        void update_slicing_parameters();

        // Orio: height under the mouse, over the bar or over the part, -1000 elsewhere.
        float cursor_coordinate(const GLCanvas3D& canvas) const;
        // The layer height under the mouse over the bar or the part ("0.12 mm"), empty elsewhere.
        std::string cursor_value_text(const GLCanvas3D& canvas) const;
        // The ray casters of the solid parts of the edited object, built again when its parts or their meshes changed.
        void update_part_raycasters() const;
        // Orio: the bounds of the edited object inside the bar (ModelObject::layer_height_bounds, heights in mm),
        // sorted, or the dragged ones; and written back to the object, as one undo step.
        std::vector<double> active_bounds() const;
        void set_bounds(std::vector<double> bounds, const std::string& snapshot_name);
        // Over the graduations on the left of the bar: the menu of the bounds (right click) and dragging one (left click),
        // drawn over the bar, with the layer and the height of the one under the mouse or dragged.
        void render_bounds(const GLCanvas3D& canvas);

    private:
        GLCanvas3D &m_canvas;
    };

    //BBS: variable print speed along Z.
    // Deliberately a mirror image of LayersEditing so that both tools feel identical: the same
    // vertical bar on the right hand side of the 3D scene, the same left / right / shift + click
    // gestures, the same band width on the mouse wheel. Only the painted quantity differs: a print
    // speed in mm/s instead of a layer height in mm. The profile never changes the layering, it only
    // overrides the feedrate of the extrusion roles ticked in the tool window.
    class SpeedEditing
    {
    public:
        enum EState : unsigned char
        {
            Unknown,
            Editing,
            Completed,
            Num_States
        };

    private:
        bool                        m_enabled{ false };
        unsigned int                m_z_texture_id{ 0 };
        // Not owned by SpeedEditing.
        const DynamicPrintConfig*   m_config{ nullptr };
        // ModelObject for the currently selected object (Model::objects[last_object_id]).
        const ModelObject*          m_model_object{ nullptr };
        // Maximum z of the currently selected object.
        float                       m_object_max_z{ 0.0f };

        // Orio: one painted speed curve per extrusion type. A type is present in m_curves
        // once it has been painted, and every type in m_curves applies its speed at slicing (so infill
        // and perimeters can carry different speeds at the same layer). m_speed_profile is the working
        // copy of the ACTIVE type's curve (the one the right-hand bar edits), mirrored back on sync.
        std::map<uint32_t, std::vector<double>> m_curves;
        // The single type currently selected for editing (its box is ticked), or 0 when none is.
        // Selection is single-choice: ticking a type unticks the previous one. It is independent from
        // which types apply (that is decided by presence in m_curves).
        uint32_t                    m_active_target{ 0 };
        // Orio: type whose curve the next Reset clears (0 = the active type). Set by a row's
        // revert icon just before it posts EVT_GLCANVAS_RESET_SPEED_PROFILE.
        uint32_t                    m_reset_bit{ 0 };
        // Object the cached m_curves belong to. m_curves is reloaded from the model only when the
        // selected object actually changes, so the painted curves survive a slice or a trip to the
        // Preview tab (both of which momentarily detach or reload the tool) instead of being lost.
        ObjectID                    m_curves_oid;
        // Orio: mirror of the curves last written to / read from the ModelObject. When
        // select_object() sees the object's speed_profile no longer match this, an undo/redo or a
        // project reload changed the model, so the cached m_curves are reloaded from it and Ctrl+Z/Y
        // affect the painted speeds. A plain reslice leaves speed_profile untouched, so it still matches
        // this and the painted curves survive.
        std::map<uint32_t, std::vector<double>> m_committed_curves;
        // Orio: bounds the painting does not cross (VariableSpeedProfile::bounds), per curve, in its coordinates;
        // and the model's own copy, as m_committed_curves.
        std::map<uint32_t, std::vector<double>> m_bounds;
        std::map<uint32_t, std::vector<double>> m_committed_bounds;
        // The bound dragged over the graduations (index in active_bounds()), -1 otherwise. Where the menu of the bounds
        // was opened: the coordinate along the bar, and the bound there (-1 for none).
        int    m_bound_drag{ -1 };
        double m_bounds_menu_coordinate{ 0. };
        int    m_bounds_menu_bound{ -1 };
        // Working copy of the active type's curve along the edited axis (empty while it has not been painted). Along X / Y
        // its coordinates start at 0 at the origin of the axis; m_curves keeps them in the frame of the object.
        std::vector<double>         m_speed_profile;
        // m_speed_profile was changed since it was loaded, it has to go back to m_curves.
        bool                        m_profile_changed{ false };
        // Orio: painting along the X / Y axes of the object (PaintAxisBar, shared with "Variable infill and walls"):
        // the Z | X / Y toggle, the edited axis and the bar at the bottom of the scene. The curve of a type along an axis
        // is keyed by paint_axis_key(type bit, axis) in m_curves, the bare bit along Z.
        PaintAxisBar                m_axis_bar;

        // Speed scale of the vertical bar, in mm/s. Orio: derived for the edited type (update_speed_range()), its
        // base and min editable in the tool window.
        float                       m_min_speed{ 10.0f };
        float                       m_max_speed{ 300.0f };
        // Speed the "Reset to base" and "Smooth" actions converge to.
        float                       m_base_speed{ 100.0f };
        bool                        m_speed_range_valid{ false };
        // Orio: speed of the edited type in the print settings, before the filament's flow caps it (10/11).
        float                       m_preset_speed{ 100.0f };
        // Orio: the base and min typed by the user, per type bit, kept until Orio is closed; the other types
        // follow the print profile (01/11, 10/11).
        std::map<uint32_t, float>   m_user_base_speeds;
        std::map<uint32_t, float>   m_user_min_speeds;
        // Orio: true once the step was set by the user or restored from the app config: it is no longer derived
        // from the scale.
        bool                        m_step_user_set{ false };

        mutable HeightProfileSmoothingParams m_smooth_params;
        // Orio: "Hide the other objects" (under "Keep min"), remembered in the app config.
        bool                        m_hide_other_objects{ false };

        static float                s_overlay_window_width;

        struct SpeedTexture
        {
            std::vector<char>   data;
            size_t              width{ 0 };
            size_t              height{ 0 };
            size_t              levels{ 0 };
            size_t              cells{ 0 };
            bool                valid{ false };
        };
        SpeedTexture    m_speed_texture;

        mutable GLModel m_baseline;
        mutable GLModel m_profile_curve;
        mutable bool m_profile_dirty{ true };
        // Neutral speed the texture and the curve were last built with: an unpainted curve is shown flat at it, and it
        // follows the base speed and the other painted curves of the type.
        float m_displayed_neutral{ -1.0f };

        // Orio: painting on the part itself. A ray caster per solid part of the edited object, kept while its
        // mesh stays the same (building it for a big mesh takes a while).
        struct PartRaycaster
        {
            const ModelVolume*                  volume{ nullptr };
            std::shared_ptr<const TriangleMesh> mesh;
            std::shared_ptr<MeshRaycaster>      raycaster;
        };
        mutable std::vector<PartRaycaster> m_part_raycasters;

    public:
        EState state{ Unknown };
        float band_width{ 2.0f };
        // Speed change per editing step, mm/s.
        float strength{ 2.0f };
        int last_object_id{ -1 };
        // Orio: NaN while a stroke started on the part is off it (the stroke pauses).
        float last_z{ 0.0f };
        // Orio: the stroke started on the part, not on the bar.
        bool part_stroke{ false };
        LayerHeightEditActionType last_action{ LAYER_HEIGHT_EDIT_ACTION_INCREASE };

        SpeedEditing(GLCanvas3D &canvas) : m_canvas(canvas) {}
        ~SpeedEditing();

        void init();

        void set_config(const DynamicPrintConfig* config);
        void select_object(const Model& model, int object_id);

        bool is_allowed() const;

        bool is_enabled() const;
        void set_enabled(bool enabled);

        void show_tooltip_information(const GLCanvas3D& canvas, std::map<wxString, wxString> captions_texts, float x, float y);
        void render_variable_speed_dialog(const GLCanvas3D& canvas);
        void render_overlay(const GLCanvas3D& canvas);
        void render_volumes(const GLCanvas3D& canvas, const GLVolumeCollection& volumes);

        void adjust_speed_profile();
        void accept_changes(GLCanvas3D& canvas);
        void reset_speed_profile(GLCanvas3D& canvas);
        void smooth_speed_profile(GLCanvas3D& canvas, const HeightProfileSmoothingParams& smoothing_params);

        // The bar, in its panel (PaintAxisBar): on the right for Z, at the bottom for X / Y.
        bool bar_rect_contains(const GLCanvas3D& canvas, float x, float y) const;
        Rect get_bar_rect_screen(const GLCanvas3D& canvas) const;
        Rect get_bar_rect_viewport(const GLCanvas3D& canvas) const;
        // Coordinate along the edited axis (0 at its start) of a point of the screen, the mouse painting the bar, or the
        // part when the stroke started on it.
        float edit_coordinate_at(const GLCanvas3D& canvas, float x, float y) const;
        // Orio: coordinate along the edited axis of the point of the part under a point of the screen (its height
        // along Z), NaN off the part.
        float part_coordinate_at(const GLCanvas3D& canvas, float x, float y) const;
        // Height kept at the bottom of the scene for the panel of the bar while painting along X / Y, 0 otherwise.
        float bottom_bar_height(const GLCanvas3D& canvas) const;
        // Width kept at the right of the scene for the panel of the bar while painting along Z, 0 otherwise.
        float right_bar_width(const GLCanvas3D& canvas) const;
        static float get_overlay_window_width() { return SpeedEditing::s_overlay_window_width; }

        float object_max_z() const { return m_object_max_z; }
        // Orio: "Hide the other objects" ticked and the volume not of the edited object: neither drawn nor picked.
        bool hides(const GLVolume& volume) const;

        std::string get_tooltip(const GLCanvas3D& canvas) const;

    private:
        bool is_initialized() const;
        void generate_speed_texture();

        void render_background_texture(const GLCanvas3D& canvas);
        void render_curve(const Rect& bar_rect);

        // Pull the speed scale out of the print profile, once per selected object.
        void update_speed_range();
        // Orio: scale max (mm/s) derived from the volumetric flow limit of the edited object's
        // material for the extrusions of a type (max_volumetric / mm3_per_mm, as the slicer); 0 when the material sets no limit.
        float object_volumetric_speed_max(uint32_t target) const;
        // Orio: load/save the tool's numeric fields (step, smoothing radius,
        // keep-min) in the application config so they persist across sessions (max is material-derived).
        void load_persisted_fields();
        void save_persisted_fields() const;
        // Make sure m_speed_profile is a valid profile spanning the whole object.
        void ensure_profile();
        // Push the per-role curves into the ModelObject and schedule a re-export.
        void commit_to_model(GLCanvas3D& canvas, const std::string& snapshot_name);
        // Orio: (re)load the cached per-role curves from a ModelObject's speed_profile,
        // rescaling them to its current height. Used when a different object is selected and when an
        // undo/redo or project reload has changed the model under the tool.
        void load_curves_from_model(const ModelObject* model_object);
        // Orio: helpers for the per-role curves.
        // Copy the working active curve (m_speed_profile) back into m_curves[edited_key()], when it changed.
        void sync_active_into_curves();
        // Load m_speed_profile from the active role's stored curve along the edited axis (empty when none/absent).
        void load_active_curve();
        // Switch the active role (syncing the current one first); enables it if needed.
        void set_active_target(uint32_t bit);

        // Orio: painting along X / Y, mirror image of InfillWallsEditing.
        // The axis and the key of the edited curve.
        PaintAxis edited_axis() const { return m_axis_bar.is_xy() ? m_axis_bar.axis() : paZ; }
        uint32_t  edited_key() const { return paint_axis_key(m_active_target, edited_axis()); }
        // Is a curve of this type painted along this axis?
        bool      painted(uint32_t target, PaintAxis axis) const;
        // Length of the edited axis along the bar: the height of the object along Z, its extent along X / Y; and where
        // that axis starts in the frame of the object (0 along Z).
        float     edit_length() const;
        double    edit_origin() const;
        // The speed the edited curve starts from and that the reset / smoothing gestures converge to: the base speed, or,
        // when other curves of the type are painted (the slowest painted speed applies), the highest speed they leave
        // everywhere, so that the edited curve changes nothing until painted slower.
        float     neutral_speed() const;
        // The edited curve, or a flat curve at the neutral speed while the type is not painted along that axis.
        std::vector<double> display_curve() const;
        // Switch to X / Y or back to Z, choose the edited axis (the X / Y switch of the bar). And at every frame, how that
        // axis appears on the screen and the height of the object along Z.
        void      set_xy(bool xy);
        void      set_xy_axis(PaintAxis axis);
        void      update_axis_bar(const GLCanvas3D& canvas);
        // Coordinate along the edited axis under the mouse, over the bar or over the part, -1000 elsewhere.
        float     cursor_coordinate(const GLCanvas3D& canvas) const;
        // The speed under the mouse over the bar or the part ("120 mm/s"), empty elsewhere.
        std::string cursor_value_text(const GLCanvas3D& canvas) const;
        // The ray casters of the solid parts of the edited object, built again when its parts or their meshes changed.
        void      update_part_raycasters() const;
        // Orio: the bounds of the edited curve along the bar (0 at its start), sorted, inside it; and written back.
        std::vector<double> active_bounds() const;
        void      set_active_bounds(std::vector<double> bounds);
        // Over the graduations of the bar (left of it along Z, under it along X / Y): the menu of the bounds (right click)
        // and dragging one (left click). Drawn over the bar, with the layer and the height (Z) or the coordinate (X / Y)
        // of the one under the mouse or dragged.
        void      render_bounds(const GLCanvas3D& canvas);
        // Along Z, the tops of the layers of the object (as the variable layer height tool slices it), where a bound goes:
        // the speed changes layer by layer.
        std::vector<double> object_layer_tops() const;

    private:
        GLCanvas3D &m_canvas;
    };

    //Orio: variable sparse infill density and wall count along Z.
    // Same interaction model as SpeedEditing and LayersEditing: one tool window, the same vertical bar on the
    // right hand side of the 3D scene, the same mouse gestures. A drop-down in the window chooses the painted
    // quantity (sparse infill density or number of walls); each quantity has its own curve, applied once
    // painted. The painted values are applied layer by layer while slicing (InfillWallsProfile.hpp).
    // A toggle of the window paints along the X / Y axes of the object instead, with the bar laid along the
    // bottom of the scene (PaintAxisBar): one more curve per quantity and axis.
    // Implemented in InfillWallsEditing.cpp.
    class InfillWallsEditing
    {
    public:
        enum EState : unsigned char
        {
            Unknown,
            Editing,
            Completed,
            Num_States
        };

    private:
        bool                        m_enabled{ false };
        unsigned int                m_z_texture_id{ 0 };
        // Not owned by InfillWallsEditing.
        const DynamicPrintConfig*   m_config{ nullptr };
        // ModelObject for the currently selected object (Model::objects[last_object_id]).
        const ModelObject*          m_model_object{ nullptr };
        // Maximum z of the currently selected object.
        float                       m_object_max_z{ 0.0f };

        // One painted curve per InfillWallsTarget bit; a quantity applies once it has been painted.
        std::map<uint32_t, std::vector<double>> m_curves;
        // Object the cached m_curves belong to, and its profile as last loaded from or committed to the model.
        // Every edit is committed to the model, which is the reference: m_curves is reloaded from it when
        // attaching to an object or when the model changed behind our back (undo / redo, project reload),
        // never because of a configuration change or a slice.
        ObjectID                    m_curves_oid;
        InfillWallsProfile          m_seen_profile;
        // Quantity chosen in the drop-down, the one edited by the bar on the right.
        uint32_t                    m_mode{ iwtInfillDensity };
        // Working copy of the edited curve (empty while that quantity has not been painted yet). Along X / Y, its
        // coordinates start at 0 at the origin of the axis, m_curves keeps them in the frame of the object.
        std::vector<double>         m_profile;
        // m_profile was changed since it was loaded, it has to go back to m_curves.
        bool                        m_profile_changed{ false };
        // Painting along X / Y: the toggle, the edited axis and the bar at the bottom of the scene.
        PaintAxisBar                m_axis_bar;
        // Bounds the painting does not cross (InfillWallsProfile::bounds), per curve, in its coordinates.
        std::map<uint32_t, std::vector<double>> m_bounds;

        // Scale of the bar for each quantity (the min / max fields of the window). They are the editor's
        // working range, remembered across sessions in the application config.
        float                       m_density_min{ 0.0f };
        float                       m_density_max{ 100.0f };
        float                       m_walls_min{ 1.0f };
        float                       m_walls_max{ 10.0f };

        mutable HeightProfileSmoothingParams m_smooth_params;
        // "Hide the other objects" (under "Keep min"), remembered in the app config with the other fields.
        bool                        m_hide_other_objects{ false };

        static float                s_overlay_window_width;

        struct ProfileTexture
        {
            std::vector<char>   data;
            size_t              width{ 0 };
            size_t              height{ 0 };
            size_t              levels{ 0 };
            size_t              cells{ 0 };
            bool                valid{ false };
        };
        ProfileTexture  m_texture;

        mutable GLModel m_baseline;
        mutable GLModel m_profile_curve;
        mutable bool m_profile_dirty{ true };
        // Base value the texture and the curve were last built with: an unpainted quantity is shown flat at
        // the base value, which follows the object settings and the print profile.
        float m_displayed_base{ -1.0f };

    public:
        EState state{ Unknown };
        float band_width{ 2.0f };
        int last_object_id{ -1 };
        // NaN while a stroke started on the part is off it (the stroke pauses).
        float last_z{ 0.0f };
        // The stroke started on the part, not on the bar.
        bool part_stroke{ false };
        LayerHeightEditActionType last_action{ LAYER_HEIGHT_EDIT_ACTION_INCREASE };

        InfillWallsEditing(GLCanvas3D &canvas) : m_canvas(canvas) {}
        ~InfillWallsEditing();

        void init();

        void set_config(const DynamicPrintConfig* config);
        void select_object(const Model& model, int object_id);

        bool is_allowed() const;

        bool is_enabled() const;
        void set_enabled(bool enabled);

        void render_overlay(const GLCanvas3D& canvas);
        void render_volumes(const GLCanvas3D& canvas, const GLVolumeCollection& volumes);

        void adjust_profile();
        void accept_changes(GLCanvas3D& canvas);
        void reset_profile(GLCanvas3D& canvas);
        void smooth_profile(GLCanvas3D& canvas, const HeightProfileSmoothingParams& smoothing_params);

        // The bar: on the right for Z, at the bottom for X / Y.
        bool bar_rect_contains(const GLCanvas3D& canvas, float x, float y) const;
        Rect get_bar_rect_screen(const GLCanvas3D& canvas) const;
        Rect get_bar_rect_viewport(const GLCanvas3D& canvas) const;
        // Coordinate along the edited axis (0 at its start) of a point of the screen, the mouse painting the bar, or the
        // part when the stroke started on it.
        float edit_coordinate_at(const GLCanvas3D& canvas, float x, float y) const;
        // Coordinate along the edited axis of the point of the part under a point of the screen (its height along Z), NaN
        // off the part.
        float part_coordinate_at(const GLCanvas3D& canvas, float x, float y) const;
        // Height kept at the bottom of the scene for the panel of the bar while painting along X / Y, margins
        // included, 0 otherwise.
        float bottom_bar_height(const GLCanvas3D& canvas) const;
        // Width kept at the right of the scene for the panel of the bar while painting along Z, margin included, 0
        // otherwise.
        float right_bar_width(const GLCanvas3D& canvas) const;

        float object_max_z() const { return m_object_max_z; }
        // "Hide the other objects" ticked and the volume not of the edited object: neither drawn nor picked.
        bool hides(const GLVolume& volume) const;

        std::string get_tooltip(const GLCanvas3D& canvas) const;

    private:
        bool is_initialized() const;
        void generate_texture();

        void render_dialog(const GLCanvas3D& canvas);
        void render_background_texture(const GLCanvas3D& canvas);
        void render_curve(const Rect& bar_rect);
        void show_tooltip_information(const GLCanvas3D& canvas, std::map<wxString, wxString> captions_texts, float x, float y);

        InfillWallsTarget mode() const { return (InfillWallsTarget)m_mode; }
        bool  is_walls_mode() const { return m_mode == (uint32_t)iwtWallLoops; }
        // The axis and the key of the edited curve.
        PaintAxis edited_axis() const { return m_axis_bar.is_xy() ? m_axis_bar.axis() : paZ; }
        uint32_t  edited_key() const { return paint_axis_key(m_mode, edited_axis()); }
        // Length of the edited axis along the bar: the height of the object along Z, its extent along X / Y; and
        // where that axis starts in the frame of the object (0 along Z).
        float  edit_length() const;
        double edit_origin() const;
        // The object's own value of the edited quantity (object settings, else print profile): the value an
        // unpainted object is sliced with.
        float base_value() const;
        // Width of the walls at each end of the object along X / Y, from its wall count and line widths: they hold
        // no infill, painting along X / Y leaves them out (PaintAxisBar::set_margins()).
        float walls_width() const;
        // Thickness of the bottom and the top shells of the object along Z, from its settings (shell layers and
        // thicknesses, layer heights): they hold no infill, painting along Z leaves them out.
        std::pair<float, float> shells_thickness() const;
        // The value the edited curve starts from and that the reset / smoothing gestures converge to: the base
        // value, or, when other curves of the quantity are painted (the highest painted value applies), the lowest
        // value they leave, so that the edited curve changes nothing until painted higher.
        float neutral_value() const;
        // Scale of the bar (the min / max fields) of the edited quantity, within the limits of the edited axis.
        float min_value() const;
        float max_value() const;
        // Painting along X / Y: switch to it or back to Z, choose the edited axis (the X / Y switch of the bar). And at
        // every frame, how that axis appears on the screen and what the bar cannot paint, both following the object.
        void  set_xy(bool xy);
        void  set_xy_axis(PaintAxis axis);
        void  update_axis_bar(const GLCanvas3D& canvas);
        // Coordinate along the edited axis under the mouse, over the bar or over the part, -1000 elsewhere.
        float cursor_coordinate(const GLCanvas3D& canvas) const;
        // The value under the mouse over the bar or the part ("Walls: 3"), empty elsewhere.
        std::string cursor_value_text(const GLCanvas3D& canvas) const;
        // The bounds of the edited curve along the bar (0 at its start), sorted, inside it; and written back.
        std::vector<double> active_bounds() const;
        void set_active_bounds(std::vector<double> bounds);
        // Change of the value per editing step (mouse event or timer tick while the button is held).
        float strength() const;
        // Value shown to the user and used by the slicer: the wall count is rounded.
        double display_value(double value) const;
        // The edited curve, or a flat curve at the base value while the quantity has not been painted yet.
        std::vector<double> display_curve() const;
        // Load / save the min / max fields, the chosen quantity and the smoothing parameters in the
        // application config, so they persist across sessions.
        void load_persisted_fields();
        void save_persisted_fields() const;
        // Make sure m_profile is a valid curve spanning the whole object.
        void ensure_profile();
        // Copy the working curve (m_profile) back into m_curves[m_mode].
        void sync_mode_into_curves();
        // Load m_profile from the stored curve of the current mode (empty when not painted).
        void load_mode_curve();
        // Switch the edited quantity, keeping the curve painted on the previous one.
        void set_mode(uint32_t mode);
        // Push the painted curves into the ModelObject and schedule a re-slice.
        void commit_to_model(GLCanvas3D& canvas, const std::string& snapshot_name);

    private:
        GLCanvas3D &m_canvas;
    };

    struct Mouse
    {
        struct Drag
        {
            static const Point Invalid_2D_Point;
            static const Vec3d Invalid_3D_Point;
            static const int MoveThresholdPx;

            Point start_position_2D;
            Vec3d start_position_3D;
            int move_volume_idx;
            bool move_requires_threshold;
            Point move_start_threshold_position_2D;

        public:
            Drag();
        };

        bool dragging;
        Vec2d position;
        Vec3d scene_position;
        Drag drag;
        bool ignore_left_up;
        bool ignore_right_up;
        bool rotating{ false };
        bool panning{ false };

        Mouse();

        void set_start_position_2D_as_invalid() { drag.start_position_2D = Drag::Invalid_2D_Point; }
        void set_start_position_3D_as_invalid() { drag.start_position_3D = Drag::Invalid_3D_Point; }
        void set_move_start_threshold_position_2D_as_invalid() { drag.move_start_threshold_position_2D = Drag::Invalid_2D_Point; }

        bool is_start_position_2D_defined() const { return (drag.start_position_2D != Drag::Invalid_2D_Point); }
        bool is_start_position_3D_defined() const { return (drag.start_position_3D != Drag::Invalid_3D_Point); }
        bool is_move_start_threshold_position_2D_defined() const { return (drag.move_start_threshold_position_2D != Drag::Invalid_2D_Point); }
        bool is_move_threshold_met(const Point& mouse_pos) const {
            return (std::abs(mouse_pos(0) - drag.move_start_threshold_position_2D(0)) > Drag::MoveThresholdPx)
                || (std::abs(mouse_pos(1) - drag.move_start_threshold_position_2D(1)) > Drag::MoveThresholdPx);
        }
    };

    struct SlaCap
    {
        struct Triangles
        {
            GLModel object;
            GLModel supports;
        };
        typedef std::map<unsigned int, Triangles> ObjectIdToModelsMap;
        double z;
        ObjectIdToModelsMap triangles;

        SlaCap() { reset(); }
        void reset() { z = DBL_MAX; triangles.clear(); }
        bool matches(double z) const { return this->z == z; }
    };

    enum class EWarning {
        ObjectOutside,
        ToolpathOutside,
        SlaSupportsOutside,
        SomethingNotShown,
        ObjectClashed,
        ObjectLimited,
        GCodeConflict,
        ToolHeightOutside,
        TPUPrintableError,
        FilamentPrintableError,
        LeftExtruderPrintableError, // before slice
        RightExtruderPrintableError, // before slice
        MultiExtruderPrintableError,      // after slice
        MultiExtruderHeightOutside,       // after slice
        FilamentUnPrintableOnFirstLayer,
        MixUsePLAAndPETG,
        MultiFilaNoWipeTower,
        PrimeTowerOutside,
        NozzleFilamentIncompatible,
        MixtureFilamentIncompatible,
        AsemblyInvalid, // for asembly view only
        FlushingVolumeZero,
        FilamentNozzleFlowIncompatible,
        TpuNozzleMultipleFilaments,
        HighTempNeedWrappingDetection,
        SingleExtruderMixedFilament
    };

    class RenderStats
    {
    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> m_measuring_start;
        int m_fps_out = -1;
        int m_fps_running = 0;
    public:
        void increment_fps_counter() { ++m_fps_running; }
        int get_fps() { return m_fps_out; }
        int get_fps_and_reset_if_needed() {
            auto cur_time = std::chrono::high_resolution_clock::now();
            int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cur_time-m_measuring_start).count();
            if (elapsed_ms > 1000  || m_fps_out == -1) {
                m_measuring_start = cur_time;
                m_fps_out = int (1000. * m_fps_running / elapsed_ms);
                m_fps_running = 0;
            }
            return m_fps_out;
        }

    };

    class Labels
    {
        bool m_enabled{ false };

        bool m_show_layer_labels{ false };
        bool m_show_object_labels{ true };

        GLCanvas3D& m_canvas;

    public:
        explicit Labels(GLCanvas3D& canvas) : m_canvas(canvas) {}
        void enable(bool enable) { m_enabled = enable; }

        void show_layer_labels(bool show) { m_show_layer_labels = m_enabled ? show : false; }
        void show_object_labels(bool show) { m_show_object_labels = m_enabled ? show : false; }
        bool are_layer_labels_shown() const { return m_show_layer_labels; }
        bool are_object_labels_shown() const { return m_show_object_labels; }

        void render(const std::vector<const ModelInstance*>& sorted_instances) const;
    };


    class Tooltip
    {
        std::string m_text;
        std::chrono::steady_clock::time_point m_start_time;
        // Indicator that the mouse is inside an ImGUI dialog, therefore the tooltip should be suppressed.
        bool m_in_imgui = false;

    public:
        bool is_empty() const { return m_text.empty(); }
        void set_text(const std::string& text);
        void render(const Vec2d& mouse_position, GLCanvas3D& canvas);
        // Indicates that the mouse is inside an ImGUI dialog, therefore the tooltip should be suppressed.
        void set_in_imgui(bool b) { m_in_imgui = b; }
        bool is_in_imgui() const { return m_in_imgui; }
    };

    // Structure to hold assembly view button information
    struct AssemblyViewButtonInfo {
        float x;      // x position in screen coordinates
        float y;      // y position in screen coordinates
        float width;  // button width
        float height; // button height
    };

    class Slope
    {
        bool m_enabled{ false };
        GLVolumeCollection& m_volumes;
    public:
        Slope(GLVolumeCollection& volumes) : m_volumes(volumes) {}

        void enable(bool enable) { m_enabled = enable; }
        bool is_enabled() const { return m_enabled; }
        void use(bool use) { m_volumes.set_slope_active(m_enabled ? use : false); }
        bool is_used() const { return m_volumes.is_slope_active(); }
        void globalUse(bool use) { m_volumes.set_slope_GlobalActive(m_enabled ? use : false); }
        bool is_GlobalUsed() const { return m_volumes.is_slope_GlobalActive(); }
        void set_normal_angle(float angle_in_deg) const {
            m_volumes.set_slope_normal_z(-::cos(Geometry::deg2rad(90.0f - angle_in_deg)));
        }
    };

    class RenderTimer : public wxTimer {
    private:
        virtual void Notify() override;
    };

    class ToolbarHighlighterTimer : public wxTimer {
    private:
        virtual void Notify() override;
    };

    class GizmoHighlighterTimer : public wxTimer {
    private:
        virtual void Notify() override;
    };

    class RenderPipelineStageModifier
    {
    public:
        explicit RenderPipelineStageModifier(GLCanvas3D& canvas, ERenderPipelineStage stage);
        ~RenderPipelineStageModifier();

    private:
        // no copy
        RenderPipelineStageModifier(const RenderPipelineStageModifier&) = delete;
        RenderPipelineStageModifier(RenderPipelineStageModifier&&) = delete;

        // no assign
        RenderPipelineStageModifier& operator=(const RenderPipelineStageModifier&) = delete;
        RenderPipelineStageModifier& operator=(RenderPipelineStageModifier&&) = delete;
    private:
        GLCanvas3D& m_canvas;
        ERenderPipelineStage m_stage;
    };

public:
    enum ECursorType : unsigned char
    {
        Standard,
        Cross,
        Hand
    };

    struct ArrangeSettings
    {
        float distance           = 0.f;
//        float distance_sla       = 6.;
        float accuracy           = 0.65f; // Unused currently
        bool  enable_rotation    = false;
        bool  allow_multi_materials_on_same_plate = true;
        bool  avoid_extrusion_cali_region = true;
        //BBS: add more arrangeSettings
        bool is_seq_print        = false;
        bool  align_to_y_axis    = false;
        bool        save_svg            = false; // for debug
        std::string postfix;
        void        reset()
        {
            distance                            = 0.f;
            accuracy                            = 0.65f;
            enable_rotation                     = false;
            allow_multi_materials_on_same_plate = true;
            avoid_extrusion_cali_region         = true;
            is_seq_print                        = false;
            align_to_y_axis                     = false;
        }
    };

    struct OrientSettings
    {
        float overhang_angle = 60.f;
        bool  enable_rotation = false;
        bool  min_area = true;
    };

    //BBS: add canvas type for assemble view usage
    enum ECanvasType
    {
        CanvasView3D = 0,
        CanvasPreview = 1,
        CanvasAssembleView = 2,
    };

    int GetHoverId();
    void set_ignore_left_up() { m_mouse.ignore_left_up = true; }
    GLVolumeCollection &get_paint_outline_volumes() { return m_paint_outline_volumes; }
    Vec2i               convert_world_pt_to_screen(Vec3d pos);
    bool                is_mouse_in_screen_convex_hull(Vec2i mouse,const BoundingBoxf3& box);

private:
    bool m_is_dark = false;
    wxGLCanvas* m_canvas;
    wxGLContext* m_context;
    bool m_dirty_context{ true };
    Bed3D &m_bed;
    std::map<std::string, wxString> m_assembly_view_desc;
#if ENABLE_RETINA_GL
    std::unique_ptr<RetinaHelper> m_retina_helper;
#endif
    unsigned int m_last_w, m_last_h;
    bool m_in_render;
    wxTimer m_timer;
    wxTimer m_timer_set_color;
    int m_color_input_value = -1;
    LayersEditing m_layers_editing;
    //BBS: variable print speed along Z.
    SpeedEditing m_speed_editing;
    //Orio: variable sparse infill density / wall count along Z.
    InfillWallsEditing m_infill_walls_editing;
    //Orio: cut X/Y/Z of the Prepare tab (same slider as in the Preview), hatched cut faces.
    PrepareCut m_prepare_cut;
    Mouse m_mouse;
    GLGizmosManager m_gizmos;
    //BBS: GUI refactor: GLToolbar
    mutable std::shared_ptr<GLToolbar> m_main_toolbar{ nullptr };
    mutable IMToolbar m_sel_plate_toolbar;
    mutable IMToolbar m_assembly_view_thumbnail;
    mutable IMReturnToolbar m_return_toolbar;
    mutable Vec2i              m_fit_camrea_button_pos = {128, 5};
    mutable float              m_sc{1};
    mutable float m_paint_toolbar_width;
    //Orio: width of the plates toolbar of the preview at the last frame (pixels), 0 if not drawn.
    float m_sel_plate_toolbar_width{0.f};
    //Orio: bottom of the "Return" button of the tools at the last frame (pixels), 0 if not drawn.
    float m_return_button_bottom{0.f};

    // assembly_view
    Camera::ViewAngleType m_assembly_view_preview_angle{Camera::ViewAngleType::Iso};
    bool                  m_show_assembly_view_preview_menu{false};
    size_t                m_assembly_view_thumbnail_volume_count{static_cast<size_t>(-1)};
    bool                  m_isolated_volumes_notified{false};
    //BBS: add canvas type for assemble view usage
    ECanvasType m_canvas_type;
    std::array<ClippingPlane, 2> m_clipping_planes;
    ClippingPlane m_camera_clipping_plane;
    bool m_use_clipping_planes;
    SlaCap m_sla_caps[2];
    //Orio: supports generated by the last slicing of the plates, shown on demand in the Prepare view to place the
    // modifiers changing the support settings: their extrusions as they will be printed. Read from the result of the slicing
    // when the display is turned on, released by the next slicing, see show_sliced_supports() and reset_sliced_supports().
    struct SlicedSupports
    {
        struct Layer
        {
            // Bottom / top of the layer above the print bed (raft excluded).
            float      bottom_z { 0.f };
            float      top_z    { 0.f };
            // Areas of the support, PrintObject coordinates.
            ExPolygons islands;
        };
        struct Instance
        {
            ObjectID    model_instance_id;
            // World matrix of the reference part (Object::reference_volume_id) of this instance when it was sliced: the support
            // follows its later moves.
            Transform3d slicing_matrix { Transform3d::Identity() };
            // PrintInstance::shift (unscaled): PrintObject coordinates into the world when sliced.
            Vec2d       shift { Vec2d::Zero() };
        };
        struct Object
        {
            ~Object();

            ObjectID              model_object_id;
            // First part of the object, whose moves the support follows: the matrix of the instance alone also changes when
            // its rotation and scale are transferred to the volumes (a modifier or a part is added), without any move.
            ObjectID              reference_volume_id;
            int                   plate_idx { -1 };
            // Released once sent to the graphics card.
            std::vector<Layer>    layers;
            // Segments of the extrusions, 8 floats each: start (x, y, z), width (negative for the interface), end (x, y, z)
            // and height, z being the top of the layer. Drawn two by two with the model SlicedSupports::box.
            std::vector<float>    segments;
            std::vector<Instance> instances;
            // Areas of the layers, drawn pale where the objects hide the supports.
            GLModel               model;
            // Segments in the graphics memory: vertex buffer of the instances (two segments each), number of instances.
            unsigned int          segments_vbo { 0 };
            unsigned int          segments_instances { 0 };
        };
        std::vector<std::unique_ptr<Object>> objects;
        // Objects replaced by a new slicing, released by the next rendering (with the OpenGL context current).
        std::vector<std::unique_ptr<Object>> released;
        // Two boxes of unit length, width and height, stretched over two segments by the "support_paths" shader.
        GLModel                              box;
        // The last slicing produced supports: the display can be turned on.
        bool                                 available { false };
        bool                                 show { false };
    };
    SlicedSupports m_sliced_supports;
    //Orio: size of the label of the « Hors impression » area at the last frame, to keep it in the canvas.
    float m_parking_label_width{ 0.f };
    float m_parking_label_height{ 0.f };
    // Instances (object, instance) whose volumes were deactivated because the area is hidden.
    std::set<std::pair<int, int>> m_parked_hidden;
    //Orio: eye of the Preview hiding the hovered object (_render_object_hide_eye). Per mesh of the shells: raycaster,
    // center of mass and volume (mesh coordinates). The object of the eye at the last frame (label in the G-code, -1: none),
    // the eye and the object on the screen (x0, y0, x1, y1).
    struct HideEyeMesh
    {
        std::shared_ptr<const TriangleMesh> mesh;
        std::shared_ptr<MeshRaycaster>      raycaster;
        Vec3d                               center{ Vec3d::Zero() };
        double                              volume{ 0. };
    };
    std::map<const TriangleMesh*, HideEyeMesh> m_hide_eye_meshes;
    int                  m_hide_eye_label{ -1 };
    // Orio : objet sous la souris, celui qu'un clic choisit (label du G-code, -1 : aucun). L'objet choisi est tenu par le
    // rendu de l'Apercu (GCodeViewer::get_picked_label) : cadre blanc, oeil garde, ses statistiques dans la legende.
    int                  m_hover_pick_label{ -1 };
    std::array<float, 4> m_hide_eye_rect{ { 0.f, 0.f, 0.f, 0.f } };
    std::array<float, 4> m_hide_eye_object_rect{ { 0.f, 0.f, 0.f, 0.f } };
    std::string m_sidebar_field;
    // when true renders an extra frame by not resetting m_dirty to false
    // see request_extra_frame()
    bool m_extra_frame_requested;
    bool m_event_handlers_bound{ false };

    GLVolumeCollection m_paint_outline_volumes;
    GLVolumeCollection m_volumes;
    mutable std::shared_ptr<gcode::GCodeViewer> m_p_gcode_viewer{ nullptr };

    RenderTimer m_render_timer;

    Selection m_selection;
    const DynamicPrintConfig* m_config;
    Model* m_model;
    BackgroundSlicingProcess *m_process;
    bool m_requires_check_outside_state{ false };

    std::array<unsigned int, 2> m_old_size{ 0, 0 };

    // Screen is only refreshed from the OnIdle handler if it is dirty.
    bool m_dirty;
    bool m_initialized;
    //BBS: add flag to controll rendering
    bool m_render_preview{ true };
    bool m_enable_render { true };
    bool m_apply_zoom_to_volumes_filter;
    bool m_picking_enabled;
    bool m_moving_enabled;
    bool m_dynamic_background_enabled;
    bool m_multisample_allowed;
    bool m_moving;
    bool m_tab_down;
    //BBS: add toolpath outside
    bool m_toolpath_outside{ false };
    ECursorType m_cursor_type;
    GLSelectionRectangle m_rectangle_selection;

    //BBS:add plate related logic
    mutable std::vector<int> m_hover_volume_idxs;
    std::vector<int> m_hover_plate_idxs;
    //BBS if explosion_ratio is changed, need to update volume bounding box
    mutable float m_explosion_ratio = 1.0;
    mutable Vec3d m_rotation_center{ 0.0, 0.0, 0.0};

    // Following variable is obsolete and it should be safe to remove it.
    // I just don't want to do it now before a release (Lukas Matena 24.3.2019)
    bool m_render_sla_auxiliaries;

    std::string m_color_by;

    bool m_reload_delayed;

#if ENABLE_RENDER_PICKING_PASS
    bool m_show_picking_texture;
#endif // ENABLE_RENDER_PICKING_PASS

    RenderStats m_render_stats;

    int m_imgui_undo_redo_hovered_pos{ -1 };
    int m_mouse_wheel{ 0 };
    int m_selected_extruder;

    Labels m_labels;
    Tooltip m_tooltip;
    bool m_tooltip_enabled{ true };

    Slope m_slope;

    OrientSettings m_orient_settings_fff, m_orient_settings_sla;

    ArrangeSettings m_arrange_settings_fff, m_arrange_settings_sla,
        m_arrange_settings_fff_seq_print;

    PrinterTechnology current_printer_technology() const;

    bool        m_show_world_axes{false};
    Bed3D::Axes m_axes;
    //BBS:record key botton frequency
    int auto_orient_count = 0;
    int auto_arrange_count = 0;
    int split_to_objects_count = 0;
    int split_to_part_count = 0;
    int custom_height_count = 0;
    int assembly_view_count = 0;

    std::stack<ERenderPipelineStage> m_render_pipeline_stage_stack;
    static GLModel s_full_screen_mesh;

    using FrameCallback = std::function<void()>;
    std::vector<FrameCallback> m_frame_callback_list;

#if ENABLE_SHOW_CAMERA_TARGET
    mutable GLModel m_camera_target_mark;
#endif // ENABLE_SHOW_CAMERA_TARGET

public:
    OrientSettings& get_orient_settings()
    {
        PrinterTechnology ptech = this->current_printer_technology();

        auto* ptr = &this->m_orient_settings_fff;

        if (ptech == ptSLA) {
            ptr = &this->m_orient_settings_sla;
        }

        return *ptr;
    }

    void load_arrange_settings();
    //Orio: reloads the arrange options shared by all the modes, including "Align to Y axis" which is
    // saved per printer structure: called again each time the printer preset is loaded.
    void load_shared_arrange_settings();
    ArrangeSettings& get_arrange_settings();// { return get_arrange_settings(this); }
    ArrangeSettings& get_arrange_settings(PrintSequence print_seq) {
        return (print_seq == PrintSequence::ByObject) ? m_arrange_settings_fff_seq_print
            : m_arrange_settings_fff;
    }

    class SequentialPrintClearance
    {
        //BBS: add the height logic
        GLModel m_height_limit;
        GLModel m_fill;
        GLModel m_perimeter;
        bool m_render_fill{ true };
        bool m_visible{ false };

        std::vector<Pointf3s> m_hull_2d_cache;

    public:
        //BBS: add the height logic
        SequentialPrintClearance(GLCanvas3D &m_canvas) : m_canvas(m_canvas) {}
        ~SequentialPrintClearance();
        void set_polygons(const Polygons& polygons, const std::vector<std::pair<Polygon, float>>& height_polygons);
        void set_render_fill(bool render_fill) { m_render_fill = render_fill; }
        void set_visible(bool visible) { m_visible = visible; }
        void render();
        void reset();

        friend class GLCanvas3D;

    private:
        GLCanvas3D &m_canvas;
    };

    SequentialPrintClearance m_sequential_print_clearance;
    bool m_sequential_print_clearance_first_displacement{ true };

    struct ToolbarHighlighter
    {
        void set_timer_owner(wxEvtHandler* owner, int timerid = wxID_ANY);
        void init(const std::shared_ptr<GLToolbarItem>& toolbar_item, GLCanvas3D* canvas);
        void blink();
        void invalidate();
        bool                    m_render_arrow{ false };
        std::weak_ptr<GLToolbarItem> m_toolbar_item;
    private:
        GLCanvas3D*             m_canvas{ nullptr };
        int				        m_blink_counter{ 0 };
        ToolbarHighlighterTimer m_timer;
    }
    m_toolbar_highlighter;

    struct GizmoHighlighter
    {
        void set_timer_owner(wxEvtHandler* owner, int timerid = wxID_ANY);
        void init(GLGizmosManager* manager, GLGizmosManager::EType gizmo, GLCanvas3D* canvas);
        void blink();
        void invalidate();
        bool                    m_render_arrow{ false };
        GLGizmosManager::EType  m_gizmo_type;
    private:
        GLGizmosManager*        m_gizmo_manager{ nullptr };
        GLCanvas3D*             m_canvas{ nullptr };
        int				        m_blink_counter{ 0 };
        GizmoHighlighterTimer   m_timer;

    }
    m_gizmo_highlighter;
    bool    m_can_show_navigator = true;

    // for debug draw
    GLModel m_unit_cube;

public:
    explicit GLCanvas3D(wxGLCanvas* canvas, Bed3D &bed);
    ~GLCanvas3D();

    bool is_initialized() const { return m_initialized; }

    void set_context(wxGLContext* context);
    void set_type(ECanvasType type);
    ECanvasType get_canvas_type() { return m_canvas_type; }

    wxGLCanvas* get_wxglcanvas() { return m_canvas; }
	const wxGLCanvas* get_wxglcanvas() const { return m_canvas; }

    bool init();
    void post_event(wxEvent &&event);

    float get_explosion_ratio() { return m_explosion_ratio; }
    void reset_explosion_ratio() { m_explosion_ratio = 1.0; }
    void on_change_color_mode(bool is_dark, bool reinit = true);
    const bool get_dark_mode_status() { return m_is_dark; }
    void set_as_dirty();
    void requires_check_outside_state() { m_requires_check_outside_state = true; }

    bool is_in_same_model_object(const std::vector<int> volume_ids);
    unsigned int get_volumes_count() const;
    const GLVolumeCollection& get_volumes() const { return m_volumes; }
    void reset_volumes(bool set_notice = true);
    ModelInstanceEPrintVolumeState check_volumes_outside_state(ObjectFilamentResults* object_results = nullptr) const;
    bool is_all_plates_selected() { return m_sel_plate_toolbar.m_all_plates_stats_item && m_sel_plate_toolbar.m_all_plates_stats_item->selected; }
    bool is_volumes_selected_and_sinking() const;
    const float get_scale() const;

    //BBS
    gcode::GCodeViewer& get_gcode_viewer() const;
    void init_gcode_viewer(ConfigOptionMode mode, Slic3r::PresetBundle* preset_bundle);
    void reset_gcode_toolpaths();
    void update_gcode_sequential_view_current(unsigned int first, unsigned int last);

    void toggle_selected_volume_visibility(bool selected_visible);
    void toggle_sla_auxiliaries_visibility(bool visible, const ModelObject* mo = nullptr, int instance_idx = -1);
    void toggle_model_objects_visibility(bool visible, const ModelObject* mo = nullptr, int instance_idx = -1, const ModelVolume* mv = nullptr);
    void update_instance_printable_state_for_object(size_t obj_idx);
    void update_instance_printable_state_for_objects(const std::vector<size_t>& object_idxs);
    //Orio: « Hors impression » area of the non printable instances. Hidden, its instances are neither drawn nor
    // pickable.
    void update_parked_volumes_visibility();
    void set_parking_zone_hidden(bool hidden);
    void zoom_to_parking_zone();

    void set_config(const DynamicPrintConfig* config);
    void set_process(BackgroundSlicingProcess* process);
    void set_model(Model* model);
    const Model* get_model() const { return m_model; }
    Model&       get_ref_model() const { return *m_model; }

    const Selection& get_selection() const;
    Selection& get_selection();

    const GLGizmosManager& get_gizmos_manager() const { return m_gizmos; }
    GLGizmosManager& get_gizmos_manager() { return m_gizmos; }

    void bed_shape_changed();

    //BBS: add part plate related logic
    void plates_count_changed();

    void set_clipping_plane(unsigned int id, const ClippingPlane& plane)
    {
        if (id < 2)
        {
            m_clipping_planes[id] = plane;
            m_sla_caps[id].reset();
        }
    }
    void reset_clipping_planes_cache() { m_sla_caps[0].triangles.clear(); m_sla_caps[1].triangles.clear(); }
    void set_use_clipping_planes(bool use);

    bool                                get_use_clipping_planes() const { return m_use_clipping_planes; }
    const std::array<ClippingPlane, 2> &get_clipping_planes() const { return m_clipping_planes; };
    void                                set_use_color_clip_plane(bool use) { m_volumes.set_use_color_clip_plane(use); }
    void                                set_color_clip_plane(const Vec3d &cp_normal, double offset) { m_volumes.set_color_clip_plane(cp_normal, offset); }
    void                                set_color_clip_plane_colors(const std::array<ColorRGBA, 2> &colors) { m_volumes.set_color_clip_plane_colors(colors); }
    void                                set_color_clip_sections(GLVolumeCollection::ColorClipSections sections) { m_volumes.set_color_clip_sections(std::move(sections)); }

    // Volume color override methods (for mesh boolean gizmo)
    void set_use_volume_color_override(bool use) { m_volumes.set_use_volume_color_override(use); }
    void set_volume_color_override(unsigned int volume_idx, const std::array<float, 4>& color) { m_volumes.set_volume_color_override(volume_idx, color); }
    void set_volumes_color_override(const std::vector<unsigned int>& volume_indices, const std::array<float, 4>& color) { m_volumes.set_volumes_color_override(volume_indices, color); }
    void clear_volume_color_override(unsigned int volume_idx) { m_volumes.clear_volume_color_override(volume_idx); }
    void clear_all_volume_color_overrides() { m_volumes.clear_all_volume_color_overrides(); }

    void set_color_by(const std::string& value);

    void set_show_world_axes(bool flag) { m_show_world_axes = flag; }
    void refresh_camera_scene_box();

    BoundingBoxf3 assembly_view_cur_bounding_box() const;
    BoundingBoxf3 volumes_bounding_box(bool limit_to_expand_plate) const;
    bool          is_volumes_limit_to_expand_plate() const;
    BoundingBoxf3 scene_bounding_box() const;
    BoundingBoxf3 plate_scene_bounding_box(int plate_idx) const;

    bool is_layers_editing_enabled() const;
    bool is_layers_editing_allowed() const;

    void reset_layer_height_profile();
    void adaptive_layer_height_profile(float quality_factor);
    void smooth_layer_height_profile(const HeightProfileSmoothingParams& smoothing_params);

    //BBS: variable print speed along Z.
    bool is_speed_editing_enabled() const;
    bool is_speed_editing_allowed() const;
    void enable_speed_editing(bool enable);
    void reset_speed_profile();
    void smooth_speed_profile(const HeightProfileSmoothingParams& smoothing_params);
    //Orio: same as detach_infill_walls_editing() below, for the variable speed tool.
    void detach_speed_editing();

    //Orio: variable sparse infill density / wall count along Z.
    bool is_infill_walls_editing_enabled() const;
    bool is_infill_walls_editing_allowed() const;
    void enable_infill_walls_editing(bool enable);
    void reset_infill_walls_profile();
    void smooth_infill_walls_profile(const HeightProfileSmoothingParams& smoothing_params);
    // Called before an Undo / Redo jump, which deletes the model objects: the tool, which may stay open across
    // the jump, lets go of the edited object. The next render attaches it to the restored one.
    void detach_infill_walls_editing();

    bool is_reload_delayed() const;

    void enable_layers_editing(bool enable);
    void enable_legend_texture(bool enable);
    void enable_picking(bool enable);
    void enable_moving(bool enable);
    void enable_gizmos(bool enable);
    void enable_selection(bool enable);
    void enable_main_toolbar(bool enable);
    //BBS: GUI refactor: GLToolbar
    void _update_select_plate_toolbar_stats_item(bool force_selected = false);
    void reset_select_plate_toolbar_selection();
    void enable_select_plate_toolbar(bool enable);
    void enable_return_toolbar(bool enable);
    void enable_dynamic_background(bool enable);
    void enable_labels(bool enable) { m_labels.enable(enable); }
    void enable_slope(bool enable) { m_slope.enable(enable); }
    void allow_multisample(bool allow);

    void zoom_to_bed();
    void zoom_to_volumes();
    void zoom_to_selection();
    void zoom_to_gcode();
    //BBS -1 for current plate
    void zoom_to_plate(int plate_idx = -1);
    void zoom_to_fit();
    void select_view(const std::string& direction);
    //BBS: add part plate related logic
    void select_plate();
    //BBS: GUI refactor: GLToolbar&&gizmo
    int   get_main_toolbar_offset() const;
    float get_main_toolbar_left(int cnv_width,float inv_zoom) const;
    int   get_main_toolbar_height() const;
    // Orio: top (pixels from the top of the view) of the window of a main toolbar item whose top is item_top (in the
    // units of the toolbar: origin at the middle of the view, y up): under the toolbar; as high as the item when the user
    // moved the toolbar against a side (ImGuiWrapper::set_tool_windows_beside puts the window beside it).
    float main_toolbar_window_top(float item_top) const;
    int   get_main_toolbar_width() const;
    float get_main_toolbar_scale() const;
    float get_assembly_paint_toolbar_width() const { return m_paint_toolbar_width; }
    bool  is_collapse_toolbar_on_left() const;
    float get_collapse_toolbar_width() const;
    float get_collapse_toolbar_height() const;

    void update_volumes_colors_by_extruder();

    bool is_dragging() const { return m_gizmos.is_dragging() || m_moving; }

    void render(bool only_init = false);
    bool is_rendering_enabled()
    {
        return m_enable_render;
    }
    void enable_render(bool enabled)
    {
        m_enable_render = enabled;
    }
    enum class ThumbnailRenderRype {
        GLVolumes,
        CustomMeshOrVertexColors,
    };
    struct ExtraThumbData {
        ThumbnailRenderRype render_type;
        ECanvasType         canvas_type;
        bool                rebuild_bvh;
        ExtraThumbData(ThumbnailRenderRype rt = ThumbnailRenderRype::GLVolumes,
                       ECanvasType ct = ECanvasType::CanvasView3D,
                       bool rb = false)
            : render_type(rt), canvas_type(ct), rebuild_bvh(rb) {}
    };
    struct IsolatedVolumeInfo {
        GLVolume*     vol     = nullptr;
        int           obj_idx = -1;
        BoundingBoxf3 world_box_assembly;
    };
    static bool                            s_enable_bvh;
    static std::vector<IsolatedVolumeInfo> s_isolated_volumes;
    static int                             s_assemble_candidate_volumes_size;
    static bool                            s_isolated_notification_shown;
    static bool                            s_intersects_notification_shown;
    static int                             s_assemble_ratio;
    static int                             s_assemble_volume_ratio;
    static bool                            s_far_from_origin_notification_shown;
    static BoundingBoxf3                   s_bvh_primary_bounds;
    static double                          s_expand_bvh_box_dist;
    static BoundingBoxf3                   s_bvh_expanded_bounds;
    static BoundingBoxf3                   s_first_primary_bounds;
    // printable_only == false -> render also non printable volumes as grayed
    // parts_only == false -> render also sla support and pad
    void render_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
                                 Camera::EType           camera_type,
                                 Camera::ViewAngleType   camera_view_angle_type = Camera::ViewAngleType::Iso,
                                 bool                    for_picking  = false,
                                 bool                    ban_light    = false,
                                 const ExtraThumbData&   extra_thumb_data = ExtraThumbData());

    void render_thumbnail(ThumbnailData &           thumbnail_data,
                                 std::vector<std::array<float, 4>> &extruder_colors,
                                 unsigned int              w,
                                 unsigned int              h,
                                 const ThumbnailsParams &  thumbnail_params,
                                 ModelObjectPtrs &         model_objects,
                                 const GLVolumeCollection &volumes,
                                 Camera::EType             camera_type,
                                 Camera::ViewAngleType     camera_view_angle_type = Camera::ViewAngleType::Iso,
                                 bool                      for_picking  = false,
                                 bool                      ban_light    = false,
                                 const ExtraThumbData&     extra_thumb_data = ExtraThumbData());

    // render thumbnail using an off-screen framebuffer
    static void render_thumbnail_framebuffer(const std::shared_ptr<OpenGLManager>& p_ogl_manager, ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
        PartPlateList& partplate_list, ModelObjectPtrs& model_objects, const GLVolumeCollection& volumes, std::vector<std::array<float, 4>>& extruder_colors,
                                             const std::shared_ptr<GLShaderProgram>& shader,
                                             Camera::EType                      camera_type,
                                             Camera::ViewAngleType              camera_view_angle_type = Camera::ViewAngleType::Iso,
                                             bool                               for_picking  = false,
                                             bool                                    ban_light              = false,
                                             const ExtraThumbData&                   extra_thumb_data       = ExtraThumbData());

    //BBS use gcoder viewer render calibration thumbnails
    void render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params);

    //BBS
    void select_curr_plate_all();
    void select_object_from_idx(std::vector<int>& object_idxs);
    void remove_curr_plate_all();

    void select_all();
    void deselect_all();
    void exit_gizmo();
    void set_selected_visible(bool visible);
    void delete_selected();
    void ensure_on_bed(unsigned int object_idx, bool allow_negative_z);

    int get_gcode_layers_count() const;
    bool is_gcode_legend_enabled() const;
    std::vector<double> get_gcode_layers_zs() const;
    std::vector<double> get_volumes_print_zs(bool active_only) const;

    void set_gcode_options_visibility_from_flags(unsigned int flags);
    unsigned int get_gcode_options_visibility_flags() const;

    unsigned int get_toolpath_role_visibility_flags() const;

    void set_volumes_z_range(const std::array<double, 2>& range);
    std::vector<CustomGCode::Item>& get_custom_gcode_per_print_z() const;

    size_t get_gcode_extruders_count()const;

    std::vector<int> load_object(const ModelObject& model_object, int obj_idx, std::vector<int> instance_idxs, bool lod_enabled);
    std::vector<int> load_object(const Model& model, int obj_idx, bool lod_enabled);

    void mirror_selection(Axis axis);

    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);
    //BBS: always load shell at preview
    void load_shells(const Print& print, bool force_previewing = false);
    void reset_shells();
    void set_shells_on_previewing(bool is_preview);

    //BBS: add only gcode mode
    void load_gcode_preview(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors, bool only_gcode);
    void refresh_gcode_preview_render_paths();
    void load_sla_preview();
    //void load_preview(const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values);
    void bind_event_handlers();
    void unbind_event_handlers();

    void on_size(wxSizeEvent& evt);
    void on_idle(wxIdleEvent& evt);
    void on_char(wxKeyEvent& evt);
    void on_key(wxKeyEvent& evt);
    void on_mouse_wheel(wxMouseEvent& evt);
    void on_timer(wxTimerEvent& evt);
    void on_render_timer(wxTimerEvent& evt);
    void on_set_color_timer(wxTimerEvent& evt);
    void on_mouse(wxMouseEvent& evt);
    void on_gesture(wxGestureEvent& evt);
    void on_paint(wxPaintEvent& evt);
    void on_kill_focus(wxFocusEvent &evt);
    void on_set_focus(wxFocusEvent& evt);
    void on_back_slice_begin();
    void force_set_focus();

    Size get_canvas_size() const;
    Vec2d get_local_mouse_position() const;

        // store opening position of menu
    std::optional<Vec2d>        m_popup_menu_positon; // position of mouse right click
    void                        set_popup_menu_position(const Vec2d &position) { m_popup_menu_positon = position; }
    const std::optional<Vec2d> &get_popup_menu_position() const { return m_popup_menu_positon; }
    void                        clear_popup_menu_position() { m_popup_menu_positon.reset(); }

    void set_tooltip(const std::string& tooltip);

    // the following methods add a snapshot to the undo/redo stack, unless the given string is empty
    void do_move(const std::string& snapshot_type,bool force_volume_move =false);
    void do_rotate(const std::string& snapshot_type);
    void do_scale(const std::string& snapshot_type);
    void do_flatten(const Vec3d& normal, const std::string& snapshot_type);
    void do_center();
    void do_center_plate(const int plate_idx);
    void do_mirror(const std::string& snapshot_type);

    void update_gizmos_on_off_state();
    void reset_all_gizmos() { m_gizmos.reset_all_states(); }

    void handle_sidebar_focus_event(const std::string& opt_key, bool focus_on);
    void handle_layers_data_focus_event(const t_layer_height_range range, const EditorType type);

    void update_ui_from_settings();

    int get_move_volume_id() const { return m_mouse.drag.move_volume_idx; }
    int get_first_hover_volume_idx() const { return m_hover_volume_idxs.empty() ? -1 : m_hover_volume_idxs.front(); }
    //Orio: is it the GLVolume of a volume of the "Modifiers" tool (ModelVolume::is_modifier_tool_volume())?
    bool is_modifier_tool_glvolume(const GLVolume *volume) const;
    //Orio: the first color of "preferred" standing out from the filaments of the object (parts, painted areas, filaments
    // of the supports), else the one standing out the most: the outlines of the intervals of the "Modifiers" tool.
    ColorRGB color_standing_out(const ModelObject &model_object, const std::vector<ColorRGB> &preferred) const;
    //Orio: color of a new modifier of the object, standing out from its filaments and from its modifiers already there.
    ColorRGB new_modifier_color(const ModelObject &model_object) const;
    void set_selected_extruder(int extruder) { m_selected_extruder = extruder;}

    class WipeTowerInfo {
    protected:
        Vec2d m_pos = {std::nan(""), std::nan("")};
        double m_rotation = 0.;
        BoundingBoxf m_bb;
        // BBS: add partplate logic
        int m_plate_idx = -1;
        friend class GLCanvas3D;

    public:
        inline operator bool() const {
            return !std::isnan(m_pos.x()) && !std::isnan(m_pos.y());
        }

        inline const Vec2d& pos() const { return m_pos; }
        inline double rotation() const { return m_rotation; }
        inline const Vec2d bb_size() const { return m_bb.size(); }

        void apply_wipe_tower() const;
    };

    // BBS: add partplate logic
    WipeTowerInfo get_wipe_tower_info(int plate_idx) const;

    // Returns the view ray line, in world coordinate, at the given mouse position.
    Linef3 mouse_ray(const Point& mouse_pos);

    void set_mouse_as_dragging() { m_mouse.dragging = true; }
    bool is_mouse_dragging() const { return m_mouse.dragging; }

    double get_size_proportional_to_max_bed_size(double factor) const;

    // BBS: get empty cells to put new object
    // start_point={-1,-1} means sort from bed center, step is the unscaled x,y stride
    std::vector<Vec2f> get_empty_cells(const Vec2f start_point, const Vec2f step = {10, 10});
    // BBS: get the nearest empty cell
    // start_point={-1,-1} means sort from bed center
    Vec2f get_nearest_empty_cell(const Vec2f start_point, const Vec2f step = {10, 10});

    void set_cursor(ECursorType type);
    void msw_rescale();

    void request_extra_frame() { m_extra_frame_requested = true; }
    // Orio: the close cross of a floating tool window (ImGuiWrapper::begin_tool_window, with the id of that window).
    void close_tool_window(const std::string &id);

    void schedule_extra_frame(int miliseconds);

    int get_main_toolbar_item_id(const std::string& name) const;
    void force_main_toolbar_left_action(int item_id);
    void force_main_toolbar_right_action(int item_id);

    bool has_toolpaths_to_export() const;
    void export_toolpaths_to_obj(const char* filename) const;

    void mouse_up_cleanup();

    bool are_layer_labels_shown() const { return m_labels.are_layer_labels_shown(); }
    void show_layer_labels(bool show) { m_labels.show_layer_labels(show); }
    bool are_object_labels_shown() const { return m_labels.are_object_labels_shown(); }
    void show_object_labels(bool show) { m_labels.show_object_labels(show); }

    bool is_overhang_shown() const { return m_slope.is_GlobalUsed(); }
    void show_overhang(bool show) { m_slope.globalUse(show); }

    //Orio: supports generated by the last slicing, shown on demand in the Prepare view.
    // Called by the plater once the processing of a plate is finished (the Print is idle): turns the display off and releases it.
    void reset_sliced_supports();
    bool has_sliced_supports() const { return m_sliced_supports.available || ! m_sliced_supports.objects.empty(); }
    bool are_sliced_supports_shown() const { return m_sliced_supports.show; }
    void show_sliced_supports(bool show);

    bool is_using_slope() const { return m_slope.is_used(); }
    void use_slope(bool use) { m_slope.use(use); }
    void set_slope_normal_angle(float angle_in_deg) { m_slope.set_normal_angle(angle_in_deg); }

    void highlight_toolbar_item(const std::string& item_name);
    void highlight_gizmo(const std::string& gizmo_name);

    // Timestamp for FPS calculation and notification fade-outs.
    static int64_t timestamp_now() {
#ifdef _WIN32
        // Cheaper on Windows, calls GetSystemTimeAsFileTime()
        return wxGetUTCTimeMillis().GetValue();
#else
        // calls clock()
        return wxGetLocalTimeMillis().GetValue();
#endif
    }

    void reset_sequential_print_clearance() {
        m_sequential_print_clearance.set_visible(false);
        m_sequential_print_clearance.set_render_fill(false);
        //BBS: add the height logic
        m_sequential_print_clearance.set_polygons(Polygons(), std::vector<std::pair<Polygon, float>>());
    }

    void set_sequential_print_clearance_visible(bool visible) {
        m_sequential_print_clearance.set_visible(visible);
    }

    void set_sequential_print_clearance_render_fill(bool render_fill) {
        m_sequential_print_clearance.set_render_fill(render_fill);
    }

    //BBS: add the height logic
    void set_sequential_print_clearance_polygons(const Polygons& polygons, const std::vector<std::pair<Polygon, float>>& height_polygons) {
        m_sequential_print_clearance.set_polygons(polygons, height_polygons);
    }

    bool can_sequential_clearance_show_in_gizmo();
    void update_sequential_clearance();

    const Print* fff_print() const;
    const SLAPrint* sla_print() const;

    void reset_old_size() { m_old_size = { 0, 0 }; }

    bool is_object_sinking(int object_idx) const;
    void apply_retina_scale(Vec2d &screen_coordinate) const;
    void _perform_layer_editing_action(wxMouseEvent* evt = nullptr);
    //BBS: variable print speed along Z.
    void _perform_speed_editing_action(wxMouseEvent* evt = nullptr);
    //Orio: variable sparse infill density / wall count along Z.
    void _perform_infill_walls_editing_action(wxMouseEvent* evt = nullptr);

    // Convert the screen space coordinate to an object space coordinate.
    // If the Z screen space coordinate is not provided, a depth buffer value is substituted.
    Vec3d _mouse_to_3d(const Camera& camera, const Point& mouse_pos, float* z = nullptr, const std::string& frame_name = "");

    bool make_current_for_postinit();

    void mark_context_dirty();

    Camera &                          get_active_camera();
    const Camera &                    get_active_camera() const;

    // Get assembly view button information
    AssemblyViewButtonInfo get_assembly_view_button_info() const;
    std::vector<std::array<float, 4>> get_active_colors();

private:
    bool _is_shown_on_screen() const;

    void _update_slice_error_status();

    void _switch_toolbars_icon_filename();
    bool _init_toolbars();
    bool _init_main_toolbar();
    bool _init_select_plate_toolbar();
    bool _update_imgui_select_plate_toolbar();
    bool _init_assembly_view_thumbnail();
    bool _update_assembly_view_thumbnail();
    bool _init_return_toolbar();
    // BBS
    //bool _init_view_toolbar();
    bool _init_collapse_toolbar();

    bool _set_current(bool force_update = false);
    void _resize(unsigned int w, unsigned int h);

    //BBS: add part plate related logic
    BoundingBoxf3 _max_bounding_box(bool include_gizmos, bool include_bed_model, bool include_plates, bool volumes_limit_to_expand_plate) const;

    void _zoom_to_box(const BoundingBoxf3& box, double margin_factor = DefaultCameraZoomToBoxMarginFactor);
    void _update_camera_zoom(double zoom);
    void _refresh_if_shown_on_screen();

    void _picking_pass();
    void _rectangular_selection_picking_pass();
    void _render_background() const;
    void _render_bed(bool bottom, bool show_axes);
    void _render_bed_for_picking(bool bottom);
    //BBS: add part plate related logic
    void _render_platelist(bool bottom, bool only_current, bool only_body = false, int hover_id = -1, bool render_cali = false, bool show_grid = true) const;
    void _render_plates_for_picking() const;
    //BBS: add outline drawing logic
    void _render_objects(GLVolumeCollection& cur_volumes, GLVolumeCollection::ERenderType type, bool with_outline = true,bool in_paint_gizmo = false);
    //BBS: GUI refactor: add canvas size as parameters
    void _render_gcode(int canvas_width, int canvas_height);
    //BBS: render a plane for assemble
    void _render_plane() const;
    void _render_bvh_primary_bounds();
    void _render_selection() const;
    void _render_sequential_clearance();
#if ENABLE_RENDER_SELECTION_CENTER
    void _render_selection_center() const;
#endif // ENABLE_RENDER_SELECTION_CENTER
    void _check_and_update_toolbar_icon_scale();
    void _render_overlays();
    void _render_style_editor();
    void _render_volumes_for_picking() const;
    void _render_current_gizmo() const;
    void _render_main_toolbar();
    void _render_imgui_select_plate_toolbar();
    void _render_assembly_view_thumbnail_toolbar();
    void _render_assembly_view_preview_menu(float anchor_x, float anchor_y, float anchor_width, float anchor_height);
    void _render_return_toolbar();
    void _render_fit_camera_toolbar();
    //Orio: top of the "Fit camera" button, above the view cube at the bottom left (pixels); the bottom of the view
    // without the view cube.
    float fit_camera_button_top();
    //Orio: vertical band of the view for the main toolbar laid out against a side (pixels): below the collapse button
    // of the settings column on that side, above the "Fit camera" button.
    void main_toolbar_vertical_band(float &top, float &bottom);
    void _render_collapse_toolbar() const;
    // BBS
    //void _render_view_toolbar() const;
    void _render_paint_toolbar() const;
    float _show_assembly_tooltip_information(float caption_max, float x, float y) const;
    void _render_assemble_control();
    void _render_assemble_info() const;
#if ENABLE_SHOW_CAMERA_TARGET
    void _render_camera_target() const;
#endif // ENABLE_SHOW_CAMERA_TARGET
    void _render_sla_slices();
    //Orio: supports of the last slicing in the Prepare view, and the button showing / hiding them.
    void _render_sliced_supports();
    void _render_sliced_supports_toggle();
    //Orio: label of the « Hors impression » area: shows / hides it, zooms to it.
    void _render_parking_zone_toggle();
    //Orio: card of the Preview when the plate is not sliced with the current settings: « Slice again (Ctrl+R) ».
    void _render_stale_slice_card();
    //Orio: eye on the center of mass of the object hovered in the Preview: hides its toolpaths (faint shell kept).
    void _render_object_hide_eye();
    void _render_picked_object_frame();
    // Reads the supports from the result of the slicing of the plates, returns false if there is none.
    bool _read_sliced_supports();
    //Orio: the cut of the Prepare tab is available (no gizmo other than move / rotate / scale, no height tool).
    bool _is_prepare_cut_allowed() const;
    //Orio: modifiers in a color standing out from the filaments of their object, never the one of its sliced supports.
    void _update_modifiers_colors();
    //Orio: support enforcers in green when they generate tree supports.
    void _update_support_enforcers_colors();
    //Orio: contours of the objects sunk below the bed in a color standing out from their filaments.
    void _update_sinking_contours_colors();
    void _render_selection_sidebar_hints() const;
    //BBS: GUI refactor: adjust main toolbar position
    bool _render_orient_menu(float left, float right, float bottom, float top);
    bool _render_arrange_menu(float left, float toolbar_height);
    void _render_3d_navigator();
    bool can_show_3d_navigator();
    void _update_volumes_hover_state();

    // Convert the screen space coordinate to world coordinate on the bed.
    Vec3d _mouse_to_bed_3d(const Point& mouse_pos);

    void _start_timer();
    void _stop_timer();

    // Create 3D thick extrusion lines for a skirt and brim.
    // Adds a new Slic3r::GUI::3DScene::Volume to volumes, updates collision with the build_volume.
    void _load_print_toolpaths(const BuildVolume &build_volume);
    // Create 3D thick extrusion lines for object forming extrusions.
    // Adds a new Slic3r::GUI::3DScene::Volume to $self->volumes,
    // one for perimeters, one for infill and one for supports, updates collision with the build_volume.
    void _load_print_object_toolpaths(const PrintObject& print_object, const BuildVolume &build_volume,
        const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values);
    // Create 3D thick extrusion lines for wipe tower extrusions, updates collision with the build_volume.
    void _load_wipe_tower_toolpaths(const BuildVolume &build_volume, const std::vector<std::string>& str_tool_colors);

    // Load SLA objects and support structures for objects, for which the slaposSliceSupports step has been finished.
	void _load_sla_shells();
    void _update_sla_shells_outside_state();
    void _set_warning_notification_if_needed(EWarning warning);

    //BBS: add partplate print volume get function
    BoundingBoxf3 _get_current_partplate_print_volume();

    // generates a warning notification containing the given message
    void _set_warning_notification(EWarning warning, bool state);

    bool is_flushing_matrix_error();
    bool _is_any_volume_outside() const;

    // updates the selection from the content of m_hover_volume_idxs
    void _update_selection_from_hover();

    bool _deactivate_collapse_toolbar_items();
    bool _deactivate_arrange_menu();
    //BBS: add deactivate_orient_menu
    bool _deactivate_orient_menu();
    //BBS: add _deactivate_layersediting_menu
    bool _deactivate_layersediting_menu();
    //BBS: add _deactivate_speedediting_menu
    bool _deactivate_speedediting_menu();
    //Orio: same for the variable infill / walls tool.
    bool _deactivate_infillwallsediting_menu();

    // BBS FIXME
    float get_overlay_window_width() { return 0; /*LayersEditing::get_overlay_window_width();*/ }

    static std::vector<std::array<float, 4>> _parse_colors(const std::vector<std::string>& colors);

    void _push_render_stage(ERenderPipelineStage stage);
    void _pop_render_stage();
    ERenderPipelineStage _get_current_render_stage() const;

    void _render_silhouette_effect();
    void _composite_silhouette_effect();

    void _debug_draw_camera(const Camera& t_camera);

    void _debug_draw_aabb();

    void _init_unit_cube();

    void _append_to_frame_callback(const FrameCallback& cb);

    void _render_toolbar();

    const std::shared_ptr<GLToolbar>& get_main_toolbar() const;

    static bool is_volume_in_plate_boundingbox(const GLVolume &v, int plate_idx, const BoundingBoxf3 &plate_build_volume);
    static void _init_fullscreen_mesh();

    static void _rebuild_postprocessing_pipeline(const std::shared_ptr<OpenGLManager>& p_ogl_manager, const std::string& input_framebuffer_name, std::string& output_framebuffer_name, uint32_t width, uint32_t height);

    static void _render_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, ModelObjectPtrs& model_objects,
        const GLVolumeCollection& volumes, std::vector<std::array<float, 4>>& extruder_colors,
        const std::shared_ptr<GLShaderProgram>& shader,
        Camera::EType                      camera_type,
        Camera::ViewAngleType              camera_view_angle_type = Camera::ViewAngleType::Iso,
        bool                               for_picking = false,
        bool                               ban_light              = false,
        const ExtraThumbData&              extra_thumb_data       = ExtraThumbData());
    void _show_isolated_volumes_notification();
    static bool _move_isolated_volumes_closer(wxEvtHandler*);
    void _check_assembly_far_from_origin();
    static bool _reset_assembly_to_origin(wxEvtHandler*);
    static void _filter_assembly_thumbnail_candidates_by_bvh(const std::vector<GLVolume*>& assemble_candidate_volumes,
        const std::vector<BoundingBoxf3>&  assemble_candidate_boxes,
        bool                               skip_single_volume_bvh,
        bool                               rebuild_bvh,
        std::vector<bool>&                 include_candidate_volumes);
    static void _render_assembly_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, ModelObjectPtrs& model_objects,
        const GLVolumeCollection& volumes, std::vector<std::array<float, 4>>& extruder_colors,
        const std::shared_ptr<GLShaderProgram>& shader,
        Camera::EType                      camera_type,
        Camera::ViewAngleType              camera_view_angle_type = Camera::ViewAngleType::Iso,
        bool                               for_picking = false,
        bool                               ban_light              = false,
        const ExtraThumbData&              extra_thumb_data       = ExtraThumbData());
    static void _render_custom_thumbnail_internal(ThumbnailData &                         thumbnail_data,
                                           const ThumbnailsParams &                thumbnail_params,
                                           PartPlateList &                         partplate_list,
                                           ModelObjectPtrs &                       model_objects,
                                           const GLVolumeCollection &              volumes,
                                           std::vector<std::array<float, 4>> &     extruder_colors,
                                           const std::shared_ptr<GLShaderProgram> &shader,
                                           Camera::EType                           camera_type,
                                           Camera::ViewAngleType                   camera_view_angle_type = Camera::ViewAngleType::Iso,
                                           bool                                    for_picking            = false,
                                           bool                                    ban_light              = false,
                                           const ExtraThumbData&                   extra_thumb_data       = ExtraThumbData(ThumbnailRenderRype::CustomMeshOrVertexColors));
};

const ModelVolume *get_model_volume(const GLVolume &v, const Model &model);
ModelVolume *get_model_volume(const ObjectID &volume_id, const ModelObjectPtrs &objects);
ModelVolume *get_model_volume(const GLVolume &v, const ModelObjectPtrs &objects);
ModelVolume *get_model_volume(const GLVolume &v, const ModelObject &object);
ModelVolume *get_selected_model_volume(const GLCanvas3D &canvas);

GLVolume *get_first_hovered_gl_volume(const GLCanvas3D &canvas);
GLVolume *get_selected_gl_volume(const GLCanvas3D &canvas);

ModelObject *get_selected_model_object(GLCanvas3D &canvas);
ModelObject *get_model_object(const GLVolume &gl_volume, const Model &model);
ModelObject *get_model_object(const GLVolume &gl_volume, const ModelObjectPtrs &objects);

ModelInstance *get_model_instance(const GLVolume &gl_volume, const Model &model);
ModelInstance *get_model_instance(const GLVolume &gl_volume, const ModelObjectPtrs &objects);
ModelInstance *get_model_instance(const GLVolume &gl_volume, const ModelObject &object);
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLCanvas3D_hpp_
