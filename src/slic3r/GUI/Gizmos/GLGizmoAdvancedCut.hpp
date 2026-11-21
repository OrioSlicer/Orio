#ifndef slic3r_GLGizmoAdvancedCut_hpp_
#define slic3r_GLGizmoAdvancedCut_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmoRotate.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Measure.hpp" // Orio : ancrages du plan de coupe (sommets, arêtes, cercles, faces)
#include "libslic3r/ExPolygon.hpp" // Orio : sections de la face coupée (connecteurs)

#include <imgui/imgui.h> // Orio : ImTextureID (icône du bouton « Choisir sur la pièce »)
#include <map>

namespace Slic3r {
enum class CutConnectorType : int;
class ModelVolume;
struct CutConnectorAttributes;

namespace GUI {
enum class SLAGizmoEventType : unsigned char;

namespace CommonGizmosDataObjects {
class ObjectClipper;
}
class PartSelection
{
public:
    PartSelection() = default;
    PartSelection(
        const ModelObject *mo, const Transform3d &cut_matrix, int instance_idx, const Vec3d &center, const Vec3d &normal, const CommonGizmosDataObjects::ObjectClipper &oc);
    PartSelection(const ModelObject *mo, int instance_idx_in);
    ~PartSelection()
    {
        m_model.clear_objects();
        for (size_t i = 0; i < m_cut_parts.size(); i++) {
            if (m_cut_parts[i].raycaster) { delete m_cut_parts[i].raycaster; }
        }
    }

    struct PartPara
    {
        GLModel        glmodel;
        MeshRaycaster* raycaster;
        bool           is_up_part;
        Transform3d    trans;
        bool           is_modifier;
    };
    void         part_render(const Vec3d *cut_center, const Vec3d *normal);
    void         toggle_selection(const Vec2d &mouse_pos);
    void         toggle_selection(int id);
    void         turn_over_selection();
    ModelObject* model_object() { return m_model.objects.front(); }
    bool         valid() const { return m_valid; }
    bool         is_one_object() const;

    const std::vector<size_t> *get_ignored_contours_ptr() const { return (valid() ? &m_ignored_contours : nullptr); }

    std::vector<Cut::Part> get_cut_parts();
    std::vector<PartPara> &get_parts() { return m_cut_parts; }
    bool                   has_modified_cut_parts();

private:
    Model                                                            m_model;
    int                                                              m_instance_idx;
    std::vector<PartPara>                                            m_cut_parts;
    std::vector<bool>                                                m_back_cut_parts_state;
    bool                                                             m_valid = false;
    std::vector<std::pair<std::vector<size_t>, std::vector<size_t>>> m_contour_to_parts; // for each contour, there is a vector of parts above and a vector of parts below
    std::vector<size_t> m_ignored_contours; // contour that should not be rendered (the parts on both sides will both be parts of the same object)

    std::vector<Vec3d>              m_contour_points; // Debugging
    std::vector<std::vector<Vec3d>> m_debug_pts;      // Debugging
    void                            add_object(const ModelObject *object);
};

class GLGizmoAdvancedCut : public GLGizmoRotate3D
{
private:
    unsigned int m_last_active_item_imgui{0};
    double m_snap_step{1.0};
    // archived values
    Vec3d m_ar_plane_center{Vec3d::Zero()};

    // plane_center and so on
    Vec3d m_plane_center{Vec3d::Zero()};//old name:m_cut_plane_center
    Vec3d m_plane_center_drag_start{Vec3d::Zero()};
    Vec3d m_plane_drag_start{Vec3d::Zero()};
    Vec3d m_bb_center{Vec3d::Zero()};//box center
    Vec3d m_center_offset{Vec3d::Zero()};

    Vec3d m_plane_normal{Vec3d::UnitZ()}; //old namce:Vec3d m_cut_normal//m_cut_plane_normal
    Vec3d m_plane_x_direction{Vec3d::UnitY()};
    Vec3d m_clp_normal{Vec3d::Ones()};
    // data to check position of the cut palne center on gizmo activation
    Vec3d m_min_pos{Vec3d::Zero()};
    Vec3d m_max_pos{Vec3d::Zero()};

    static const double Offset;
    static const double Margin;
    static const std::array<float, 4> GrabberColor;
    static const std::array<float, 4> GrabberHoverColor;

    mutable double m_movement;
    double m_start_movement;
    double m_start_height;

    Vec3d m_rotation;

    Vec3d m_buffered_rotation;
    double m_buffered_movement;
    double m_buffered_height;

    Vec3d m_drag_pos_start;

    bool m_keep_upper;
    bool m_keep_lower;
    bool m_cut_to_parts{false};
    bool m_place_on_cut_upper{true};
    bool m_place_on_cut_lower{false};
    bool m_rotate_upper{false};
    bool m_rotate_lower{false};

    bool m_do_segment;
    double m_segment_smoothing_alpha;
    int m_segment_number;

    mutable Grabber m_move_z_grabber;
    mutable Grabber m_move_x_grabber;

    bool m_connectors_editing{false};
    CutConnectors m_connectors_before_editing; // Orio : ceux que « Annuler » remet
    bool m_localized_cut_editing = true;
    Vec3d m_localized_cut_pos = Vec3d::Zero();
    bool m_add_connector_ok{false};
    std::vector<size_t> m_invalid_connectors_idxs;
    bool m_show_shortcuts{false};

    std::vector<std::pair<wxString, wxString>> m_connector_shortcuts;
    std::vector<std::pair<wxString, wxString>> m_cut_plane_shortcuts;
    std::vector<std::pair<wxString, wxString>> m_cut_groove_shortcuts;
    double m_label_width{150.0};
    double m_control_width{ 200.0 };
    double m_editing_window_width;

    CutMode                  m_cut_mode{CutMode::cutPlanar};
    CutConnectorType         m_connector_type;
    size_t                   m_connector_style;
    size_t                   m_connector_shape_id;

      // Dovetail para
    Groove             m_groove;
    bool               m_groove_editing{false};
    float              m_contour_width{0.4f};
    float              m_cut_plane_radius_koef{1.5f};
    float              m_shortcut_label_width{-1.f};
    bool               m_is_slider_editing_done{false};
    bool               m_hide_cut_plane{false};
    double             m_radius{0.0};
    double             m_grabber_radius{0.0};
    double             m_grabber_connection_len{0.0};
    Vec3d              m_cut_plane_start_move_pos{Vec3d::Zero()};
    bool               m_cut_plane_as_circle{false};
    std::vector<Vec3d> m_groove_vertices;
    bool               m_was_cut_plane_dragged{false};
    bool               m_was_contour_selected{false};
    bool               m_is_dragging{false};
    bool               m_auto_center_connector{true}; // Orio : « Magnétisme des ancrages » (ancrages de la section)
    std::shared_ptr<PartSelection>    m_part_selection{nullptr};
    // dragging angel in hovered axes
    double             m_rotate_angle{0.0};
    bool               m_imperial_units{false};
    BoundingBoxf3      m_bounding_box;
    BoundingBoxf3      m_transformed_bounding_box;

    float m_connector_depth_ratio{3.f};
    float m_connector_depth_ratio_tolerance{CUT_CONNECTOR_TOLERANCE}; // Orio : 0,15 mm

    float m_connector_size{2.5f};
    float m_connector_size_tolerance{CUT_CONNECTOR_TOLERANCE}; // Orio : 0,15 mm sur le rayon
    // Input params for cut with snaps
    float        m_snap_space_proportion{0.3f};
    float        m_snap_bulge_proportion{0.15f};

    TriangleMesh m_connector_mesh;

    // remember the connectors which is selected
    mutable std::vector<bool> m_selected;
    int                       m_selected_count{0};

    GLModel m_plane; // old name:PickingModel

    GLModel m_grabber_model;

    Vec3d m_cut_line_begin{Vec3d::Zero()};
    Vec3d m_cut_line_end{Vec3d::Zero()};

    GLModel m_cut_line_model;

    Transform3d m_rotate_matrix{Transform3d::Identity()};
    Transform3d m_start_dragging_m{Transform3d::Identity()};
    std::map<CutConnectorAttributes, GLModel> m_shapes;

    // ---- Orio : repère du plan de coupe et plans définis par des ancrages ----
    // La « Position » du plan se mesure le long de sa normale, depuis une origine : par défaut le
    // point le plus bas de la pièce (mesure vers le haut), ou un point choisi sur la pièce (pivot,
    // centre des rotations du plan).
    enum class OriginType : unsigned char { Bottom, Custom };
    // Ce que l'utilisateur est en train de cliquer sur la pièce.
    enum class PickMode : unsigned char { None, Origin, Face, ThreePoints, EdgePoint, EdgeAngle, Circle, CircleAxis };
    // Ancrage trouvé sous la souris.
    enum class PickKind : unsigned char { None, Vertex, EdgeMid, EdgePoint, CircleCenter, PolygonCenter, FacePoint, Edge, Circle, Face };

    struct PickHover
    {
        PickKind kind{PickKind::None};
        Vec3d    point{Vec3d::Zero()};         // point retenu (monde)
        Vec3d    edge_a{Vec3d::Zero()};        // arête (monde)
        Vec3d    edge_b{Vec3d::Zero()};
        Vec3d    circle_center{Vec3d::Zero()}; // cercle (monde)
        Vec3d    circle_normal{Vec3d::UnitZ()};
        double   circle_radius{0.0};
        Vec3d    face_normal{Vec3d::UnitZ()};  // normale sortante de la face survolée (monde)
        Vec3d    facet_center{Vec3d::Zero()};  // centre du triangle survolé : côté matière de la face
        int      volume{-1};                   // face survolée (surbrillance)
        int      plane{-1};
        bool     valid() const { return kind != PickKind::None; }
    };

    struct PickVolume
    {
        const TriangleMesh                 *mesh{nullptr};
        Transform3d                         trafo{Transform3d::Identity()};
        std::unique_ptr<MeshRaycaster>      raycaster;
        std::unique_ptr<Measure::Measuring> measuring;
    };

    // Axe autour duquel le champ « Angle » fait tourner le plan (arête, ou axe d'un cercle).
    // Actif tant que le plan contient la direction de l'axe.
    struct Hinge
    {
        bool  active{false};
        Vec3d point{Vec3d::Zero()};
        Vec3d axis{Vec3d::UnitX()};
        Vec3d ref0{Vec3d::UnitZ()};  // normale du plan à 0°
        Vec3d ref90{Vec3d::UnitY()}; // normale du plan à 90°
    };

    OriginType              m_origin_type{OriginType::Bottom};
    Vec3d                   m_origin{Vec3d::Zero()};
    PickKind                m_origin_kind{PickKind::None};
    ImTextureID             m_select_icon{nullptr};      // icône du bouton « Choisir sur la pièce » (celle de
    ImTextureID             m_select_icon_dark{nullptr}; // « Sélectionner » des Modificateurs), thème clair / sombre
    Hinge                   m_hinge;
    double                  m_buffered_position{0.0};
    double                  m_buffered_hinge_angle{0.0};
    bool                    m_position_slider_editing{false};
    bool                    m_arrow_moved{false};

    PickMode                 m_pick_mode{PickMode::None};
    PickHover                m_pick_hover;
    // bouton appuyé pendant le choix d'ancrages, en attente du relâchement : un clic agit, un glisser
    // fait tourner (gauche) ou déplacer (droit) la vue sans rien choisir ni annuler
    enum class PickPress : unsigned char { None, Left, Right };
    PickPress                m_pick_press{PickPress::None};
    Vec2d                    m_pick_press_pos{Vec2d::Zero()};
    std::vector<PickHover>   m_picked; // ancrages déjà cliqués pour la construction en cours
    std::vector<PickVolume>  m_pick_volumes;
    GLModel                  m_pick_sphere;
    GLModel                  m_pick_cylinder;
    std::shared_ptr<GLModel> m_pick_face_model;
    std::pair<int, int>      m_pick_face_key{-1, -1};
    std::shared_ptr<GLModel> m_pick_torus;
    std::array<double, 8>    m_pick_torus_key{};
    wxString                 m_pick_warning;

    // ---- Orio : plusieurs plans de coupe ----
    // Le plan actif vit dans les membres habituels (m_plane_center, m_rotate_matrix, origine, axe) : tout
    // l'outil continue de ne connaître qu'un plan, les autres attendent ici.
    struct SavedPlane
    {
        Vec3d         center;
        Transform3d   rotation;
        int           origin_type;
        Vec3d         origin;
        int           origin_kind;
        Hinge         hinge;
        CutConnectors connectors; // ceux du plan actif vivent dans mo->cut_connectors
    };
    // planes vide = un seul plan ; sinon au moins 2. La case du plan actif est périmée jusqu'à store_active_plane().
    struct PlaneList
    {
        std::vector<SavedPlane> planes;
        int                     active{0};
        ObjectID                object;                            // pièce de ces plans
        Transform3d             instance{Transform3d::Identity()}; // sa matrice d'instance quand ils ont été posés
        bool                    place_on_cut{false};               // false = Maintenir orientation
        std::vector<uint32_t>   dropped;                           // morceaux décochés dans « Après la coupe », par leurs côtés (plane_cells)
    };
    PlaneList                   m_multi;
    struct CellsCache // plane_cells(), refait quand les plans ou la pièce changent
    {
        std::vector<std::pair<Vec3d, double>> planes;
        ObjectID                              object;
        Transform3d                           instance{Transform3d::Identity()};
        std::vector<uint32_t>                 cells;
    };
    mutable CellsCache          m_cells_cache;
    std::map<size_t, PlaneList> m_parked_planes; // plans laissés en quittant l'outil, par id d'objet (« pour cet objet »)
    ImTextureID                 m_delete_icon{nullptr};

    // ---- Orio : connecteurs précis (filetage ISO, ancrages de la section, motifs) ----
    // Sections = îlots de la face coupée, dans le repère du plan de coupe (origine m_plane_center,
    // axes X/Y de m_rotate_matrix), en mm. Repère propre de chaque section : barycentre, axe de la
    // longueur u et de la largeur v (côtés du plus petit rectangle englobant).
    // Droites guides d'une section : dessinées au survol (médianes, diagonales, rayons, axes de
    // symétrie), et proposées comme axes pour la symétrie des connecteurs (avec longueur / largeur).
    enum class GuideKind : unsigned char { SymmetryAxis, LengthAxis, WidthAxis, CornerRay, SideRay, LengthMedian, WidthMedian, Diagonal };
    struct SectionGuide
    {
        Vec2d     a{Vec2d::Zero()};
        Vec2d     b{Vec2d::Zero()};
        GuideKind kind{GuideKind::LengthAxis};
    };
    struct SectionIsland
    {
        ExPolygon                       expoly;            // à l'échelle (scale_), pour Clipper
        std::vector<std::vector<Vec2d>> rings;             // contour puis trous, en mm
        double                          area{0.0};
        Vec2d                           centroid{Vec2d::Zero()};
        Vec2d                           axis_u{Vec2d::UnitX()};
        Vec2d                           axis_v{Vec2d::UnitY()};
        Vec2d                           box_min{Vec2d::Zero()}; // rectangle englobant, en (u, v) depuis le barycentre
        Vec2d                           box_max{Vec2d::Zero()};
        Vec2d                           pole{Vec2d::Zero()};    // point le plus éloigné des bords
        double                          pole_radius{0.0};
        std::vector<Vec2d>              hole_centers;
        bool                            polygonal{false}; // à coins, ni ronde ni rectangulaire : ancrages sur des rayons
        std::vector<SectionGuide>       guides;
    };
    // Ancrages : les premiers (barycentre … milieux des côtés) passent avant les subdivisions (grille
    // de la longueur × largeur, médianes et diagonales du rectangle, rayons vers les coins et côtés).
    enum class AnchorKind : unsigned char {
        Centroid, Pole, BoxCenter, HoleCenter, Focus, Corner, SideMiddle,
        Fraction, LengthMedian, WidthMedian, Diagonal, CornerRay, SideRay
    };
    struct SectionAnchor
    {
        Vec2d      pos{Vec2d::Zero()};
        AnchorKind kind{AnchorKind::Fraction};
        int        island{-1};
        Vec2i      frac_a{0, 1}; // fraction (numérateur, dénominateur) : de la longueur, ou le long de la médiane / diagonale / du rayon
        Vec2i      frac_b{0, 1}; // Fraction : de la largeur
    };
    // Motifs de connecteurs.
    enum class PatternMode : unsigned char { None, Circle, Grid, Contour, Line, Auto };

    std::vector<SectionIsland> m_sections;
    std::vector<SectionAnchor> m_section_anchors;
    std::vector<double>        m_sections_key;
    int                        m_hover_anchor{-1};
    int                        m_hover_island{-1};
    // symétrie des connecteurs : un axe (droite guide) ou deux points (ancrages ou points libres)
    // choisis sur la face
    bool                       m_mirror_picking{false};
    int                        m_mirror_hover_anchor{-1};
    int                        m_mirror_hover_island{-1};
    int                        m_mirror_hover_guide{-1};
    Vec2d                      m_mirror_hover_point{Vec2d::Zero()}; // point sous la souris (aimanté sur un ancrage)
    bool                       m_mirror_has_first{false};
    Vec2d                      m_mirror_first{Vec2d::Zero()};       // premier point de l'axe

    PatternMode          m_pattern_mode{PatternMode::None};
    int                  m_pattern_island{-1}; // -1 = toutes les sections
    int                  m_pattern_count{3};
    int                  m_pattern_center{1};  // cercle, grille : point cliqué, barycentre, point le plus éloigné des bords, centre de la boîte, centre d'un trou
    Vec2d                m_pattern_center_point{Vec2d::Zero()}; // centre cliqué sur la face (repère du plan)
    bool                 m_pattern_center_set{false};
    double               m_pattern_radius{10.0};
    double               m_pattern_angle{90.0};
    int                  m_pattern_cols{2};
    int                  m_pattern_rows{2};
    double               m_pattern_spacing_u{10.0}; // grille : espacement le long de la longueur (X) et de la largeur (Y)
    double               m_pattern_spacing_v{10.0};
    double               m_pattern_margin{5.0};
    double               m_pattern_start{0.0}; // le long du bord : décalage du départ (% d'un intervalle)
    std::vector<Vec2d>   m_pattern_line;       // entre deux points : A puis B (repère du plan)
    std::vector<Vec2d>   m_pattern_preview;    // connecteurs du motif, avant ajout (repère du plan)
    std::vector<double>  m_pattern_key;        // paramètres de l'aperçu calculé
    double               m_buffered_connector_u{0.0}; // coordonnées du connecteur sélectionné (repère de sa section)
    double               m_buffered_connector_v{0.0};

    size_t m_thread_designation{0}; // index dans ISO_METRIC_THREADS
    size_t m_thread_class{0};       // 0 = 6g / 6H, 1 = 4g6g / 6H
    size_t m_thread_hand{0};        // 0 = à droite, 1 = à gauche

    struct InvalidConnectorsStatistics
    {
        unsigned int outside_cut_contour;
        unsigned int outside_bb;
        bool         is_overlap;

        void invalidate()
        {
            outside_cut_contour = 0;
            outside_bb          = 0;
            is_overlap          = false;
        }
    } m_info_stats;

    //GLSelectionRectangle m_selection_rectangle;

public:
    GLGizmoAdvancedCut(GLCanvas3D& parent, unsigned int sprite_id);

    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;
    bool on_key(const wxKeyEvent &evt) override;

    double get_movement() const { return m_movement; }
    void reset_rotation();
    std::string get_tooltip() const override;

    BoundingBoxf3 bounding_box() const;
    BoundingBoxf3 transformed_bounding_box(const Vec3d &plane_center, const Transform3d &rotation_m = Transform3d::Identity()) const;

    bool is_looking_forward() const;

    bool unproject_on_cut_plane(const Vec2d &mouse_pos, Vec3d &pos, Vec3d &pos_world, bool respect_contours = true);

    virtual bool apply_clipping_plane() { return m_connectors_editing; }

    BoundingBoxf3 get_bounding_box() const override;

    std::string get_icon_filename(bool b_dark_mode) const override;
    bool        wants_enter_leave_snapshots() const override { return true; }

protected:
    virtual bool on_init();
    virtual void on_load(cereal::BinaryInputArchive &ar) override;
    virtual void on_save(cereal::BinaryOutputArchive &ar) const override;
    virtual void data_changed(bool is_serializing) override;
    virtual std::string on_get_name() const;
    virtual std::string on_get_name_str() override { return "Cut"; }
    void    apply_color_clip_plane_colors();
    virtual void on_set_state();
    void         close();
    virtual bool on_is_activable() const;
    virtual CommonGizmosDataID on_get_requirements() const override;
    virtual void on_start_dragging() override;
    virtual void on_stop_dragging() override;
    virtual void update_plate_center(Axis axis_type, double projection, bool is_abs_move); // old name:dragging_grabber_move
    virtual void update_plate_normal_boundingbox_clipper(const Transform3d &rotation_tmp); // old name:dragging_grabber_rotation
    virtual void on_update(const UpdateData& data);
    virtual bool on_mouse(const wxMouseEvent &mouse_event) override;
    virtual void on_render();
    virtual void on_render_for_picking();
    virtual void on_render_input_window(float x, float y, float bottom_limit);

    void show_tooltip_information(float x, float y);

    virtual void on_enable_grabber(unsigned int id)
    {
        if (id < 3)
            m_gizmos[id].enable_grabber(0);
        else if (id == 3)
            this->enable_grabber(0);
    }

    virtual void on_disable_grabber(unsigned int id)
    {
        if (id < 3)
            m_gizmos[id].disable_grabber(0);
        else if (id == 3)
            this->disable_grabber(0);
    }

    virtual void on_set_hover_id()
    {
        for (int i = 0; i < 3; ++i)
            m_gizmos[i].set_hover_id((m_hover_id == i) ? 0 : -1);
    }

private:
    void perform_cut(const Selection& selection);
    bool can_perform_cut() const;
    void apply_connectors_in_model(ModelObject *mo, int &dowels_count, const Transform3d &rotation, const Vec3d &normal);

    bool is_selection_changed(bool alt_down, bool shift_down);
    void select_connector(int idx, bool select);

    double calc_projection(const Vec3d &drag_pos, const Linef3 &mouse_ray, const Vec3d &project_dir) const;

    Vec3d get_plane_normal() const;
    Vec3d get_plane_center() const;

    void reset_cut_plane();
    void reset_all();

    // update the connectors position so that the connectors are on the cut plane
    void put_connectors_on_cut_plane(const Vec3d &cp_normal, double cp_offset);
    void update_plane_normal();
    void update_clipper();
    // on render
    void render_cut_plane_and_grabbers();
    void on_render_rotate_gizmos();
    void render_connectors();
    void render_localized_cut_shadow();
    void render_clipper_cut();
    void render_cut_line();

    void clear_selection();
    void init_connector_shapes();
    void set_connectors_editing(bool connectors_editing);
    void reset_connectors();
    void update_connector_shape();
    void apply_selected_connectors(std::function<void(size_t idx)> apply_fn);
    void select_all_connectors();
    void unselect_all_connectors();
    void validate_connector_settings();
    bool add_connector(CutConnectors &connectors, const Vec2d &mouse_position);
    bool delete_selected_connectors();
    bool is_outside_of_cut_contour(size_t idx, const CutConnectors &connectors, const Vec3d cur_pos);
    bool is_conflict_for_connector(size_t idx, const CutConnectors &connectors, const Vec3d cur_pos);
    //deal groove
    void switch_to_mode(CutMode new_mode);
    void flip_cut_plane();
    void update_plane_model();
    void init_picking_models();
    bool has_valid_groove() const;
    bool has_valid_contour() const;
    void reset_cut_by_contours();
    void render_flip_plane_button(bool disable_pred = false);
    void process_contours();
    void toggle_model_objects_visibility(bool show_in_3d = false);
    void deal_connector_pos_by_type(Vec3d &pos, float &height, CutConnectorType, CutConnectorStyle, bool looking_forward, bool is_edit, const Vec3d &clp_normal);
    void update_bb();
    void check_and_update_connectors_state();
    void set_center(const Vec3d &center, bool update_tbb = false);
    bool set_center_pos(const Vec3d &center_pos, bool update_tbb = false);
    void invalidate_cut_plane();
    void rotate_vec3d_around_plane_center(Vec3d &vec, const Transform3d &rotate_matrix, const Vec3d &center);
    Transform3d get_cut_matrix(const Selection &selection);
    Transform3d get_cut_matrix(const Selection &selection, const Vec3d &center, const Transform3d &rotation);
    // render input window
    void update_buffer_data();
    bool render_cut_mode_combo(double label_width,float item_width);
    void render_color_marker(float size, const ColorRGBA &color);
    void render_cut_plane_input_window(float x, float y, float bottom_limit);
    void init_connectors_input_window_data();
    void render_connectors_input_window(float x, float y, float bottom_limit);
    void render_input_window_warning() const;
    bool render_reset_button(const std::string &label_id, const std::string &tooltip) const;
    bool render_connect_type_radio_button(CutConnectorType type);

    bool render_slider_double_input(const std::string &label, float &value_in, float &tolerance_in);
    bool render_slider_double_input_by_format(const std::string &label, float &value_in, float value_min, float value_max, DoubleShowType show_type = DoubleShowType::Normal);
    bool cut_line_processing() const;
    void discard_cut_line_processing();
    bool process_cut_line(SLAGizmoEventType action, const Vec2d &mouse_position);

    // ---- Orio : position absolue, origine (pivot), plans par ancrages ----
    Vec3d  current_plane_normal() const;
    Vec3d  rotation_pivot() const;
    Vec3d  position_axis() const;
    void   object_extent_along_axis(double &lo, double &hi) const;
    double get_position() const;
    void   get_position_range(double &lo, double &hi) const;
    void   set_position(double position);
    void   sync_archived_plane();
    void   rotate_plane_to(const Transform3d &rotation);
    void   set_origin_bottom();
    void   set_origin(const Vec3d &point, PickKind kind);
    void   apply_plane(const Vec3d &normal, const Vec3d &point, const Vec3d *x_dir);
    void   set_hinge(const Vec3d &point, const Vec3d &axis, const Vec3d &ref0, const Vec3d &ref90, double angle_deg);
    bool   hinge_usable() const;
    double get_hinge_angle() const;
    void   set_hinge_angle(double angle_deg);
    void   render_position_input(float caption_size, float space_size, float unit_size, unsigned int current_active_id);
    void   render_origin_and_anchors(float caption_size, float space_size, float unit_size);
    bool   is_picking() const { return m_pick_mode != PickMode::None; }
    void   start_picking(PickMode mode);
    void   stop_picking();
    void   init_pick_volumes();
    void   update_pick_hover();
    bool   pick_accepts(PickKind kind) const;
    void   on_pick_click();
    void   render_pick_markers();
    void   render_pick_sphere(const Vec3d &center, double radius, const ColorRGBA &color);
    void   render_pick_edge(const Vec3d &a, const Vec3d &b, double radius, const ColorRGBA &color);
    void   render_pick_circle(const Vec3d &center, double radius, const Vec3d &normal, double thickness, const ColorRGBA &color);
    double pixel_size_at(const Vec3d &point) const;
    wxString pick_kind_name(PickKind kind) const;
    wxString pick_instructions() const;

    // ---- Orio : plusieurs plans de coupe ----
    bool several_planes() const { return !m_multi.planes.empty(); }
    const ModelInstance *selected_instance() const;
    void store_active_plane();
    void load_plane(int idx);
    void add_plane();
    void delete_active_plane();
    void forget_planes();
    void park_planes();
    void restore_planes();
    bool plane_crosses_piece(int idx) const;
    std::vector<std::pair<Vec3d, Vec3d>> plane_frames() const;
    GLVolumeCollection::ColorClipSections color_clip_sections() const;
    const std::vector<uint32_t> &plane_cells() const;
    size_t dropped_pieces() const;
    bool all_pieces_dropped() const { const size_t n = dropped_pieces(); return n > 0 && n == plane_cells().size(); }
    void render_planes_row(float caption_size, float space_size);
    void cut_by_planes(ModelObject &object, const PlaneList &list, int instance_idx, const Selection &selection, Model &result);

    // ---- Orio : connecteurs précis ----
    Vec2d    world_to_plane2d(const Vec3d &world) const;
    Vec3d    plane2d_to_world(const Vec2d &point) const;
    Vec3d    world_to_connector_pos(const Vec3d &world) const;
    Vec3d    connector_pos_to_world(const Vec3d &pos) const;
    void     update_sections();
    int      section_at(const Vec2d &point) const;
    int      section_of(const Vec2d &point) const;
    int      connector_section(const CutConnector &connector) const;
    int      thread_on_section(const CutConnectors &connectors, int island, int skip = -1) const;
    double   section_edge_distance(int island, const Vec2d &point) const;
    bool     snap_to_section(Vec3d &world, bool update_hover, bool allow_holes = false);
    wxString anchor_name(const SectionAnchor &anchor) const;
    wxString guide_name(const SectionGuide &guide) const;
    CutConnectorParas current_connector_paras() const;
    void     add_connectors_at(const std::vector<Vec2d> &points);
    void     render_section_markers();
    void     render_pattern_preview();
    void     render_selected_connector_coordinates();
    void     render_patterns_input();
    void     update_pattern_preview();
    void     reset_pattern_defaults();
    Vec2d    pattern_center(const SectionIsland &island) const;
    bool     click_picks_pattern_point() const;
    void     mirror_selected_connectors(const Vec2d &a, const Vec2d &b);
    void     update_mirror_hover(const Vec3d &world);
    void     apply_mirror_pick();
    void     render_thread_input();
    float    connector_wall_margin() const;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoAdvancedCut_hpp_