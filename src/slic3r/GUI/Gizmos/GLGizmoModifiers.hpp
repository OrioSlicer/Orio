#ifndef slic3r_GLGizmoModifiers_hpp_
#define slic3r_GLGizmoModifiers_hpp_

// Orio : outil « Modificateurs » de la barre supérieure. Pose précise, à la manière des
// connecteurs de l'outil Coupe, de ce qui s'ajoute à une pièce : matière (Joindre), trou (Couper),
// modificateur de réglages (Modifier), bloqueur / forceur de supports ; formes, dont l'intervalle (une
// tranche de la pièce entre deux plans, montrée par leurs contours). La forme
// suit la souris sur la pièce et s'aimante à ses ancrages (centres de cercles, barycentre d'une
// face, sommets, milieux d'arêtes…) ; ses cotes se règlent au réglet ou au clavier ; « Inverser »
// la passe de l'autre côté du plan d'ancrage ; « Angle » la tourne autour de la normale ;
// « Joindre » (comme la Liaison de Fusion 360) amène un point ou une face de la forme sur un point ou
// une face de la pièce. Sous les réglages, le tableau des modificateurs de l'objet montre seulement
// leurs réglages modifiés.

#include "GLGizmoBase.hpp"
#include "FaceAnchors.hpp"

#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLTexture.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <imgui/imgui.h>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r {

namespace Measure {
class Measuring;
}

namespace GUI {

class MeshRaycaster;
class GLGizmoModifierMeasure;

class GLGizmoModifiers : public GLGizmoBase
{
public:
    // Ce que l'outil pose (Joindre / Couper / Modifier comme l'outil Texte, puis les supports).
    enum class Nature : unsigned char { Join, Cut, Modifier, SupportBlocker, SupportEnforcer, Count };
    using Shape = ModifierPlacement::Shape;

    GLGizmoModifiers(GLCanvas3D &parent, unsigned int sprite_id);
    ~GLGizmoModifiers() override;

    std::string get_icon_filename(bool b_dark_mode) const override;
    bool        on_mouse(const wxMouseEvent &mouse_event) override;
    bool        gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;
    void        data_changed(bool is_serializing) override;

    bool        wants_enter_leave_snapshots() const override { return true; }
    std::string get_gizmo_entering_text() const override { return "Entering Modifiers tool"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving Modifiers tool"; }

    // Nature d'un volume (couleurs, tableau).
    static Nature nature_of(const ModelVolume &volume);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_set_state() override;
    void        on_render() override;
    void        on_render_for_picking() override {}
    void        on_render_input_window(float x, float y, float bottom_limit) override;

private:
    // Repère d'un volume, dans le monde : point d'ancrage (centre du dessous de la boîte de son
    // maillage), rotation, et échelle le long de ses axes.
    struct Pose
    {
        Vec3d    anchor{Vec3d::Zero()};
        Matrix3d rotation{Matrix3d::Identity()};
        Vec3d    scale{Vec3d::Ones()};
    };

    // Point de la pièce sous la souris (monde), aimanté ou non.
    struct SurfacePoint
    {
        bool     valid{false};
        Vec3d    point{Vec3d::Zero()};
        Vec3d    normal{Vec3d::UnitZ()}; // normale sortante
        Vec3d    xref{Vec3d::UnitX()};   // axe de référence dans le plan (longueur de la face)
        int      part{-1};               // index dans m_parts
        int      plane{-1};              // face plane du moteur de l'outil Mesure
        int      facet{-1};              // triangle touché (maillage de la pièce)
        bool     snapped{false};
        wxString anchor_name;
        // aimanté au centre d'un cercle (bord d'un trou) : son contour, surligné (monde)
        bool     on_circle{false};
        Vec3d    circle_center{Vec3d::Zero()};
        Vec3d    circle_normal{Vec3d::UnitZ()};
        double   circle_radius{0.};
    };

    // Maillage d'un volume de l'objet, avec son lanceur de rayons et (au besoin) ses faces planes.
    struct PickMesh
    {
        ObjectID                            volume_id;
        const TriangleMesh                 *mesh{nullptr};
        Transform3d                         trafo{Transform3d::Identity()}; // monde ← maillage
        std::shared_ptr<MeshRaycaster>      raycaster;
        std::shared_ptr<Measure::Measuring> measuring;
    };
    // Point d'une forme ronde sur un de ses plans YZ / ZX / XY (repère du maillage), avec la normale de
    // la surface là ; axis : 0, 1, 2 = l'axe normal à ce plan.
    struct PrincipalPoint
    {
        Vec3d point;
        Vec3d normal;
        int   axis;
    };
    // Lanceurs de rayons et faces planes gardés par maillage (le maillage est retenu pour que son
    // adresse ne serve pas à un autre).
    struct PickCache
    {
        std::shared_ptr<const TriangleMesh> mesh;
        std::shared_ptr<MeshRaycaster>      raycaster;
        std::shared_ptr<Measure::Measuring> measuring;
        // sommets et centre de la matière (repère du maillage), pour Maj pendant « Joindre »
        bool               corners_ready{false};
        std::vector<Vec3d> corners;
        Vec3d              barycentre{Vec3d::Zero()};
        // points de ses plans YZ / ZX / XY (forme ronde de l'outil, pendant « Joindre »)
        bool                        principal_ready{false};
        std::vector<PrincipalPoint> principal;
        // voisins de chaque triangle (« Depuis une face »), au premier survol
        std::vector<Vec3i> neighbors;
    };

    // Face plane survolée et ses ancrages, dans un repère 2D posé sur elle.
    struct FaceData
    {
        int                 part{-1};
        int                 plane{-1};
        Vec3d               origin{Vec3d::Zero()};
        Vec3d               normal{Vec3d::UnitZ()};
        Vec3d               axis_x{Vec3d::UnitX()};
        Vec3d               axis_y{Vec3d::UnitY()};
        FaceAnchors::Face   face;
        std::vector<double> key;
    };
    // « Depuis une face » : la face survolée, triangles reliés au triangle visé et tournés comme lui à 30° près.
    struct FaceSelection
    {
        std::shared_ptr<const TriangleMesh> mesh;
        Transform3d                         trafo{Transform3d::Identity()}; // monde ← maillage
        int                                 seed{-1};                       // triangle visé
        std::vector<int>                    facets;                         // triés
        std::shared_ptr<GLModel>            highlight;
    };

    // « Joindre » : rien, origine à cliquer sur la forme, puis sur la cible (pièce ou autre forme).
    enum class AlignStep : unsigned char { None, ShapePoint, TargetPoint };
    // Origine d'une jonction (monde) : un ancrage, avec la normale de sa face, ou la face elle-même.
    struct JointPick
    {
        bool     valid{false};
        bool     face{false}; // la face elle-même (pas un de ses ancrages)
        ObjectID volume_id;
        bool     on_tool{false};
        int      plane{-1};
        Vec3d    point{Vec3d::Zero()};
        Vec3d    normal{Vec3d::Zero()}; // nulle : aucune face (centre d'un volume pointé dans le vide)
        wxString name;
    };

    // « Références » : une référence pointée (monde) pour une cote, sur la forme puis sur la pièce.
    using RefKind = ModifierDimension::Kind;
    struct PickedRef
    {
        bool     valid{false};
        RefKind  kind{RefKind::Point};
        Vec3d    point{Vec3d::Zero()};
        Vec3d    dir{Vec3d::UnitZ()}; // axe ou normale, unitaire
        wxString name;
        // pour la dessiner
        ObjectID volume_id;
        bool     on_tool{false};
        int      plane{-1};
        Vec3d    edge_a{Vec3d::Zero()};
        Vec3d    edge_b{Vec3d::Zero()};
        bool     circle{false};
        Vec3d    circle_center{Vec3d::Zero()};
        Vec3d    circle_normal{Vec3d::UnitZ()};
        double   circle_radius{0.};
        // point aligné sur le premier point choisi : 0..2 = même X, Y ou Z ; 3..5 = sur la droite
        // passant par lui le long de X, Y ou Z (deux coordonnées égales)
        int      aligned{-1};
    };
    // Références : rien, référence de la forme à cliquer, de la pièce, puis distance à taper.
    enum class RefStep : unsigned char { None, VolumeRef, PartRef, Value };

    // Intervalle : son plan du bas (un point, ses axes puis sa normale, du bas vers le haut) et son épaisseur,
    // dans le monde ; les contours de ses deux plans sur les pièces pleines (bas, haut) et leurs traits,
    // refaits seulement quand l'intervalle ou les pièces bougent.
    struct IntervalPlanes
    {
        Vec3d    origin{Vec3d::Zero()};
        Matrix3d frame{Matrix3d::Identity()};
        double   thickness{0.};
    };
    struct IntervalOutlines
    {
        std::vector<double>                            key;
        std::array<std::vector<std::vector<Vec3d>>, 2> loops;
        std::array<std::shared_ptr<GLModel>, 2>        models;
    };
    // Cible d'une cote d'intervalle (monde), fixe dans l'espace : un point, une droite (sa direction) ou un plan
    // (sa normale), par où passe son plan du bas, ou du haut (« offset » : son épaisseur).
    struct IntervalTarget
    {
        RefKind kind{RefKind::Point};
        Vec3d   point{Vec3d::Zero()};
        Vec3d   dir{Vec3d::UnitZ()};
        double  offset{0.};
    };

    // ---- objet édité ----
    void         sync_with_selection();
    ModelObject *model_object() const;
    int          object_index() const;
    int          instance_index() const;
    Transform3d  instance_matrix() const;
    ModelVolume *current_volume() const;
    ModelVolume *volume_by_id(const ObjectID &id) const;
    bool         is_tool_listed(const ModelVolume &volume) const;
    int          solid_parts_count() const;

    // ---- formes ----
    static ModelVolumeType              volume_type(Nature nature);
    static bool                         default_outside(Nature nature);
    const TriangleMesh                 *shape_mesh(Shape shape);
    std::shared_ptr<const TriangleMesh> panel_mesh();
    Vec3d                               default_size(Shape shape);
    wxString                            shape_label(Shape shape) const;
    ImTextureID                         shape_image(Shape shape);
    wxString                            generic_name(Shape shape) const;
    static wxString                     nature_name(Nature nature);
    std::string                         shape_name(const ModelVolume &volume) const;
    std::string                         default_name(const std::string &shape_text, Nature nature) const;
    bool                                is_default_name(const std::string &name, const std::string &shape_text, Shape shape, Nature nature) const;

    // ---- repères ----
    static Vec3d       base_point(const BoundingBoxf3 &mesh_box);
    static Pose        decompose(const Transform3d &world, const BoundingBoxf3 &mesh_box);
    static Transform3d compose(const Pose &pose, const BoundingBoxf3 &mesh_box);
    static Matrix3d    surface_frame(const Vec3d &normal, const Vec3d &xref, bool outside);
    static Matrix3d    rotation_z(double degrees);
    Transform3d        volume_world_matrix(const ModelVolume &volume) const;
    Vec3d              volume_size(const ModelVolume &volume) const;
    void               set_volume_world_matrix(ModelVolume &volume, const Transform3d &world);
    void               sync_gl_volumes(const ModelVolume &volume);
    void               ensure_rigid_instance();
    ModifierPlacement  placement_of(const ModelVolume &volume) const;
    Matrix3d           posed_frame(const Pose &pose, const ModifierPlacement &placement) const;

    // ---- survol, ancrages ----
    void                                update_picks();
    std::shared_ptr<Measure::Measuring> measuring_of(PickMesh &pick);
    const PickCache                    &corners_of(PickMesh &pick);
    const PickCache                    &principal_of(PickMesh &pick);
    std::vector<PrincipalPoint>         ring_points(PickMesh &pick);
    std::optional<Vec3d>                face_centroid(PickMesh &pick, int plane);
    double                              pixel_size_at(const Vec3d &point) const;
    static bool  mouse_on_imgui();
    bool         raycast(const std::vector<PickMesh> &meshes, const Vec2d &mouse, int &index, Vec3d &hit, Vec3d &normal, size_t &facet) const;
    SurfacePoint surface_under_mouse(const Vec2d &mouse, bool snap);
    void         update_face(int part, int plane, const Vec3d &normal);
    void         update_face_selection();
    int          tool_volume_under_mouse(const Vec2d &mouse) const;
    void         update_hover(const Vec2d &mouse, bool shift);
    JointPick    joint_under_mouse(const Vec2d &mouse, bool shift, bool placing = false);

    // ---- actions ----
    void select_volume(ModelVolume *volume);
    void select_object();
    void select_range(const t_layer_height_range &range);
    bool create_at(const SurfacePoint &surface);
    bool face_volume(const Vec3d &xref, TriangleMesh &mesh, Pose &pose) const;
    void start_svg(const Vec2d &mouse);
    void set_placing(bool placing);
    void move_to(ModelVolume &volume, const SurfacePoint &surface);
    void apply_size(ModelVolume &volume, const Vec3d &size);
    void apply_angle(ModelVolume &volume, double angle);
    void flip(ModelVolume &volume);
    void align(ModelVolume &volume, const Vec3d &from_normal, const Vec3d &from_point, const Vec3d &to_normal, const Vec3d &to_point, bool coincide);
    void rebase_on_joint(ModelVolume &volume, const Vec3d &plane_normal, const Vec3d &plane_point);
    void change_nature(ModelVolume &volume, Nature nature);
    void change_shape(ModelVolume &volume, Shape shape);
    void delete_current();
    void delete_volume(const ObjectID &id);
    void delete_range(const t_layer_height_range &range);
    void start_measure();
    void stop_measure();

    // ---- références (cotes-contraintes : elles déplacent la forme, et la redimensionnent si cela ne suffit pas) ----
    PickedRef pick_reference(const Vec2d &mouse, bool on_tool, bool point_only);
    bool      make_dimension(const ModelVolume &volume, const PickedRef &volume_ref, const PickedRef &part_ref, int along, double value_mm,
                             ModifierDimension &out, wxString &error, double *current_mm = nullptr) const;
    double    instance_scale() const;
    bool      instance_is_rigid() const;
    void      dimension_equations(const ModelVolume &volume, const ModifierDimension &dimension, std::vector<std::pair<Vec3d, double>> &equations) const;
    bool      solve_dimensions(const ModelVolume &volume, const std::vector<ModifierDimension> &dimensions, Transform3d &world, Matrix3d *free_dirs,
                               std::array<bool, 3> *set_sizes = nullptr) const;
    IntervalTarget interval_target(const ModelVolume &volume, const ModifierDimension &dimension) const;
    bool      solve_interval(const ModelVolume &volume, const std::vector<ModifierDimension> &dimensions, Transform3d &world) const;
    bool      solve_pose(const ModelVolume &volume, const std::vector<ModifierDimension> &dimensions, Transform3d &world) const;
    Vec3d     dimension_end(const ModelVolume &volume, const ModifierDimension &dimension) const;
    int       conflicting_dimension(const ModelVolume &volume, const std::vector<ModifierDimension> &dimensions, size_t tested) const;
    double    dimension_error_mm(const ModelVolume &volume, const ModifierDimension &dimension) const;
    double    dimension_target_mm(const ModifierDimension &dimension) const;
    bool      has_dimensions(const ModelVolume &volume) const;
    bool      enforce_dimensions(ModelVolume &volume);
    void      rebase_dimensions(ModelObject &object, const Transform3d &old_to_new);
    void      constrained_drag(ModelVolume &volume, const Vec2d &mouse);
    void      start_references();
    void      stop_references();
    void      apply_reference_value();
    void      close_value();
    void      open_dimension(size_t index);
    void      set_dimension_value(size_t index, double value_mm);
    void      delete_dimension(size_t index);
    void      reapply_dimensions();
    wxString  dimension_label(const ModifierDimension &dimension) const;
    static wxString kind_label(RefKind kind);
    static wxString dimension_name(const ModifierDimension &dimension);
    void            rename_dimension(size_t index, const std::string &name);
    void finish_change(ModelVolume *reordered = nullptr);
    void begin_edit(const std::string &snapshot_name);
    bool load_shape_file();
    void refresh_layer_ranges();

    // ---- intervalles : une tranche entre deux plans qui couvre la pièce, montrée par les contours des plans ----
    std::vector<const ModelVolume *> solid_parts() const;
    std::vector<double>              parts_key() const;
    void                             parts_extent(const Vec3d &axis, double &low, double &high) const;
    IntervalPlanes                   interval_planes(const ModelVolume &volume) const;
    const IntervalOutlines          &interval_outlines(IntervalOutlines &cache, const IntervalPlanes &planes);
    Vec3d                            interval_center(const ModelVolume &volume) const;
    void                             fit_interval(ModelVolume &volume);
    void                             set_interval_planes(ModelVolume &volume, double offset, double thickness);
    PickedRef                        pick_interval_reference(const ModelVolume &volume, const Vec2d &mouse);
    PickedRef                        pick_outline_reference(const Vec2d &mouse, bool point_only);
    ColorRGBA                        standing_out(const ColorRGBA &own, float alpha) const;
    void                             render_outlines(const IntervalOutlines &outlines, const ColorRGBA &color, float width, int only = -1);
    void                             render_intervals();

    // ---- rendu ----
    void             render_xray();
    void             render_ghost();
    void             render_markers();
    void             render_mesh(const std::shared_ptr<const TriangleMesh> &mesh, const Transform3d &world, const ColorRGBA &color);
    void             render_sphere(const Vec3d &center, double radius, const ColorRGBA &color);
    void             render_circle(const Vec3d &center, double radius, const Vec3d &normal, double thickness, const ColorRGBA &color);
    void             render_segment(const Vec3d &a, const Vec3d &b, double radius, const ColorRGBA &color);
    void             render_plane_highlight(const PickMesh &pick, int plane, const ColorRGBA &color);
    void             render_reference(const PickedRef &ref, const ColorRGBA &color);
    void             render_dimensions();
    const PickMesh  *pick_mesh_of(const ObjectID &volume_id, bool on_tool) const;

    // ---- poignées : anneaux pour tourner (comme Pivoter), flèches pour déplacer (comme Déplacer), cubes pour l'échelle ----
    bool   handles_visible() const;
    void   handle_frame(const ModelVolume &volume, Vec3d &center, Matrix3d &axes, double &size) const;
    struct ScaleHandle
    {
        int    side{0}; // côté de la forme (axe de son maillage)
        Vec3d  tip{Vec3d::Zero()};
        Vec3d  dir{Vec3d::UnitZ()};
        double motion{1.};     // part de l'allongement du côté que fait sa face
        bool   blocked{false}; // taille fixée par ses cotes
    };
    std::array<ScaleHandle, 3> scale_handles(const ModelVolume &volume) const;
    Vec2d  to_screen(const Vec3d &point) const;
    int    handle_under_mouse(const Vec2d &mouse) const;
    double axis_parameter(const Vec3d &origin, const Vec3d &axis, const Vec2d &mouse) const;
    double ring_angle(const Vec3d &center, const Vec3d &axis, const Vec2d &mouse, double &distance) const;
    void   drag_handle(const Vec2d &mouse);
    void   rotate_volume(ModelVolume &volume, const Transform3d &start_world, const ModifierPlacement &start_placement, const Vec3d &center,
                         const Vec3d &axis, double degrees);
    void   slide_off(ModelVolume &volume, const Vec2d &mouse);
    void   render_handles();
    void   render_handle_fields(const ModelVolume &volume);
    void   render_scale_fields(const ModelVolume &volume);
    void   apply_handle_field(int axis, double value);
    bool   icon_button(ImTextureID &light, ImTextureID &dark, const std::string &name, float size, bool active);
    static ColorRGBA nature_color(Nature nature, float alpha);
    ColorRGBA        volume_color(const ModelVolume &volume, float alpha) const;
    ColorRGBA        ghost_color(Nature nature, float alpha) const;

    // ---- fenêtre ----
    void        render_nature_input(ModelVolume *volume);
    void        render_shape_input(ModelVolume *volume);
    void        render_place_input(const ModelVolume *volume);
    void        render_measure_button(float size);
    void        render_measure_panel();
    void        render_tool_steps();
    void        render_dimension_list(const ModelVolume &volume, float row_x);
    void        render_dimension_popup();
    void        render_dimension_labels();
    void        render_size_input(ModelVolume &volume);
    void        render_interval_input(ModelVolume &volume);
    void        render_enforcer_input(ModelVolume &volume);
    void        set_enforcer_setting(const ObjectID &volume_id, const std::string &key, int value);
    void        render_range_input();
    void        render_table();
    bool        slider_row(const wxString &label, const std::string &id, double &value, double lo, double hi, const char *format, float reserve = 0.f);
    bool        cross_button(const char *id, bool enabled, float size);
    std::string settings_summary(const ModelConfig &config, bool layer_range) const;

    // réglages du panneau : ce que le prochain clic pose (et, un volume sélectionné, les siens)
    Nature m_nature{Nature::Modifier};
    Shape  m_shape{Shape::Cube};
    bool   m_magnetism{true};
    bool   m_placing{false}; // « Placer » : la forme suit la souris sur la pièce, un clic la pose

    // « Mesurer » (en haut à droite de la fenêtre) : la Mesure de la barre, sans rien modifier
    std::unique_ptr<GLGizmoModifierMeasure> m_measure;
    bool                                    m_measuring{false};
    ImTextureID                             m_measure_icon{nullptr};
    ImTextureID                             m_measure_icon_dark{nullptr};
    float                                   m_top_row_reserve{0.f}; // place du bouton sur la 1re ligne
    ImTextureID                             m_move_icon{nullptr};
    ImTextureID                             m_rotate_icon{nullptr};
    ImTextureID                             m_rotate_icon_dark{nullptr};
    ImTextureID                             m_move_icon_dark{nullptr};
    ImTextureID                             m_scale_icon{nullptr};
    ImTextureID                             m_scale_icon_dark{nullptr};
    ImTextureID                             m_symmetric_icon{nullptr};
    ImTextureID                             m_symmetric_icon_dark{nullptr};
    ImTextureID                             m_select_icon{nullptr};
    ImTextureID                             m_select_icon_dark{nullptr};
    std::map<Shape, GLTexture>              m_shape_images; // images du menu « Forme » (resources/model)
    ImTextureID                             m_flip_icon{nullptr};
    ImTextureID                             m_flip_icon_dark{nullptr};
    ImTextureID                             m_join_icon{nullptr};
    ImTextureID                             m_join_icon_dark{nullptr};
    ImTextureID                             m_references_icon{nullptr};
    ImTextureID                             m_references_icon_dark{nullptr};
    ImTextureID                             m_lock_icon{nullptr};
    ImTextureID                             m_lock_icon_dark{nullptr};
    ImTextureID                             m_unlock_icon{nullptr};
    ImTextureID                             m_unlock_icon_dark{nullptr};
    bool   m_keep_ratio{true};
    // « Symétrie » par axe de la forme (une par ligne des cotes) : la cote change des deux côtés de son
    // centre ; sinon d'un seul (le dessous reste sur la pièce)
    std::array<bool, 3> m_symmetric{{true, true, false}};
    double m_new_angle{0.};
    double m_range_height{2.0}; // épaisseur d'un nouvel intervalle (mm)

    // fichier chargé (« Charger un fichier »)
    std::shared_ptr<TriangleMesh> m_loaded_mesh;
    std::string                   m_loaded_name;
    std::string                   m_loaded_path;

    // maillages des formes
    std::map<Shape, std::shared_ptr<TriangleMesh>> m_shape_meshes;

    // maillages de l'objet pour les rayons
    std::vector<PickMesh>                        m_parts; // pièces pleines (où l'on pose)
    std::vector<PickMesh>                        m_tools; // volumes de l'outil (que l'on déplace)
    std::map<const TriangleMesh *, PickCache>    m_pick_cache;
    FaceData                                     m_face;
    FaceSelection                                m_face_selection;

    // survol
    SurfacePoint m_hover;
    int          m_hover_tool{-1}; // index dans m_tools
    bool         m_shift_down{false};
    Vec2d        m_mouse{Vec2d::Zero()};

    // glisser
    struct Drag
    {
        ObjectID volume_id;
        Vec2d    start{Vec2d::Zero()};
        bool     moved{false};
        bool     created{false}; // posé par ce clic : son instantané d'annulation est déjà pris
        // volume coté : il glisse seulement le long des directions que ses cotes laissent libres,
        // dans un plan face à la caméra
        bool     constrained{false};
        Vec3d    start_offset{Vec3d::Zero()};
        Vec3d    plane_origin{Vec3d::Zero()};
        Vec3d    plane_normal{Vec3d::UnitZ()};
        Vec3d    start_point{Vec3d::Zero()};
        Matrix3d free_dirs{Matrix3d::Identity()};
    };
    std::optional<Drag> m_drag;
    bool                m_left_down_unused{false}; // clic dans le vide : désélectionne au relâcher
    Vec2d               m_left_down_pos{Vec2d::Zero()};

    // « Joindre »
    AlignStep          m_align{AlignStep::None};
    JointPick          m_join_hover;  // sous la souris
    JointPick          m_join_lock;   // face choisie d'un clic hors ancrage : ses ancrages, avec sa normale
    JointPick          m_join_first;  // origine choisie sur la forme
    std::vector<std::pair<Vec3d, int>> m_join_points; // ancrages montrés (-1 gris, sinon couleur de l'axe normal à leur plan)

    // poignées de la forme sélectionnée : anneaux (tourner) ou, après l'icône Déplacer du tableau,
    // flèches (déplacer) ; Échelle : cubes sur ses faces, et ses champs au bout de la ligne
    enum class HandleMode : unsigned char { None, Move, Rotate, Scale };
    HandleMode            m_handle_mode{HandleMode::None};
    ObjectID              m_handle_volume; // la forme dont on a allumé Pivoter / Déplacer / Échelle
    int                   m_handle_hover{-1};
    std::array<double, 6> m_handle_fields{}; // champs X / Y / Z au bout de la ligne (Échelle : en %, puis en mm)
    Vec3d                 m_scale_reference{Vec3d::Ones()}; // Échelle : tailles de ses côtés quand on l'a allumée (100 %)
    bool                  m_uniform_scale{true};
    struct HandleDrag
    {
        ObjectID          volume_id;
        int               axis{-1};
        bool              ring{true};
        bool              moved{false};
        Vec2d             start_mouse{Vec2d::Zero()};
        Transform3d       start_world{Transform3d::Identity()};
        ModifierPlacement start_placement;
        Vec3d             center{Vec3d::Zero()};
        Vec3d             axis_dir{Vec3d::UnitZ()};
        double            last_angle{0.};
        double            size{1.};
        double            accumulated{0.}; // degrés (anneau) ou mm (flèche)
        double            value{0.};       // appliqué, après les crans
        double            start_param{0.};
        Vec3d             start_offset{Vec3d::Zero()};
        Matrix3d          free_dirs{Matrix3d::Identity()};
        bool              scale{false}; // un cube (Échelle) : « side », « motion », « start_size » ; « value » = nouvelle taille
        int               side{0};
        double            motion{1.};
        Vec3d             start_size{Vec3d::Ones()};
    };
    std::optional<HandleDrag> m_handle_drag;

    // références : étape, référence survolée, celles choisies, distance tapée
    RefStep             m_ref_step{RefStep::None};
    ObjectID            m_ref_volume_id;
    PickedRef           m_ref_hover;
    PickedRef           m_ref_first;  // sur la forme
    PickedRef           m_ref_second; // sur la pièce
    int                 m_ref_along{3}; // deux points : 0 X, 1 Y, 2 Z de la pièce, 3 en ligne droite
    double              m_ref_value{0.};  // mm, dans le champ
    bool                m_ref_focus{false};
    int                 m_ref_edit{-1}; // cote modifiée (clic sur son étiquette), -1 = nouvelle
    RefStep             m_ref_before_edit{RefStep::None};
    wxString            m_ref_error;
    std::vector<double> m_dimension_buffers; // champs des cotes dans la fenêtre
    char                m_dimension_name[128]{}; // nom d'une cote en cours de saisie (son champ dans la fenêtre)

    // plage de hauteur sélectionnée dans la liste, et ses bords en cours de saisie
    std::optional<t_layer_height_range> m_current_range;
    std::optional<t_layer_height_range> m_range_buffer;
    wxString                            m_warning;
    // sélection dont le panneau a repris les valeurs
    size_t                              m_synced_volume_id{size_t(-1)};
    std::optional<t_layer_height_range> m_synced_range;

    // modification au réglet / au clavier en cours
    bool m_edit_snapshot{false};
    bool m_pending_finish{false};

    // actions décidées dans la fenêtre, faites après elle (elles changent la sélection ou la liste)
    std::optional<Nature>               m_pending_nature;
    std::optional<Shape>                m_pending_shape;
    std::optional<ObjectID>             m_pending_select;
    std::optional<t_layer_height_range> m_pending_select_range;
    bool                                m_pending_select_object{false};
    std::optional<ObjectID>             m_pending_flip;       // outils d'une ligne : Inverser,
    std::optional<ObjectID>             m_pending_join;       // Joindre (recliqué : annulé),
    std::optional<ObjectID>             m_pending_references; // Références (recliqué : arrêtées),
    std::optional<ObjectID>             m_pending_lock;       // cadenas (recliqué : déverrouillé)
    std::optional<ObjectID>             m_pending_delete_volume;
    std::optional<t_layer_height_range> m_pending_delete_range;
    std::optional<bool>                 m_pending_measure; // allumer / éteindre « Mesurer »
    std::optional<std::pair<ObjectID, HandleMode>> m_pending_handle_toggle; // boutons Pivoter / Déplacer / Échelle d'une ligne
    std::optional<std::pair<int, double>>          m_pending_handle_field;  // valeur tapée dans X / Y / Z (Échelle : 0 à 5)
    bool                                m_pending_ref_apply{false};
    bool                                m_pending_ref_cancel{false};
    bool                                m_pending_ref_delete{false};
    std::optional<std::pair<size_t, double>> m_pending_dimension_value;
    std::optional<size_t>               m_pending_dimension_delete;
    std::optional<size_t>               m_pending_dimension_open; // clic sur l'étiquette 3D d'une cote
    bool                                m_pending_dimensions_reapply{false};
    std::optional<std::pair<size_t, std::string>> m_pending_dimension_rename;
    bool                                m_pending_color{false}; // couleur d'un modificateur en cours de choix
    std::optional<std::tuple<ObjectID, std::string, int>> m_pending_enforcer; // réglage de supports d'un forceur choisi
    std::array<float, 3>                m_color_rgb{{1.f, 1.f, 0.f}}; // celle du sélecteur (non arrondie à « #RRGGBB »)
    bool                                m_pending_load{false};
    int                                 m_pending_tool{-1};

    // fenêtre flottante
    float  m_label_width{0.f};
    float  m_field_width{0.f};

    // modèles de rendu
    std::map<const TriangleMesh *, std::pair<std::shared_ptr<const TriangleMesh>, std::shared_ptr<GLModel>>> m_mesh_models;
    GLModel                                                                                                  m_sphere_model;
    GLModel                                                                                                  m_cylinder_model;
    GLModel                                                                                                  m_cone_model;
    GLModel                                                                                                  m_cube_model;
    std::array<std::shared_ptr<GLModel>, 3>                                                                  m_ring_models;
    std::array<std::array<double, 8>, 3>                                                                     m_ring_keys{};
    // faces surlignées (alignement, références) : par maillage, face et décalage
    std::map<std::tuple<const TriangleMesh *, int, int>, std::shared_ptr<GLModel>>                          m_plane_models;
    std::shared_ptr<GLModel>                                                                                 m_circle_model;
    std::array<double, 8>                                                                                    m_circle_key{};
    // intervalles : contours de leurs plans (par volume), de l'aperçu de « Placer », de la plage de hauteur
    // choisie dans la liste d'objets ; centres de gravité (pivots de « Pivoter »)
    std::map<size_t, IntervalOutlines>                              m_interval_outlines;
    IntervalOutlines                                                m_ghost_outlines;
    IntervalOutlines                                                m_range_outlines;
    mutable std::map<size_t, std::pair<std::vector<double>, Vec3d>> m_interval_centers;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoModifiers_hpp_
