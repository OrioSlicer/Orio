// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code, which overrides our localization "L" macro.
#include "GLGizmoAdvancedCut.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include <GL/glew.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/stattext.h>
#include <wx/sizer.h>

#include <algorithm>
#include "GLGizmosCommon.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "../GUI/MsgDialog.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"

#include <imgui/imgui_internal.h>
#include "FixModelByWin10.hpp"
// Orio : connecteurs précis (sections de la face coupée, motifs, filetage ISO)
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/IsoMetricThreads.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Utils.hpp" // Orio : resources_dir()
#include "libslic3r/AABBMesh.hpp" // Orio : connecteurs de chaque plan, pièce qui les reçoit
#include <cereal/types/vector.hpp> // Orio : connecteurs des plans dans l'archive du gizmo
#include <cstdint>
#include <queue>
#include <set>
namespace Slic3r {
namespace GUI {

const double       units_in_to_mm = 25.4;
const double       units_mm_to_in = 1 / units_in_to_mm;

const int c_connectors_group_id = 6;
const int c_cube_z_move_id      = 3;
const int c_cube_x_move_id      = 4;
const int c_plate_move_id       = 5;
const int c_connectors_start_id = c_connectors_group_id - c_cube_z_move_id;
const float UndefFloat = -999.f;

// connector colors
static const ColorRGBA PLAG_COLOR           = ColorRGBA::YELLOW();
static const ColorRGBA DOWEL_COLOR          = ColorRGBA::DARK_YELLOW();
static const ColorRGBA HOVERED_PLAG_COLOR   = ColorRGBA::CYAN();
static const ColorRGBA HOVERED_DOWEL_COLOR  = {0.0f, 0.5f, 0.5f, 1.0f};
static const ColorRGBA SELECTED_PLAG_COLOR  = ColorRGBA::GRAY();
static const ColorRGBA SELECTED_DOWEL_COLOR = ColorRGBA::GRAY(); // DARK_GRAY();
static const ColorRGBA CONNECTOR_DEF_COLOR  = {1.0f, 1.0f, 1.0f, 0.5f};
static const ColorRGBA CONNECTOR_ERR_COLOR  = {1.0f, 0.3f, 0.3f, 0.5f};
static const ColorRGBA HOVERED_ERR_COLOR    = {1.0f, 0.3f, 0.3f, 1.0f};

static const ColorRGBA CUT_PLANE_DEF_COLOR = {0.9f, 0.9f, 0.9f, 0.5f};
static const ColorRGBA CUT_PLANE_ERR_COLOR = {1.0f, 0.8f, 0.8f, 0.5f};

static const ColorRGBA UPPER_PART_COLOR = ColorRGBA::CYAN();
static const ColorRGBA LOWER_PART_COLOR = ColorRGBA::MAGENTA();
static const ColorRGBA MODIFIER_COLOR   = {0.75f, 0.75f, 0.75f, 0.5f};
// Orio : couleurs des sections avec plusieurs plans (les 7 de gouraud.fs), les deux premières comme avec un seul
// plan ; ni rouge ni rose (plan hors de la pièce), ni jaune (connecteurs), ni gris
static const std::array<ColorRGBA, 7> SECTION_COLORS = {LOWER_PART_COLOR, UPPER_PART_COLOR, ColorRGBA{1.0f, 0.55f, 0.1f, 1.0f},
    ColorRGBA{0.3f, 0.75f, 0.25f, 1.0f}, ColorRGBA{0.25f, 0.45f, 0.95f, 1.0f}, ColorRGBA{0.6f, 0.35f, 0.95f, 1.0f}, ColorRGBA{0.6f, 0.9f, 0.1f, 1.0f}};

// Orio : la couleur que gouraud.fs donne au point (GLGizmoAdvancedCut::color_clip_sections)
static ColorRGBA section_color(const GLVolumeCollection::ColorClipSections &sections, const Vec3d &point)
{
    int total = 0;
    for (size_t i = 0; i < sections.planes.size(); ++i) {
        const std::array<float, 4> &plane = sections.planes[i];
        if (plane[0] * point.x() + plane[1] * point.y() + plane[2] * point.z() + plane[3] > 0.)
            total += int(sections.weights[i]);
    }
    return sections.colors[total % sections.colors.size()];
}

// Orio : plans de coupe (centre, normale) en (normale, d) : p est au-dessus du plan i si normale·p + d > 0, bit i
// des côtés d'un morceau. families : famille de chaque plan (plans parallèles) ; flips : plans à l'envers de leur famille,
// orientée comme son premier plan (gouraud.fs colore avec les côtés ^ flips).
// ponytail: 32 plans au plus (bits d'un uint32_t), les suivants sont ignorés
static std::vector<std::pair<Vec3d, double>> cut_planes(const std::vector<std::pair<Vec3d, Vec3d>> &frames, std::vector<size_t> &families, uint32_t &flips)
{
    std::vector<std::pair<Vec3d, double>> planes;
    std::vector<Vec3d>                    firsts; // normale du premier plan de chaque famille
    flips = 0;
    for (size_t i = 0; i < std::min<size_t>(frames.size(), 32); ++i) {
        const Vec3d normal = frames[i].second.normalized();
        size_t      family = 0;
        while (family < firsts.size() && std::abs(firsts[family].dot(normal)) < 0.9999)
            ++family;
        if (family == firsts.size())
            firsts.push_back(normal);
        else if (firsts[family].dot(normal) < 0.)
            flips |= 1u << i;
        planes.emplace_back(normal, -normal.dot(frames[i].first));
        families.push_back(family);
    }
    return planes;
}

// Orio : la couleur que gouraud.fs donne au morceau (côtés ^ flips, 16 plans au plus comme color_clip_sections)
static ColorRGBA cell_color(uint32_t oriented, const std::vector<size_t> &families)
{
    size_t total = 0;
    for (size_t i = 0; i < std::min<size_t>(families.size(), 16); ++i)
        if ((oriented >> i) & 1)
            total += size_t(1) << (families[i] % 3);
    return SECTION_COLORS[total % SECTION_COLORS.size()];
}

// Orio : ajoute à cells les côtés de chaque bout du polygone coupé par les plans k et suivants (voir list_cells)
static void add_cells(const std::vector<Vec3d> &poly, const std::vector<std::pair<Vec3d, double>> &planes, size_t k, uint32_t cell, std::set<uint32_t> &cells)
{
    for (; k < planes.size(); ++k) {
        std::vector<double> d;
        bool                above = false, below = false;
        for (const Vec3d &p : poly) {
            d.push_back(planes[k].first.dot(p) + planes[k].second);
            above = above || d.back() > 0.01;
            below = below || d.back() < -0.01;
        }
        if (!above && !below)
            return; // posé sur le plan : ne fait aucun morceau
        if (above && below) { // coupé en deux (Sutherland-Hodgman)
            std::vector<Vec3d> up, down;
            for (size_t i = 0; i < poly.size(); ++i) {
                const size_t j = (i + 1) % poly.size();
                if (d[i] >= 0.)
                    up.push_back(poly[i]);
                if (d[i] <= 0.)
                    down.push_back(poly[i]);
                if ((d[i] > 0. && d[j] < 0.) || (d[i] < 0. && d[j] > 0.)) {
                    const Vec3d x = poly[i] + d[i] / (d[i] - d[j]) * (poly[j] - poly[i]);
                    up.push_back(x);
                    down.push_back(x);
                }
            }
            add_cells(up, planes, k + 1, cell | (1u << k), cells);
            add_cells(down, planes, k + 1, cell, cells);
            return;
        }
        if (above)
            cell |= 1u << k;
    }
    cells.insert(cell);
}

// Orio : côtés de chaque morceau que les plans feront de la pièce, lus sur sa surface coupée par les plans, dans
// l'ordre de leurs noms : rang dans la famille du plan 1 (de bas en haut le long de sa normale), puis dans la suivante…
// À moins de 0,01 mm d'un plan, un point est dessus, comme pour le test « traversé » de cut_by_planes.
// ponytail: un morceau tout intérieur, sans surface de la pièce, manque : il est gardé et numéroté après les autres
static std::vector<uint32_t> list_cells(const ModelObject &object, const Transform3d &instance, const std::vector<std::pair<Vec3d, double>> &planes,
                                        const std::vector<size_t> &families, uint32_t flips)
{
    const uint32_t     all = planes.size() >= 32 ? ~0u : (1u << planes.size()) - 1;
    std::set<uint32_t> cells;
    for (const ModelVolume *volume : object.volumes) {
        if (!volume->is_model_part())
            continue;
        const Transform3d           trafo = instance * volume->get_matrix();
        const indexed_triangle_set &its   = volume->mesh().its;
        std::vector<Vec3d>          points(its.vertices.size());
        std::vector<uint32_t>       above(its.vertices.size(), 0), below(its.vertices.size(), 0);
        for (size_t v = 0; v < its.vertices.size(); ++v) {
            points[v] = trafo * its.vertices[v].cast<double>();
            for (size_t k = 0; k < planes.size(); ++k) {
                const double d = planes[k].first.dot(points[v]) + planes[k].second;
                if (d > 0.01)
                    above[v] |= 1u << k;
                else if (d < -0.01)
                    below[v] |= 1u << k;
            }
        }
        for (const auto &face : its.indices) {
            const uint32_t up   = above[face[0]] | above[face[1]] | above[face[2]];
            const uint32_t down = below[face[0]] | below[face[1]] | below[face[2]];
            if ((up & down) != 0) // traversé par un plan
                add_cells({points[face[0]], points[face[1]], points[face[2]]}, planes, 0, 0, cells);
            else if ((up | down) == all) // posé sur aucun plan
                cells.insert(up);
        }
    }
    size_t family_count = 0;
    for (const size_t family : families)
        family_count = std::max(family_count, family + 1);
    const auto key = [&families, family_count, flips](uint32_t cell) {
        std::vector<int> ranks(family_count, 0);
        for (size_t i = 0; i < families.size(); ++i)
            ranks[families[i]] += int(((cell ^ flips) >> i) & 1);
        return ranks;
    };
    std::vector<uint32_t> sorted(cells.begin(), cells.end());
    std::stable_sort(sorted.begin(), sorted.end(), [&key](uint32_t a, uint32_t b) { return key(a) < key(b); });
    return sorted;
}

// Orio : côtés d'un morceau de cut_by_planes, ceux du barycentre des sommets de ses parties : il est dans la cellule
// des plans, convexe. Les connecteurs, qui débordent sur le voisin, ne comptent que si le morceau n'a qu'eux.
static uint32_t piece_cell(const ModelObject &piece, int instance_idx, const std::vector<std::pair<Vec3d, double>> &planes)
{
    Vec3d  sum   = Vec3d::Zero();
    size_t count = 0;
    for (const bool connectors : {false, true}) {
        for (const ModelVolume *volume : piece.volumes)
            if (volume->is_model_part() && volume->cut_info.is_connector == connectors) {
                const Transform3d trafo = piece.instances[instance_idx]->get_matrix() * volume->get_matrix();
                for (const auto &vertex : volume->mesh().its.vertices) {
                    sum += trafo * vertex.cast<double>();
                    ++count;
                }
            }
        if (count > 0)
            break;
    }
    uint32_t cell = 0;
    for (size_t k = 0; k < planes.size() && count > 0; ++k)
        if (planes[k].first.dot(sum / double(count)) + planes[k].second > 0.)
            cell |= 1u << k;
    return cell;
}

static Vec3d rotate_vec3d_around_vec3d_with_rotate_matrix(
    const Vec3d& rotate_point,
    const Vec3d& origin_point,
    const Transform3d& rotate_matrix)
{
    Transform3d translate_to_point = Transform3d::Identity();
    translate_to_point.translate(origin_point);
    Transform3d translate_to_zero = Transform3d::Identity();
    translate_to_zero.translate(-origin_point);
    return (translate_to_point * rotate_matrix * translate_to_zero) * rotate_point;
}

static inline void rotate_point_2d(double& x, double& y, const double c, const double s)
{
    double xold = x;
    double yold = y;
    x = c * xold - s * yold;
    y = s * xold + c * yold;
}

static void rotate_x_3d(std::array<Vec3d, 4>& verts, float radian_angle)
{
    double c = cos(radian_angle);
    double s = sin(radian_angle);
    for (uint32_t i = 0; i < verts.size(); ++i)
        rotate_point_2d(verts[i](1), verts[i](2), c, s);
}

static void rotate_y_3d(std::array<Vec3d, 4>& verts, float radian_angle)
{
    double c = cos(radian_angle);
    double s = sin(radian_angle);
    for (uint32_t i = 0; i < verts.size(); ++i)
        rotate_point_2d(verts[i](2), verts[i](0), c, s);
}

static void rotate_z_3d(std::array<Vec3d, 4>& verts, float radian_angle)
{
    double c = cos(radian_angle);
    double s = sin(radian_angle);
    for (uint32_t i = 0; i < verts.size(); ++i)
        rotate_point_2d(verts[i](0), verts[i](1), c, s);
}

const double GLGizmoAdvancedCut::Offset = 20.0;
const double GLGizmoAdvancedCut::Margin = 20.0;
const std::array<float, 4> GLGizmoAdvancedCut::GrabberColor      = { 1.0, 1.0, 0.0, 1.0 };
const std::array<float, 4> GLGizmoAdvancedCut::GrabberHoverColor = { 0.7, 0.7, 0.0, 1.0};

GLGizmoAdvancedCut::GLGizmoAdvancedCut(GLCanvas3D& parent, unsigned int sprite_id)
    : GLGizmoRotate3D(parent, sprite_id, nullptr)
    , m_movement(0.0)
    , m_buffered_movement(0.0)
    , m_keep_upper(true)
    , m_keep_lower(true)
    , m_cut_to_parts(false)
    , m_do_segment(false)
    , m_segment_smoothing_alpha(0.5)
    , m_segment_number(5)
    , m_connector_type(CutConnectorType::Plug)
    , m_connector_style(size_t(CutConnectorStyle::Prizm))
    , m_connector_shape_id(size_t(CutConnectorShape::Circle))
{
    set_group_id(m_gizmos.size());
    m_buffered_rotation.setZero();
    m_thread_designation = size_t(iso_metric_thread_closest(10.)); // Orio : M10x1.5 par défaut
}

bool GLGizmoAdvancedCut::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    // Orio : choix d'ancrages sur la pièce (origine, face, points, arête, cercle).
    // C'est le CLIC qui agit, au relâchement (appui puis relâchement sans glisser de plus de 5 pixels,
    // le seuil du canevas) : gauche, retient l'ancrage survolé ; droit, annule le dernier ancrage, ou
    // la sélection s'il n'y en a pas. Un glisser, même commencé sur la pièce, fait tourner (gauche)
    // ou déplacer (droit) la vue sans rien choisir ni annuler : avant, l'appui choisissait déjà
    // (une face, une origine… terminait le choix) et l'appui droit l'annulait. Échap : annule.
    if (is_picking()) {
        switch (action) {
        case SLAGizmoEventType::LeftDown:
            if (!m_pick_hover.valid()) {
                m_pick_press = PickPress::None;
                return false; // hors de la pièce : la vue tourne normalement
            }
            // appui gardé (pas de sélection ni de déplacement de la pièce par le canevas) ; un glisser
            // fait quand même tourner la vue : le canevas la fait tourner dès le premier mouvement
            m_pick_press     = PickPress::Left;
            m_pick_press_pos = mouse_position;
            return true;
        case SLAGizmoEventType::RightDown:
            m_pick_press     = PickPress::Right;
            m_pick_press_pos = mouse_position;
            return true;
        case SLAGizmoEventType::Dragging:
            if (m_pick_press != PickPress::None && (mouse_position - m_pick_press_pos).cwiseAbs().maxCoeff() > 5.)
                m_pick_press = PickPress::None; // glisser : la vue tourne ou se déplace, pas de clic
            return false;
        case SLAGizmoEventType::LeftUp: {
            // (Ctrl pendant un glisser arrive ici aussi, avec control_down : pas un clic)
            const bool click = m_pick_press == PickPress::Left && !control_down;
            m_pick_press     = PickPress::None;
            if (click && m_pick_hover.valid()) {
                on_pick_click();
                m_parent.set_as_dirty();
            }
            return true;
        }
        case SLAGizmoEventType::RightUp: {
            const bool click = m_pick_press == PickPress::Right;
            m_pick_press     = PickPress::None;
            if (click) {
                if (!m_picked.empty()) {
                    m_picked.pop_back();
                    m_pick_warning.clear();
                } else
                    stop_picking();
                m_parent.set_as_dirty();
            }
            return true;
        }
        case SLAGizmoEventType::Escape:
            m_pick_press = PickPress::None;
            stop_picking();
            m_parent.set_as_dirty();
            return true;
        default: return false;
        }
    }

    CutConnectors &connectors = m_c->selection_info()->model_object()->cut_connectors;

    if (shift_down && !m_connectors_editing &&
        (action == SLAGizmoEventType::LeftDown || action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::Dragging)) {
        process_cut_line(action, mouse_position);
        return true;
    } else { // if (!shift_down)
        discard_cut_line_processing();
    }

    if (action == SLAGizmoEventType::LeftDown) {
        if (m_hover_id == c_plate_move_id) {
            Vec3d pos;
            Vec3d pos_world;
            if (unproject_on_cut_plane(mouse_position, pos, pos_world, false)) {
                m_plane_drag_start = pos_world;
            }
        }
        if (!m_connectors_editing)
            return false;

        // Orio : Ctrl + clic = toujours un nouveau connecteur, même sur un connecteur existant
        // (un ancrage caché derrière le connecteur sélectionné reste atteignable) ; il devient la sélection
        if (control_down && !shift_down && !alt_down) {
            add_connector(connectors, mouse_position);
            return true;
        }

        if (m_hover_id != -1) {
            start_dragging();
            return true;
        }

        if (shift_down || alt_down) {
            // left down with shift - show the selection rectangle:
            //if (m_hover_id == -1)
            //    m_selection_rectangle.start_dragging(mouse_position, shift_down ? GLSelectionRectangle::EState::Select : GLSelectionRectangle::EState::Deselect);
        } else if (m_hover_id == -1 && m_mirror_picking) {
            // Orio : symétrie de la sélection par rapport à l'axe ou au point survolé
            apply_mirror_pick();
            m_add_connector_ok = false;
            m_parent.set_as_dirty();
        } else if (m_hover_id == -1 && m_pattern_mode == PatternMode::Line && m_pattern_line.size() < 2) {
            // Orio : motif « entre deux points » : le clic choisit A puis B (magnétisme compris)
            Vec3d pos, pos_world;
            if (unproject_on_cut_plane(mouse_position, pos, pos_world)) {
                snap_to_section(pos_world, false);
                m_pattern_line.push_back(world_to_plane2d(pos_world));
                m_add_connector_ok = false;
                m_parent.set_as_dirty();
            }
        } else if (m_hover_id == -1 && (m_pattern_mode == PatternMode::Circle || m_pattern_mode == PatternMode::Grid)) {
            // Orio : motifs cercle et grille : le clic choisit le centre (magnétisme compris,
            // centre d'un trou compris) au lieu de poser un connecteur. Le centre reste sur la face
            // coupée : dans le contour d'une section (ses trous compris) ; un clic à côté de la pièce
            // est ignoré.
            Vec3d pos, pos_world;
            if (unproject_on_cut_plane(mouse_position, pos, pos_world, false)) {
                snap_to_section(pos_world, false, true);
                const Vec2d center  = world_to_plane2d(pos_world);
                const Point scaled  = Point::new_scale(center);
                const bool  on_face = std::any_of(m_sections.begin(), m_sections.end(),
                                                  [&scaled](const SectionIsland &island) { return island.expoly.contour.contains(scaled); });
                m_add_connector_ok  = false;
                if (on_face) {
                    m_pattern_center_point = center;
                    m_pattern_center       = 0;
                    m_pattern_center_set   = true;
                    m_parent.set_as_dirty();
                }
            }
        } else {
            // If there is no selection and no hovering, add new point
            if (m_hover_id == -1 && !shift_down && !alt_down)
                add_connector(connectors, mouse_position);
                    //m_ldown_mouse_position = mouse_position;
        }
        return true;
    }
    else if (action == SLAGizmoEventType::LeftUp) {
        if (m_connectors_editing && control_down && !shift_down && !alt_down)
            return true; // Ctrl + clic : le connecteur ajouté reste seul sélectionné
        if (m_connectors_editing && m_connector_type == CutConnectorType::Snap && m_add_connector_ok && m_hover_id == -1) {
            return true;//Snap connector special logic, due to gaps in the middle of the snap, other connectors such as cylinders are solid
        }
        // (pendant le choix de symétrie, la sélection reste : c'est elle qui sera copiée)
        if (m_hover_id == -1 && !shift_down && !alt_down && !m_mirror_picking)
            unselect_all_connectors();

        is_selection_changed(alt_down, shift_down);
        return true;
    }
    else if (action == SLAGizmoEventType::RightDown) {
        if (m_connectors_editing && m_mirror_picking) { // Orio : symétrie : retire le premier point, sinon abandonne
            if (m_mirror_has_first)
                m_mirror_has_first = false;
            else
                m_mirror_picking = false;
            m_parent.set_as_dirty();
            return true;
        }
        if (!m_connectors_editing && m_cut_mode == CutMode::cutPlanar && !several_planes()) { //&& control_down
            // Check the internal part raycasters.
            if (m_part_selection && !m_part_selection->valid()) {
                process_contours();
            }
            m_part_selection->toggle_selection(mouse_position);
            check_and_update_connectors_state(); // after a contour is deactivated, its connectors are inside the object

            return true;
        }
        if (m_hover_id < c_connectors_group_id)
            return false;

        unselect_all_connectors();
        select_connector(m_hover_id - c_connectors_group_id, true);
        return delete_selected_connectors();
    }
    else if (action == SLAGizmoEventType::RightUp) {
        // catch right click event
        return true;
    }
    else if (action == SLAGizmoEventType::Escape && m_connectors_editing) {
        // Orio : Échap abandonne d'abord le choix de symétrie, puis désélectionne les
        // connecteurs ; l'outil ne se ferme que s'il n'y a rien de tout cela
        if (m_mirror_picking) {
            m_mirror_picking   = false;
            m_mirror_has_first = false;
            m_parent.set_as_dirty();
            return true;
        }
        if (m_selected_count > 0) {
            unselect_all_connectors();
            m_parent.set_as_dirty();
            return true;
        }
    }

    return false;
}

bool GLGizmoAdvancedCut::on_key(const wxKeyEvent& evt)
{
    // Orio : flèches haut / bas = plan déplacé de ±0,1 mm le long de sa normale (Maj : ±1 mm).
    // Une seule étape d'annulation par appui, prise au relâchement de la touche.
    const int  key   = evt.GetKeyCode();
    const bool arrow = key == WXK_UP || key == WXK_DOWN || key == WXK_NUMPAD_UP || key == WXK_NUMPAD_DOWN;
    if (arrow && m_state == On && !m_connectors_editing && !is_picking()) {
        if (evt.GetEventType() == wxEVT_KEY_DOWN) {
            const double step = evt.ShiftDown() ? 1.0 : 0.1;
            const bool   up   = key == WXK_UP || key == WXK_NUMPAD_UP;
            if (m_cut_mode == CutMode::cutTongueAndGroove)
                m_groove_editing = true; // la rainure est recalculée au relâchement
            set_position(get_position() + (up ? step : -step));
            reset_cut_by_contours();
            m_arrow_moved = true;
        } else if (evt.GetEventType() == wxEVT_KEY_UP && m_arrow_moved) {
            m_arrow_moved    = false;
            m_groove_editing = false;
            reset_cut_by_contours();
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Move cut plane");
            sync_archived_plane();
        }
        m_parent.set_as_dirty();
        return true;
    }

    bool ctrl_down = evt.GetModifiers() & wxMOD_CONTROL;

    if (evt.GetKeyCode() == WXK_DELETE) {
        return delete_selected_connectors();
    }
    else if (ctrl_down
        && (evt.GetKeyCode() == 'A' || evt.GetKeyCode() == 'a'))
    {
        select_all_connectors();
        return true;
    }
    return false;
}

std::string GLGizmoAdvancedCut::get_tooltip() const
{
    std::string tooltip;
    if (m_dragging && (m_hover_id == c_plate_move_id || m_hover_id == c_cube_z_move_id)) {
        double               koef     = m_imperial_units ? units_mm_to_in : 1.0;
        std::string          unit_str = " " + (m_imperial_units ? _u8L("in") : _u8L("mm"));
        const BoundingBoxf3 &tbb      = m_transformed_bounding_box;

        const std::string name = m_cut_to_parts ? _u8L("Part") : _u8L("Object");
        // Orio : position du plan depuis son origine
        tooltip += _u8L("Cut position") + ": " + format(static_cast<float>(get_position() * koef), 2) + unit_str + "\n";
        if (tbb.max.z() >= 0.0) {
            double top = (tbb.min.z() <= 0.0 ? tbb.max.z() : tbb.size().z()) * koef;
            tooltip += format(static_cast<float>(top), 2) + " " + unit_str + " (" + name + " A)";
            if (tbb.min.z() <= 0.0) tooltip += "\n";
        }
        if (tbb.min.z() <= 0.0) {
            double bottom = (tbb.max.z() <= 0.0 ? tbb.size().z() : (tbb.min.z() * (-1))) * koef;
            tooltip += format(static_cast<float>(bottom), 2) + " " + unit_str + " (" + name + " B)";
        }
        return tooltip;
    }

    if (!m_dragging && m_hover_id == c_plate_move_id) {
        if (m_cut_mode == CutMode::cutTongueAndGroove || several_planes()) return _u8L("Drag to move the cut plane");
        return _u8L("Drag to move the cut plane\n"
                    "Right-click a part to assign it to the other side");
    }

    if (tooltip.empty() && (m_hover_id == X || m_hover_id == Y || m_hover_id == Z)) {
        std::string axis = m_hover_id == X ? "X" : m_hover_id == Y ? "Y" : "Z";
        return axis + ": " + format(float(Geometry::rad2deg(m_rotate_angle)), 1) + _u8L("°");
    }

    return tooltip;
}

BoundingBoxf3 GLGizmoAdvancedCut::bounding_box() const
{
    BoundingBoxf3                 ret;
    const Selection &             selection = m_parent.get_selection();
    const Selection::IndicesList &idxs      = selection.get_volume_idxs();
    for (unsigned int i : idxs) {
        const GLVolume *volume = selection.get_volume(i);
        // respect just to the solid parts for FFF and ignore pad and supports for SLA
        if (!volume->is_modifier && !volume->is_sla_pad() && !volume->is_sla_support())
            ret.merge(volume->transformed_convex_hull_bounding_box());
    }
    return ret;
}

BoundingBoxf3 GLGizmoAdvancedCut::transformed_bounding_box(const Vec3d &plane_center, const Transform3d &rotation_m) const
{
    const Selection &selection = m_parent.get_selection();

    const auto first_volume    = selection.get_first_volume();
    BoundingBoxf3 ret;
    if (!first_volume) {
        return ret;
    }
    Vec3d      instance_offset = first_volume->get_instance_offset();
    instance_offset[Z] += first_volume->get_sla_shift_z();

    const auto cut_matrix = Transform3d::Identity() * rotation_m.inverse() * Geometry::translation_transform(instance_offset - plane_center);

    const Selection::IndicesList &idxs = selection.get_volume_idxs();
    for (unsigned int i : idxs) {
        const GLVolume *volume = selection.get_volume(i);
        // respect just to the solid parts for FFF and ignore pad and supports for SLA
        if (!volume->is_modifier && !volume->is_sla_pad() && !volume->is_sla_support()) {
            const auto instance_matrix = volume->get_instance_transformation().get_matrix_no_offset();
            auto       volume_trafo    = instance_matrix * volume->get_volume_transformation().get_matrix();
            ret.merge(volume->transformed_convex_hull_bounding_box(cut_matrix * volume_trafo));
        }
    }
    return ret;
}

bool GLGizmoAdvancedCut::is_looking_forward() const
{
    const Camera &camera = wxGetApp().plater()->get_camera();
    const double  dot    = camera.get_dir_forward().dot(m_plane_normal);
    return dot < 0.05;
}

// Unprojects the mouse position on the mesh and saves hit point and normal of the facet into pos_and_normal
// Return false if no intersection was found, true otherwise.
bool GLGizmoAdvancedCut::unproject_on_cut_plane(const Vec2d &mouse_pos, Vec3d &pos, Vec3d &pos_world, bool respect_contours)
{
    const float sla_shift = m_c->selection_info()->get_sla_shift();

    const ModelObject *  mo     = m_c->selection_info()->model_object();
    const ModelInstance *mi     = mo->instances[m_c->selection_info()->get_active_instance()];
    const Camera &       camera = wxGetApp().plater()->get_camera();

    // Calculate intersection with the clipping plane.
    const ClippingPlane *cp = m_c->object_clipper()->get_clipping_plane();
    Vec3d                point;
    Vec3d                direction;
    Vec3d                hit;
    MeshRaycaster::line_from_mouse_pos_static(mouse_pos, Transform3d::Identity(), camera, point, direction);
    Vec3d  normal = -cp->get_normal().cast<double>();
    double den    = normal.dot(direction);
    if (den != 0.) {
        double t = (-cp->get_offset() - normal.dot(point)) / den;
        hit      = (point + t * direction);
    } else
        return false;

    if (respect_contours) {
        // Do not react to clicks outside a contour (or inside a contour that is ignored)
        int cont_id = m_c->object_clipper()->is_projection_inside_cut(hit);
        if (cont_id == -1)
            return false;
        if (m_part_selection&&m_part_selection->valid()) {
            const std::vector<size_t> &ign = *m_part_selection->get_ignored_contours_ptr();
            if (std::find(ign.begin(), ign.end(), cont_id) != ign.end())
                return false;
        }
    }

    // recalculate hit to object's local position
    Vec3d hit_d = hit;
    hit_d -= mi->get_offset();
    hit_d[Z] -= sla_shift;

    // Return both the point and the facet normal.
    pos       = hit_d;
    pos_world = hit;

    return true;
}

void GLGizmoAdvancedCut::reset_cut_plane()
{
    // Orio : rotation remise à zéro AVANT de placer le centre (set_center le ramène sur la
    // pièce le long de la normale du plan) ; origine et axe d'angle oubliés
    if (is_picking())
        stop_picking();
    set_origin_bottom();
    m_hinge.active = false;
    m_start_dragging_m = m_rotate_matrix = Transform3d::Identity();
    m_transformed_bounding_box = transformed_bounding_box(m_bb_center);
    set_center(m_bb_center);
    update_plane_normal();
    m_ar_plane_center  = m_plane_center;

    reset_cut_by_contours();
    m_parent.request_extra_frame();

    const Selection& selection = m_parent.get_selection();
    const BoundingBoxf3& box = selection.get_bounding_box();

    m_movement = 0.0;
    m_rotation = Geometry::extract_euler_angles(m_rotate_matrix);

    update_buffer_data();
}

void GLGizmoAdvancedCut::reset_all()
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "reset cut");
    forget_planes();
    if (m_cut_mode == CutMode::cutPlanar) {
        reset_connectors();
    } else if (m_cut_mode == CutMode::cutTongueAndGroove) {
        m_groove.depth = m_groove.depth_init;
        m_groove.width = m_groove.width_init;
        m_groove.flaps_angle = m_groove.flaps_angle_init;
        m_groove.angle = m_groove.angle_init;
        m_groove.depth_tolerance = CUT_TOLERANCE;
        m_groove.width_tolerance = CUT_TOLERANCE;
    }
    reset_cut_plane();
    m_keep_upper = true;
    m_keep_lower = true;
    m_cut_to_parts = false;
    m_place_on_cut_upper = true;
    m_place_on_cut_lower = false;
    m_rotate_upper = false;
    m_rotate_lower = false;
}

BoundingBoxf3 GLGizmoAdvancedCut::get_bounding_box() const
{
    BoundingBoxf3 t_aabb;
    t_aabb.reset();

    // rotate aabb
    auto t_rotate_aabb = GLGizmoRotate3D::get_bounding_box();
    if (t_rotate_aabb.defined) {
        t_aabb.merge(t_rotate_aabb);
        t_aabb.defined = true;
    }
    // end rotate aabb

    bool is_render_z_grabber = true;                                      // m_hover_id < 0 || m_hover_id == cube_z_move_id;
    bool is_render_x_grabber = m_cut_mode == CutMode::cutTongueAndGroove; // m_hover_id < 0 || m_hover_id == cube_x_move_id;
    // m_move_z_grabber aabb
    if (is_render_z_grabber) {
        const auto& z_grabber = m_move_z_grabber.get_cube();
        if (z_grabber.is_initialized()) {
            auto z_grabber_aabb = z_grabber.get_bounding_box();
            const auto& z_grabber_model_matrix = m_move_z_grabber.m_matrix;
            z_grabber_aabb = z_grabber_aabb.transformed(z_grabber_model_matrix);
            z_grabber_aabb.defined = true;
            t_aabb.merge(z_grabber_aabb);
            t_aabb.defined = true;
        }
    }
    // end m_move_z_grabber

    // m_move_x_grabber aabb
    if (is_render_x_grabber) {
        const auto& x_grabber = m_move_x_grabber.get_cube();
        if (x_grabber.is_initialized()) {
            auto x_grabber_aabb = x_grabber.get_bounding_box();
            const auto& x_grabber_model_matrix = m_move_x_grabber.m_matrix;
            x_grabber_aabb = x_grabber_aabb.transformed(x_grabber_model_matrix);
            x_grabber_aabb.defined = true;
            t_aabb.merge(x_grabber_aabb);
            t_aabb.defined = true;
        }
    }
    // end m_move_x_grabber

    // m_plane aabb
    if (!m_connectors_editing) {
        if (m_plane.is_initialized()) {
            const auto merge_plane = [this, &t_aabb](const Vec3d &center, const Transform3d &rotation) {
                auto t_plane_aabb = m_plane.get_bounding_box();
                const auto t_plane_model_matrix = Geometry::translation_transform(center) * rotation;
                t_plane_aabb = t_plane_aabb.transformed(t_plane_model_matrix);
                t_plane_aabb.defined = true;
                t_aabb.merge(t_plane_aabb);
                t_aabb.defined = true;
            };
            merge_plane(m_plane_center, m_rotate_matrix);
            for (int i = 0; i < int(m_multi.planes.size()); ++i) // Orio : les plans en attente
                if (i != m_multi.active)
                    merge_plane(m_multi.planes[i].center, m_multi.planes[i].rotation);
        }
    }
    // end m_plane aabb

    return t_aabb;
}

std::string GLGizmoAdvancedCut::get_icon_filename(bool b_dark_mode) const
{
    return b_dark_mode ? "toolbar_cut_dark.svg" : "toolbar_cut.svg";
}

bool GLGizmoAdvancedCut::on_init()
{
    if (!GLGizmoRotate3D::on_init())
        return false;

    m_shortcut_key = WXK_CONTROL_C;

    // initiate info shortcuts
    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString alt   = GUI::shortkey_alt_prefix();
    const wxString shift = "Shift+";

    m_connector_shortcuts.push_back(std::make_pair(_L("Left click"), _L("Add connector")));
    m_connector_shortcuts.push_back(std::make_pair(_L("Right click"), _L("Remove connector")));
    m_connector_shortcuts.push_back(std::make_pair(_L("Drag"), _L("Move connector")));
    m_connector_shortcuts.push_back(std::make_pair(shift + _L("Left click"), _L("Add connector to selection")));
    m_connector_shortcuts.push_back(std::make_pair(alt + _L("Left click"), _L("Remove connector from selection")));
    m_connector_shortcuts.push_back(std::make_pair(ctrl + "A", _L("Select all connectors")));

    m_cut_plane_shortcuts.push_back(std::make_pair(shift + _L("Left drag"), _L("Plot cut plane")));
    m_cut_plane_shortcuts.push_back(std::make_pair(_L("right click"), _L("Assign the part to the other side")));
    // Orio : flèches haut / bas
    m_cut_plane_shortcuts.push_back(std::make_pair(_L("Arrows Up / Down"), _L("Move the plane by 0.1 mm (Shift: 1 mm)")));

    m_cut_groove_shortcuts.push_back(std::make_pair(shift + _L("Left click"), _L("Plot cut plane")));
    m_cut_groove_shortcuts.push_back(std::make_pair(_L("Arrows Up / Down"), _L("Move the plane by 0.1 mm (Shift: 1 mm)")));

    init_connector_shapes();
    return true;
}

std::string GLGizmoAdvancedCut::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        return _u8L("Cut") + ":\n" + _u8L("Please select single object.");
    } else {
        return _u8L("Cut");
    }
}

void GLGizmoAdvancedCut::apply_color_clip_plane_colors()
{
    if (CutMode(m_cut_mode) == CutMode::cutTongueAndGroove)
        m_parent.set_color_clip_plane_colors({CUT_PLANE_DEF_COLOR, CUT_PLANE_DEF_COLOR});
    else
        m_parent.set_color_clip_plane_colors({UPPER_PART_COLOR, LOWER_PART_COLOR});
}

void GLGizmoAdvancedCut::on_load(cereal::BinaryInputArchive &ar)
{
    size_t mode;
    float  groove_depth;
    float  groove_width;
    float  groove_flaps_angle;
    float  groove_angle;
    float  groove_depth_tolerance;
    float  groove_width_tolerance;

    ar(m_keep_upper, m_keep_lower, m_rotate_lower, m_rotate_upper, m_hide_cut_plane, mode, m_connectors_editing, m_ar_plane_center, m_rotate_matrix, groove_depth, groove_width,
       groove_flaps_angle, groove_angle, groove_depth_tolerance, groove_width_tolerance);
    // Orio : origine (pivot) et axe d'angle du plan
    int origin_type = 0, origin_kind = 0;
    ar(origin_type, m_origin, origin_kind, m_hinge.active, m_hinge.point, m_hinge.axis, m_hinge.ref0, m_hinge.ref90);
    m_origin_type = OriginType(origin_type);
    m_origin_kind = PickKind(origin_kind);
    // Orio : plans de coupe. La case du plan actif est périmée : ce plan vient d'être lu au-dessus.
    size_t plane_count = 0;
    ar(m_multi.active, m_multi.place_on_cut, plane_count, m_multi.object.id, m_multi.instance, m_multi.dropped);
    m_multi.planes.resize(plane_count);
    for (SavedPlane &plane : m_multi.planes)
        ar(plane.center, plane.rotation, plane.origin_type, plane.origin, plane.origin_kind, plane.hinge.active, plane.hinge.point, plane.hinge.axis, plane.hinge.ref0, plane.hinge.ref90, plane.connectors);
    ar(m_connectors_before_editing); // une annulation peut ramener dans une édition plus ancienne

    m_start_dragging_m = m_rotate_matrix;

    m_transformed_bounding_box = transformed_bounding_box(m_ar_plane_center, m_rotate_matrix);
    set_center_pos(m_ar_plane_center);
    m_rotation = Geometry::extract_euler_angles(m_rotate_matrix);
    m_movement = 0;
    if (m_cut_mode != (CutMode) mode)
        switch_to_mode((CutMode) mode);
    else if (m_cut_mode == CutMode::cutTongueAndGroove) {
        if (!is_approx(m_groove.depth, groove_depth) || !is_approx(m_groove.width, groove_width) || !is_approx(m_groove.flaps_angle, groove_flaps_angle) ||
            !is_approx(m_groove.angle, groove_angle) || !is_approx(m_groove.depth_tolerance, groove_depth_tolerance) ||
            !is_approx(m_groove.width_tolerance, groove_width_tolerance)) {
            m_groove.depth           = groove_depth;
            m_groove.width           = groove_width;
            m_groove.flaps_angle     = groove_flaps_angle;
            m_groove.angle           = groove_angle;
            m_groove.depth_tolerance = groove_depth_tolerance;
            m_groove.width_tolerance = groove_width_tolerance;
            update_plane_model();
        }
        reset_cut_by_contours();
    } else if (m_cut_mode == CutMode::cutPlanar) {
        reset_cut_by_contours();
    }
    if (is_picking()) // Orio : un choix d'ancrages en cours est abandonné
        stop_picking();

    m_parent.request_extra_frame();
    update_buffer_data();
}

void GLGizmoAdvancedCut::on_save(cereal::BinaryOutputArchive &ar) const
{
    ar(m_keep_upper, m_keep_lower, m_rotate_lower, m_rotate_upper, m_hide_cut_plane, (size_t) m_cut_mode, m_connectors_editing, m_ar_plane_center, m_start_dragging_m,
       m_groove.depth, m_groove.width, m_groove.flaps_angle, m_groove.angle, m_groove.depth_tolerance, m_groove.width_tolerance);
    // Orio : origine (pivot) et axe d'angle du plan. Ils ne changent que par les fonctions
    // de l'outil, qui prennent l'instantané d'annulation AVANT de les modifier : l'état courant est
    // donc celui d'avant l'action, comme m_ar_plane_center / m_start_dragging_m.
    ar(int(m_origin_type), m_origin, int(m_origin_kind), m_hinge.active, m_hinge.point, m_hinge.axis, m_hinge.ref0, m_hinge.ref90);
    ar(m_multi.active, m_multi.place_on_cut, m_multi.planes.size(), m_multi.object.id, m_multi.instance, m_multi.dropped);
    for (const SavedPlane &plane : m_multi.planes)
        ar(plane.center, plane.rotation, plane.origin_type, plane.origin, plane.origin_kind, plane.hinge.active, plane.hinge.point, plane.hinge.axis, plane.hinge.ref0, plane.hinge.ref90, plane.connectors);
    ar(m_connectors_before_editing);
}

void GLGizmoAdvancedCut::data_changed(bool is_serializing)
{
    // Orio : les maillages de la pièce ont pu être remplacés (annulation…) : les outils de
    // sélection d'ancrages pointent dessus, ils sont refaits
    m_pick_volumes.clear();
    m_pick_face_model.reset();
    m_pick_face_key = {-1, -1};
    m_cells_cache   = CellsCache{}; // morceaux de « Après la coupe », relus sur les nouveaux maillages
    if (is_picking())
        init_pick_volumes();
    reset_rotation();
    if (m_hover_id < 0) { // BBL
        // Orio : plans d'une autre pièce, ou d'une pièce déplacée, mis de côté avant que update_bb ne remette le plan à zéro
        if (!is_serializing && several_planes()) {
            const ModelInstance *instance = selected_instance();
            if (instance == nullptr || instance->get_object()->id() != m_multi.object || !instance->get_matrix().isApprox(m_multi.instance, 1e-6)) {
                park_planes();
                forget_planes();
            }
        }
        update_bb();
        if (!is_serializing && !several_planes())
            restore_planes();
        if (auto oc = m_c->object_clipper()) {
            oc->set_behaviour(m_connectors_editing, m_connectors_editing, double(m_contour_width));
        }
    }
}

void GLGizmoAdvancedCut::on_set_state()
{
    GLGizmoRotate3D::on_set_state();

    // Reset m_cut_z on gizmo activation
    if (get_state() == On) {
        m_parent.set_use_color_clip_plane(true);
        const Selection &selection = m_parent.get_selection();
        if (selection.is_empty()) {//check selection again
            close();
            return;
        }
        m_hover_id           = -1;
        m_connectors_editing = false;

        update_bb();
        reset_cut_plane();//according to boundingbox
        restore_planes(); // Orio : ceux laissés sur cette pièce (à l'ouverture par annulation, on_load passe ensuite)

        // Orio : avec plusieurs plans, l'outil se rouvre sur leurs pastilles, pas dans les connecteurs du plan actif
        m_connectors_editing = !m_selected.empty() && !several_planes();
        if (m_connectors_editing)
            m_connectors_before_editing = m_c->selection_info()->model_object()->cut_connectors;
        m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotate_matrix);

        // initiate archived values
        m_ar_plane_center  = m_plane_center;
        m_start_dragging_m = m_rotate_matrix;

        m_parent.request_extra_frame();
    }
    else if (get_state() == Off) {
        park_planes(); // Orio : les plans restent en mémoire pour cette pièce
        // Orio : fin d'une éventuelle sélection d'ancrages, mémoire des maillages rendue
        if (is_picking())
            stop_picking();
        m_pick_volumes.clear();
        m_pick_face_model.reset();
        m_pick_face_key = {-1, -1};
        m_pick_torus.reset();
        toggle_model_objects_visibility(true);
        if (auto oc = m_c->object_clipper()) {
            oc->set_behaviour(true, true, 0.);
            oc->release();
        }
        m_selected.clear();
        m_parent.set_use_color_clip_plane(false);
        m_c->selection_info()->set_use_shift(false);

        // Make sure that the part selection data are released when the gizmo is closed.
        // The CallAfter is needed because in perform_cut, the gizmo is closed BEFORE
        // the cut is performed (because of undo/redo snapshots), so the data would
        // be deleted prematurely.
        if (m_part_selection && m_part_selection->valid()) {
            wxGetApp().CallAfter([this]() {
                m_part_selection.reset(new PartSelection());
            });
        }
    }
}

void GLGizmoAdvancedCut::close()
{//close gizmo == open it again
    auto &mng = m_parent.get_gizmos_manager();
    if (mng.get_current_type() == GLGizmosManager::Cut)
        mng.open_gizmo(GLGizmosManager::Cut);
}

bool GLGizmoAdvancedCut::on_is_activable() const
{
    const Selection &selection  = m_parent.get_selection();
    const int        object_idx = selection.get_object_idx();
    if (object_idx < 0 || selection.is_wipe_tower())
        return false;

    if (const ModelObject *mo = wxGetApp().plater()->model().objects[object_idx]; mo->is_cut() && mo->volumes.size() == 1) {
        const ModelVolume *volume = mo->volumes[0];
        if (volume->is_cut_connector() && volume->cut_info.connector_type == CutConnectorType::Dowel)
            return false;
    }

    // This is assumed in GLCanvas3D::do_rotate, do not change this
    // without updating that function too.
    return selection.is_single_full_instance() && !m_parent.is_layers_editing_enabled();
}

CommonGizmosDataID GLGizmoAdvancedCut::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo)
        | int(CommonGizmosDataID::InstancesHider)
        | int(CommonGizmosDataID::ObjectClipper));
}

void GLGizmoAdvancedCut::on_start_dragging()
{
    if (m_connectors_editing && m_hover_id >= c_connectors_group_id) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Move connector");
        return;
    } else if (m_hover_id >= 0 && m_hover_id <= 2) {
        for (auto& gizmo : m_gizmos) {
            if (m_hover_id == gizmo.get_group_id()) {
                gizmo.start_dragging();
                break;
            }
        }
        m_rotate_angle = 0;
        m_plane_center_drag_start = m_plane_center; // Orio : rotation autour de l'origine choisie
    } else if (m_hover_id >= c_cube_z_move_id && c_connectors_group_id) {
        const Selection &    selection = m_parent.get_selection();
        const BoundingBoxf3 &box       = selection.get_bounding_box();
        m_start_movement               = 0;
        m_start_height                 = m_plane_center.z();
        m_plane_center_drag_start      = m_plane_center;
        if (m_hover_id == c_cube_z_move_id) {
            m_drag_pos_start = m_move_z_grabber.center;
        } else if (m_hover_id == c_cube_x_move_id) {
            m_drag_pos_start = m_move_x_grabber.center;
        } else if (m_hover_id == c_plate_move_id) {
            m_drag_pos_start = m_plane_drag_start;
        }
    }
    m_start_dragging_m = m_rotate_matrix;
}

void GLGizmoAdvancedCut::on_stop_dragging()
{
    if (m_is_dragging ==false) {
        return;
    }
    m_is_dragging = false;
    if (m_hover_id >= 0 && m_hover_id <= 2) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Rotate cut plane");
        m_start_dragging_m = m_rotate_matrix; // for takeshot
        m_ar_plane_center  = m_plane_center;  // Orio : le centre bouge aussi (pivot, retour sur la pièce)
        for (auto& gizmo : m_gizmos) {
            if (m_hover_id == gizmo.get_group_id()) {
                gizmo.stop_dragging();
                break;
            }
        }
    } else if (m_hover_id == c_cube_z_move_id || m_hover_id == c_cube_x_move_id || m_hover_id == c_plate_move_id) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Move cut plane"); // todo
        m_ar_plane_center = m_plane_center;
    }
    m_plane_center_drag_start = m_plane_center;
    if (!m_connectors_editing) {
        reset_cut_by_contours();
    }
    m_movement     = 0.0;
    m_rotation = Geometry::extract_euler_angles(m_rotate_matrix);
    update_buffer_data();
}

void GLGizmoAdvancedCut::update_plate_center(Axis axis_type, double projection, bool is_abs_move) {
    const Vec3d shift = m_rotate_matrix * ((axis_type == Axis::Z ? Vec3d::UnitZ() : axis_type == Axis::Y ? Vec3d::UnitY() : Vec3d::UnitX()) * projection);
    if (shift != Vec3d::Zero()) {
        // update m_plane_center
        if (is_abs_move) {
            this->set_center(m_plane_center_drag_start + shift, true);
        } else {
            this->set_center(m_plane_center + shift, true);
        }
    }
}

void GLGizmoAdvancedCut::update_plate_normal_boundingbox_clipper(const Transform3d &rotation_tmp)
{
    const bool        update_tbb   = !m_rotate_matrix.rotation().isApprox(rotation_tmp.rotation());
    if (update_tbb) {
        m_rotate_matrix            = rotation_tmp;
        m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotate_matrix);
        // Orio : un plan tourné autour d'un point hors de la pièce pourrait ne plus la
        // couper : il est alors ramené sur son extrémité
        set_center_pos(m_plane_center);
        update_clipper();
    }
}

void GLGizmoAdvancedCut::on_update(const UpdateData& data)
{
    // 1. GHOST LOGIC: Update position BEFORE the gatekeeper
    if (m_connectors_editing) {
        Vec3d pos;
        Vec3d pos_world;
        if (unproject_on_cut_plane(data.mouse_pos.cast<double>(), pos, pos_world)) {
            m_localized_cut_pos = pos_world;
        }
    }
    if (m_hover_id < 0)
        return;
    m_is_dragging = true;

    if (m_hover_id <= 2) { // drag rotate
        GLGizmoRotate3D::on_update(data);
        Vec3d rotation{ 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 3; i++) {
            if (m_gizmos[i].is_dragging())
            {
                rotation(i) = m_gizmos[i].get_angle();
                if (rotation(i) < 0) rotation(i) = 2 * PI + rotation(i);
                m_rotate_angle = rotation(i);
                break;
            }
        }
        const Transform3d rotation_tmp = m_start_dragging_m * Geometry::rotation_transform(rotation);
        // deal rotate
        if (!is_approx(rotation, Vec3d(0, 0, 0))) {
            // Orio : avec une origine choisie, le plan tourne autour d'elle (pivot) : sa
            // distance à l'origine, donc sa « Position », ne change pas
            if (m_origin_type == OriginType::Custom) {
                const Matrix3d relative    = rotation_tmp.linear() * m_start_dragging_m.linear().transpose();
                m_plane_center             = m_origin + relative * (m_plane_center_drag_start - m_origin);
                m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotate_matrix);
            }
            update_plate_normal_boundingbox_clipper(rotation_tmp);
        }
    } // move plane
    else if (m_hover_id == c_cube_z_move_id || m_hover_id == c_plate_move_id)
    {
        double move = calc_projection(m_drag_pos_start, data.mouse_ray, m_plane_normal);
        m_buffered_movement = m_movement = m_start_movement + move;
        update_plate_center(Axis::Z, move, true);
    } // move x
    else if (m_hover_id == c_cube_x_move_id && m_cut_mode == CutMode::cutTongueAndGroove)
    {
        double move = calc_projection(m_drag_pos_start, data.mouse_ray, m_plane_x_direction);
        m_buffered_movement = m_movement = m_start_movement + move;
        update_plate_center(Axis::X, move, true);
    } // drag existing connectors
    else if (m_connectors_editing && m_hover_id >= c_connectors_group_id)
    {
        CutConnectors &connectors = m_c->selection_info()->model_object()->cut_connectors;
        Vec3d pos;
        Vec3d pos_world;
        if (unproject_on_cut_plane(data.mouse_pos.cast<double>(), pos, pos_world)) {
            if (snap_to_section(pos_world, true)) // Orio : magnétisme des ancrages
                pos = world_to_connector_pos(pos_world);
            // Orio : un filetage ne passe pas sur une face qui a déjà le sien (il s'arrête au bord)
            const int idx = m_hover_id - c_connectors_group_id;
            if (connectors[idx].attribs.type != CutConnectorType::Thread ||
                thread_on_section(connectors, section_of(world_to_plane2d(pos_world)), idx) < 0)
                connectors[idx].pos = pos;
        }
    }
    check_and_update_connectors_state();
}

void GLGizmoAdvancedCut::on_render()
{
    if (m_state == On) {
        // This gizmo is showing the object elevated. Tell the common
        // SelectionInfo object to lie about the actual shift.
        m_c->selection_info()->set_use_shift(true);
    }
    // Orio : pendant le choix d'ancrages, la pièce est montrée seule (ni plan, ni
    // poignées, ni section) avec l'ancrage survolé et ceux déjà choisis
    if (is_picking()) {
        toggle_model_objects_visibility(true);
        update_pick_hover();
        render_pick_markers();
        return;
    }
    // check objects visibility
    toggle_model_objects_visibility();
    update_clipper();
    init_picking_models();

    // Show placed connectors
    if (m_connectors_editing) {
        render_connectors();
    }

    if (m_part_selection) {
        if (!m_connectors_editing) {
            if (m_is_dragging == false) {
                m_part_selection->part_render(nullptr, nullptr);
            }
        } else {
            m_part_selection->part_render(&m_plane_center, &m_plane_normal);
        }
    }

    if (!m_connectors_editing) {
        render_cut_plane_and_grabbers();
    }
    // render_clipper_cut for get the cut plane result
    render_clipper_cut();
    // render a cut line on screen by shift key and mouse move
    render_cut_line();
    // Orio : origine choisie (pivot)
    if (!m_connectors_editing)
        render_pick_markers();
    // Orio : connecteurs — motif en cours (transparent), ancrages et axes de la section
    if (m_connectors_editing) {
        update_pattern_preview();
        render_pattern_preview();
        render_section_markers();
    }

    // THE VISIBILITY FIX: Draw the ghost absolute last so it paints ON TOP of everything
    // (pas quand le clic choisit un point de motif : il ne posera pas de connecteur)
    if (m_connectors_editing && m_hover_id < c_connectors_group_id && !click_picks_pattern_point()) {
        render_localized_cut_shadow();
    }
}

bool GLGizmoAdvancedCut::on_mouse(const wxMouseEvent &mouse_event)
{
    // Orio : l'ancrage survolé se recalcule à chaque mouvement de la souris
    if (is_picking()) {
        if (mouse_event.Moving() || mouse_event.Dragging())
            m_parent.set_as_dirty();
        return false;
    }

    // If we are in connector mode and the mouse is hovering...
    if (m_connectors_editing && mouse_event.Moving()) {
        Vec3d pos;
        Vec3d pos_world;

        // Grab the raw X/Y from the mouse and unproject it to the 3D cut plane
        // (centre d'un motif cercle / grille : le survol suit aussi les trous)
        // (symétrie : axe ou point cherché sur toute la face, trous compris)
        const bool center_pick = m_pattern_mode == PatternMode::Circle || m_pattern_mode == PatternMode::Grid;
        if (unproject_on_cut_plane(Vec2d(mouse_event.GetX(), mouse_event.GetY()), pos, pos_world, !center_pick && !m_mirror_picking)) {
            // Orio : magnétisme des ancrages — l'ancrage de la section le plus proche (vrai
            // barycentre, point le plus éloigné des bords, coins, rayons, fractions…) attire le
            // connecteur à 10 pixels près ; ailleurs il reste libre
            if (m_mirror_picking)
                update_mirror_hover(pos_world);
            else
                snap_to_section(pos_world, true, center_pick);
            m_localized_cut_pos = pos_world;

            // Force the 3D canvas to update the frame immediately
            wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
        } else if (m_hover_anchor >= 0 || m_hover_island >= 0) {
            m_hover_anchor = -1;
            m_hover_island = -1;
            wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
        }
    }

    // Pass the event back up the chain!
    return GLGizmoRotate3D::on_mouse(mouse_event);
}

void GLGizmoAdvancedCut::render_localized_cut_shadow()
{
    ::glEnable(GL_BLEND);
    ::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ::glEnable(GL_CULL_FACE);
    ::glDisable(GL_DEPTH_TEST);
    ::glDepthMask(GL_FALSE);

    const Camera& camera = wxGetApp().plater()->get_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();

    ColorRGBA shadow_color(0.38f, 0.41f, 0.57f, 0.65f);

    // Calculate transformations
    Transform3d translate_tf = Transform3d::Identity();
    translate_tf.translate(m_localized_cut_pos);

    Transform3d rotate_tf = Transform3d::Identity();
    rotate_tf.rotate(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), m_plane_normal));

    Transform3d scale_tf = Transform3d::Identity();
    scale_tf.scale(Vec3f(m_connector_size / 2.0, m_connector_size / 2.0, m_connector_depth_ratio).cast<double>());

    const Transform3d view_model_matrix = view_matrix * translate_tf * rotate_tf * scale_tf;

    if (m_shapes.empty()) {
        init_connector_shapes();
    }

    if (!m_shapes.empty()) {
        GLModel* shape_to_render = nullptr;

        // Find the correct shape
        for (auto& pair : m_shapes) {
            if (pair.first.type == m_connector_type && (int)pair.first.shape == (int)m_connector_shape_id) {
                shape_to_render = &pair.second;
                break;
            }
        }

        if (!shape_to_render) {
            shape_to_render = &m_shapes.begin()->second;
        }

        // Render it
        render_glmodel(*shape_to_render, shadow_color.get_data(), view_model_matrix, projection_matrix);
    }

    ::glEnable(GL_DEPTH_TEST);
    ::glDepthMask(GL_TRUE);
    ::glEnable(GL_CULL_FACE);
    ::glDisable(GL_BLEND);
}

void GLGizmoAdvancedCut::on_render_for_picking()
{
    // Orio : rien à attraper (plan, poignées) pendant le choix d'ancrages
    if (is_picking())
        return;
    const Camera& camera = wxGetApp().plater()->get_picking_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();
    glsafe(::glDisable(GL_DEPTH_TEST));
    if (!m_connectors_editing) {
        std::array<float, 4> color;
        // pick plane
        {
            color = picking_color_component(2);
            render_glmodel(m_plane, color, view_matrix * Geometry::translation_transform(m_plane_center) * m_rotate_matrix, projection_matrix, true);
        }
        // pick Rotate
        GLGizmoRotate3D::on_render_for_picking();

        BoundingBoxf3 box = m_parent.get_selection().get_bounding_box();
        // pick grabber
        {
            const auto& flat_shader = wxGetApp().get_shader("flat");
            wxGetApp().bind_shader(flat_shader);
            color                     = picking_color_component(0);
            m_move_z_grabber.color[0] = color[0];
            m_move_z_grabber.color[1] = color[1];
            m_move_z_grabber.color[2] = color[2];
            m_move_z_grabber.color[3] = color[3];
            m_move_z_grabber.render_for_picking();
            if (m_cut_mode == CutMode::cutTongueAndGroove) {
                color                     = picking_color_component(1);
                m_move_x_grabber.color[0] = color[0];
                m_move_x_grabber.color[1] = color[1];
                m_move_x_grabber.color[2] = color[2];
                m_move_x_grabber.color[3] = color[3];
                m_move_x_grabber.render_for_picking();
            }
            wxGetApp().unbind_shader();
        }

    } else {
        auto inst_id = m_c->selection_info()->get_active_instance();
        if (inst_id < 0) return;

        const ModelObject *  mo              = m_c->selection_info()->model_object();
        const ModelInstance *mi              = mo->instances[inst_id];
        const Vec3d &        instance_offset = mi->get_offset();
        const double         sla_shift       = double(m_c->selection_info()->get_sla_shift());

        const CutConnectors &connectors      = mo->cut_connectors;
        const bool           looking_forward = is_looking_forward();
        for (int i = 0; i < connectors.size(); ++i) {
            CutConnector connector = connectors[i];
            Vec3d        pos       = connector.pos + instance_offset + sla_shift * Vec3d::UnitZ();
            float        height    = connector.height;

            deal_connector_pos_by_type(pos, height, connector.attribs.type, connector.attribs.style, looking_forward, m_connectors_editing, m_clp_normal);

            Transform3d translate_tf = Transform3d::Identity();
            translate_tf.translate(pos);

            Transform3d scale_tf = Transform3d::Identity();
            scale_tf.scale(Vec3f(connector.radius, connector.radius, height).cast<double>());

            const Transform3d view_model_matrix = view_matrix * translate_tf * m_rotate_matrix * scale_tf;

            std::array<float, 4> color = picking_color_component(i + c_connectors_start_id);
            render_glmodel(m_shapes[connectors[i].attribs], color, view_model_matrix, projection_matrix, true);
        }
    }
}

void GLGizmoAdvancedCut::on_render_input_window(float x, float y, float bottom_limit)
{
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(on_get_name(),
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    m_control_width = m_imgui->get_font_size() * 9.f;
    m_editing_window_width = 1.55 * m_control_width;
    if (m_connectors_editing) {
        init_connectors_input_window_data();
        render_connectors_input_window(x, y, bottom_limit);
    }
    else
        render_cut_plane_input_window(x, y, bottom_limit);

    render_input_window_warning();

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoAdvancedCut::show_tooltip_information(float x, float y)
{
    //Orio: shown by the keyboard icon right of the name of the tool, in the title bar of its window.
    std::vector<std::pair<wxString, wxString>> shortcuts;
    auto draw_text_with_caption = [&shortcuts](const wxString &caption, const wxString &text) { shortcuts.emplace_back(caption, text); };
    if (m_connectors_editing) {
        for (const auto &short_cut : m_connector_shortcuts) draw_text_with_caption(short_cut.first + ": ", short_cut.second);
    } else if (m_cut_mode == CutMode::cutPlanar) {
        for (size_t i = 0; i < m_cut_plane_shortcuts.size(); ++i)
            if (i != 1 || !several_planes()) // 1 = clic droit, inactif avec plusieurs plans
                draw_text_with_caption(m_cut_plane_shortcuts[i].first + ": ", m_cut_plane_shortcuts[i].second);
    } else if (m_cut_mode == CutMode::cutTongueAndGroove) {
        for (const auto &short_cut : m_cut_groove_shortcuts) draw_text_with_caption(short_cut.first + ": ", short_cut.second);
    }
    m_imgui->tool_window_shortcuts(shortcuts);
}

void update_object_cut_id(CutObjectBase &cut_id, ModelObjectCutAttributes attributes, const int dowels_count)
{
    // we don't save cut information, if result will not contains all parts of initial object
    if (!attributes.has(ModelObjectCutAttribute::KeepUpper) || !attributes.has(ModelObjectCutAttribute::KeepLower) || attributes.has(ModelObjectCutAttribute::InvalidateCutInfo))
        return;

    if (cut_id.id().invalid()) cut_id.init();
    // increase check sum, if it's needed
    {
        int cut_obj_cnt = -1;
        if (attributes.has(ModelObjectCutAttribute::KeepUpper)) cut_obj_cnt++;
        if (attributes.has(ModelObjectCutAttribute::KeepLower)) cut_obj_cnt++;
        if (attributes.has(ModelObjectCutAttribute::CreateDowels)) cut_obj_cnt += dowels_count;
        if (cut_obj_cnt > 0) cut_id.increase_check_sum(size_t(cut_obj_cnt));
    }
}

static void check_objects_after_cut(const ModelObjectPtrs &objects)
{
    std::vector<std::string> err_objects_names;
    std::vector<int>         err_objects_idxs;
    int                      obj_idx{0};
    for (const ModelObject *object : objects) {
        std::vector<std::string> connectors_names;
        connectors_names.reserve(object->volumes.size());
        for (const ModelVolume *vol : object->volumes)
            if (vol->cut_info.is_connector) connectors_names.push_back(vol->name);
        const size_t connectors_count = connectors_names.size();
        sort_remove_duplicates(connectors_names);
        if (connectors_count != connectors_names.size()) err_objects_names.push_back(object->name);

        // check manifol/repairs
        auto stats = object->get_object_stl_stats();
        if (!stats.manifold() || stats.has_open_edges() || stats.repaired()) err_objects_idxs.push_back(obj_idx);
        obj_idx++;
    }

    auto plater = wxGetApp().plater();
    if (!err_objects_names.empty()) {
        wxString names = from_u8(err_objects_names[0]);
        for (size_t i = 1; i < err_objects_names.size(); i++) names += ", " + from_u8(err_objects_names[i]);
        WarningDialog(plater, format_wxstr("Objects(%1%) have duplicated connectors. "
                                           "Some connectors may be missing in slicing result.\n"
                                           "Please report to BambuSudio team in which scenario this issue happened.\n"
                                           "Thank you.",
                                           names))
            .ShowModal();
    }
}

    void synchronize_model_after_cut(Model &model, const CutObjectBase &cut_id)
{
    for (ModelObject *obj : model.objects)
        if (obj->is_cut() && obj->cut_id.has_same_id(cut_id) && !obj->cut_id.is_equal(cut_id)) obj->cut_id.copy(cut_id);
}

void GLGizmoAdvancedCut::perform_cut(const Selection& selection)
{
    if (!can_perform_cut()) return;

    const int instance_idx = selection.get_instance_idx();
    const int object_idx   = selection.get_object_idx();

    wxCHECK_RET(instance_idx >= 0 && object_idx >= 0, "GLGizmoAdvancedCut: Invalid object selection");

    Plater *     plater = wxGetApp().plater();
    ModelObject *mo     = plater->model().objects[object_idx];
    if (!mo) return;
    // Orio : plusieurs plans, lus avant de fermer l'outil
    store_active_plane();
    const PlaneList list      = m_multi;
    const bool      by_planes = !list.planes.empty();
    const bool      drops     = by_planes && dropped_pieces() > 0; // des morceaux décochés partent
    // deactivate CutGizmo and than perform a cut
    m_parent.reset_all_gizmos();
    // m_cut_z is the distance from the bed. Subtract possible SLA elevation.
    // const GLVolume* first_glvolume = selection.get_volume(*selection.get_volume_idxs().begin());

    // perform cut
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Cut by Plane");

        // This shall delete the part selection class and deallocate the memory.
        ScopeGuard part_selection_killer([this]() {
            m_part_selection.reset(new PartSelection());
        });

        const bool cut_with_groove = m_cut_mode == CutMode::cutTongueAndGroove;
        bool       cut_by_contour  = !cut_with_groove && m_part_selection->valid() && m_part_selection->has_modified_cut_parts();

        ModelObject *cut_mo = cut_by_contour ? m_part_selection->model_object() : nullptr;
        if (cut_mo)
            cut_mo->cut_connectors = mo->cut_connectors;
        else
            cut_mo = mo;

        int  dowels_count   = 0;
        bool has_connectors = !mo->cut_connectors.empty();
        for (const SavedPlane &plane : list.planes) // Orio : ceux des autres plans comptent aussi
            has_connectors = has_connectors || !plane.connectors.empty();
        // update connectors pos as offset of its center before cut performing
        if (!by_planes) // Orio : avec plusieurs plans, cut_by_planes pose ceux de chaque plan sur les morceaux
            apply_connectors_in_model(cut_mo, dowels_count, m_rotate_matrix, m_plane_normal);

        wxBusyCursor wait;

        ModelObjectCutAttributes attributes = only_if(has_connectors ? true : m_keep_upper, ModelObjectCutAttribute::KeepUpper) |
                                              only_if(has_connectors ? true : m_keep_lower, ModelObjectCutAttribute::KeepLower) |
                                              only_if(has_connectors ? false : m_cut_to_parts, ModelObjectCutAttribute::CutToParts) |
                                              only_if(m_place_on_cut_upper, ModelObjectCutAttribute::PlaceOnCutUpper) |
                                              only_if(m_place_on_cut_lower, ModelObjectCutAttribute::PlaceOnCutLower) |
                                              only_if(m_rotate_upper, ModelObjectCutAttribute::FlipUpper) | only_if(m_rotate_lower, ModelObjectCutAttribute::FlipLower) |
                                              only_if(dowels_count > 0, ModelObjectCutAttribute::CreateDowels) |
                                              only_if(!has_connectors && !cut_with_groove && cut_mo->cut_id.id().invalid(), ModelObjectCutAttribute::InvalidateCutInfo);

        // update cut_id for the cut object in respect to the attributes
        if (!drops) // Orio : comme avec un seul plan, pas d'infos de coupe si des morceaux partent
            update_object_cut_id(cut_mo->cut_id, attributes, dowels_count);

        Cut cut(by_planes ? nullptr : cut_mo, instance_idx, get_cut_matrix(selection), attributes);
        cut.set_offset_for_two_part        = true;
        Model pieces; // Orio : résultat avec plusieurs plans (doit survivre à new_objects)
        if (by_planes)
            cut_by_planes(*cut_mo, list, instance_idx, selection, pieces);
        const ModelObjectPtrs &new_objects = by_planes       ? pieces.objects :
                                             cut_by_contour  ? cut.perform_by_contour(m_part_selection->get_cut_parts(), dowels_count) :
                                             cut_with_groove ? cut.perform_with_groove(m_groove, m_rotate_matrix) :
                                                               cut.perform_with_plane();
        check_objects_after_cut(new_objects);// Fix for #11487 - Cut Connectors Broken when assigning part to other side
        // fix_non_manifold_edges
#ifdef HAS_WIN10SDK
        if (is_windows10()) {
            bool is_showed_dialog = false;
            bool user_fix_model   = false;
            bool  all_break       = false;
            Plater::TakeSnapshot snapshot(plater, "RepairingModelObjectInCut");
            for (size_t i = 0; i < new_objects.size(); i++) {
                for (size_t j = 0; j < new_objects[i]->volumes.size(); j++) {
                    if (its_num_open_edges(new_objects[i]->volumes[j]->mesh().its) > 0) {
                        if (!is_showed_dialog) {
                            is_showed_dialog = true;
                            MessageDialog dlg(nullptr, _L("Open edges may be caused by the cut tool, do you want to fix it now?"), "", wxYES | wxNO);
                            int           ret = dlg.ShowModal();
                            if (ret == wxID_YES) {
                                user_fix_model = true;
                            }
                        }
                        if (!user_fix_model) {
                            break;
                        }
                        // model_name
                        std::vector<std::string> succes_models;
                        // model_name     failing reason
                        std::vector<std::pair<std::string, std::string>> failed_models;
                        auto                                             plater = wxGetApp().plater();
                        auto fix_and_update_progress = [this, plater](ModelObject *model_object, const int vol_idx, const string &model_name, ProgressDialog &progress_dlg,
                                                                      std::vector<std::string> &succes_models, std::vector<std::pair<std::string, std::string>> &failed_models) {
                            wxString msg = _L("Repairing model object");
                            msg += ": " + from_u8(model_name) + "\n";
                            std::string res;
                            if (!fix_model_by_win10_sdk_gui(*model_object, vol_idx, progress_dlg, msg, res))
                                return false;
                            return true;
                        };
                        ProgressDialog progress_dlg(_L("Repairing model object"), "", 100, find_toplevel_parent(plater), wxPD_AUTO_HIDE | wxPD_APP_MODAL | wxPD_CAN_ABORT, true);

                        auto model_name = new_objects[i]->name;
                        if (!fix_and_update_progress(new_objects[i], j, model_name, progress_dlg, succes_models, failed_models)) {
                            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "run fix_and_update_progress error";
                            plater->take_snapshot("RepairingModelObjectInCut");
                            all_break = true;
                            break;
                        };
                    };
                }
                if (all_break) {
                    break;
                }
            }
        }
 #endif
        // set offset for new_objects

        // save cut_id to post update synchronization
        const CutObjectBase cut_id = cut_mo->cut_id;

        // update cut results on plater and in the model
        plater->apply_cut_object_to_model(object_idx, new_objects);

        synchronize_model_after_cut(plater->model(), cut_id);
    }
}

bool GLGizmoAdvancedCut::can_perform_cut() const
{
    if (!m_invalid_connectors_idxs.empty() || (!m_keep_upper && !m_keep_lower) || m_connectors_editing || all_pieces_dropped()) return false;

    if (m_cut_mode == CutMode::cutTongueAndGroove) return has_valid_groove();

    // Orio : plan posé sur une extrémité de la pièce (position 0 ou maximale) : rien à couper
    for (int i = 0; i < std::max(1, int(m_multi.planes.size())); ++i)
        if (!plane_crosses_piece(i))
            return false;

    if (m_part_selection && m_part_selection->valid()) return !m_part_selection->is_one_object();

    return true;
}

// Orio : rotation et normale du plan qui porte les connecteurs (plusieurs plans : pas forcément l'actif)
void GLGizmoAdvancedCut::apply_connectors_in_model(ModelObject *mo, int &dowels_count, const Transform3d &rotation, const Vec3d &normal) {
    if (m_cut_mode == CutMode::cutTongueAndGroove) return;
    {
        clear_selection();

        for (CutConnector &connector : mo->cut_connectors) {
            connector.rotation_m = rotation; // m_rotation_m

            if (connector.attribs.type == CutConnectorType::Dowel) {
                if (connector.attribs.style == CutConnectorStyle::Prizm) connector.height *= 2;
                dowels_count++;
            } else {
                // calculate shift of the connector center regarding to the position on the cut plane
                connector.pos += normal * 0.5 * double(connector.height);
            }
        }
        mo->apply_cut_connectors(_u8L("Connector"));
    }
}

bool GLGizmoAdvancedCut::is_selection_changed(bool alt_down, bool shift_down)
{
    if (m_hover_id >= c_connectors_group_id) {
        if (alt_down)
            select_connector(m_hover_id - c_connectors_group_id, false);
        else {
            if (!shift_down) unselect_all_connectors();
            select_connector(m_hover_id - c_connectors_group_id, true);
        }
        return true;
    }
    return false;
}

void GLGizmoAdvancedCut::select_connector(int idx, bool select)
{
    m_selected[idx] = select;
    if (select)
        ++m_selected_count;
    else
        --m_selected_count;
}

double GLGizmoAdvancedCut::calc_projection(const Vec3d &drag_pos, const Linef3 &mouse_ray, const Vec3d &project_dir) const
{
    Vec3d mouse_dir  = mouse_ray.unit_vector();
    Vec3d inters     = mouse_ray.a + (drag_pos - mouse_ray.a).dot(mouse_dir) / mouse_dir.squaredNorm() * mouse_dir;
    Vec3d inters_vec = inters - drag_pos;
    return inters_vec.dot(project_dir);
}

Vec3d GLGizmoAdvancedCut::get_plane_normal() const {
    return m_plane_normal;
}

Vec3d GLGizmoAdvancedCut::get_plane_center() const {
    return m_plane_center;
}

void GLGizmoAdvancedCut::reset_rotation()
{
    for (int i = 0; i < 3; i++) {
        m_gizmos[i].set_angle(0.);
    }
}

void GLGizmoAdvancedCut::put_connectors_on_cut_plane(const Vec3d &cp_normal, double cp_offset)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (CutConnectors &connectors = mo->cut_connectors; !connectors.empty()) {
        const float  sla_shift       = m_c->selection_info()->get_sla_shift();
        const Vec3d &instance_offset = mo->instances[m_c->selection_info()->get_active_instance()]->get_offset();

        for (auto &connector : connectors) {
            // convert connetor pos to the world coordinates
            Vec3d pos = connector.pos + instance_offset;
            pos[Z] += sla_shift;
            // scalar distance from point to plane along the normal
            double distance = -cp_normal.dot(pos) + cp_offset;
            // move connector
            connector.pos += distance * cp_normal;
        }
    }
}

void GLGizmoAdvancedCut::update_plane_normal()
{
    // update cut_normal
    Vec3d normal        = m_rotate_matrix * Vec3d::UnitZ();
    m_plane_normal      = normal;                           // core
    m_plane_x_direction = m_rotate_matrix * Vec3d::UnitX(); // core
    m_clp_normal        = normal;
}

void GLGizmoAdvancedCut::update_clipper()
{
    update_plane_normal();
    auto normal = m_plane_normal;
    // calculate normal and offset for clipping plane
    Vec3d beg = m_bb_center;
    beg[Z] -= m_radius;
    rotate_vec3d_around_plane_center(beg, m_rotate_matrix, m_plane_center);

    double offset = normal.dot(m_plane_center);
    double dist   = normal.dot(beg);

    m_parent.set_color_clip_plane(normal, offset);
    m_parent.set_color_clip_sections(color_clip_sections()); // Orio : sans effet avec un seul plan
    if (!is_looking_forward()) {
        // recalculate normal and offset for clipping plane, if camera is looking downward to cut plane
        normal = m_rotate_matrix * (-1. * Vec3d::UnitZ());
        normal.normalize();

        beg = m_bb_center;
        beg[Z] += m_radius;
        rotate_vec3d_around_plane_center(beg, m_rotate_matrix, m_plane_center);

        m_clp_normal = normal;
        offset       = normal.dot(m_plane_center);
        dist         = normal.dot(beg);
    }

    if (m_c->object_clipper()) {
        m_c->object_clipper()->set_range_and_pos(normal, offset, dist);
        put_connectors_on_cut_plane(normal, offset);
    }
}

void GLGizmoAdvancedCut::render_cut_plane_and_grabbers()
{
    // plane points is in object coordinate
    // draw plane
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    // Orio : avec plusieurs plans, chacun en pâle de la couleur de la section où pointe sa flèche
    const GLVolumeCollection::ColorClipSections sections = color_clip_sections();
    auto plane_color = [&sections](bool valid, const Vec3d &center, const Transform3d &rotation) -> ColorRGBA {
        if (!valid || sections.planes.size() < 2)
            return valid ? CUT_PLANE_DEF_COLOR : CUT_PLANE_ERR_COLOR;
        ColorRGBA color = lerp(section_color(sections, center + 0.1 * (rotation * Vec3d::UnitZ()).normalized()), ColorRGBA::WHITE(), 0.5f);
        color.a(CUT_PLANE_DEF_COLOR.a());
        return color;
    };
    bool      is_valid = (several_planes() ? plane_crosses_piece(m_multi.active) : can_perform_cut()) && has_valid_groove();
    ColorRGBA cp_clr   = plane_color(is_valid, m_plane_center, m_rotate_matrix);
    if (m_cut_mode == CutMode::cutTongueAndGroove) {
        cp_clr.a(cp_clr.a() - 0.1f);
    }
    const Camera& camera = wxGetApp().plater()->get_camera();

    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();
    // Orio : les plans en attente, translucides (sans écrire la profondeur : ils ne se masquent pas entre eux)
    glsafe(::glDepthMask(GL_FALSE));
    for (int i = 0; i < int(m_multi.planes.size()); ++i) {
        if (i == m_multi.active)
            continue;
        const SavedPlane &plane = m_multi.planes[i];
        ColorRGBA         color = plane_color(plane_crosses_piece(i), plane.center, plane.rotation);
        color.a(0.25f);
        render_glmodel(m_plane, color.get_data(), view_matrix * Geometry::translation_transform(plane.center) * plane.rotation, projection_matrix);
    }
    glsafe(::glDepthMask(GL_TRUE));
    render_glmodel(m_plane, cp_clr.get_data(), view_matrix * Geometry::translation_transform(m_plane_center) * m_rotate_matrix, projection_matrix);

    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));

    // Draw the grabber and the connecting line
    float radius = Offset;
    modify_radius(radius);
    radius *= 0.5f;
    m_move_z_grabber.center  = m_plane_center + m_plane_normal * radius;
    bool is_render_z_grabber = true;                                      // m_hover_id < 0 || m_hover_id == cube_z_move_id;
    bool is_render_x_grabber = m_cut_mode == CutMode::cutTongueAndGroove; // m_hover_id < 0 || m_hover_id == cube_x_move_id;
    glsafe(::glDisable(GL_DEPTH_TEST));

    if (!m_grabber_model.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(Vec3f(-0.5f, 0.0f, 0.0f));
        init_data.add_vertex(Vec3f(0.5f,  0.0f, 0.0f));

        // indices
        init_data.add_line(0, 1);

        m_grabber_model.init_from(std::move(init_data));
    }
    m_grabber_model.set_color({ 1.0f, 1.0f, 0.0f, 1.0f });

    auto calculate_model_matrix = [](const Vec3d& source, const Vec3d& target, Transform3d& model_matrix)->void {
        Vec3d the_vector = target - source;
        const auto scale = the_vector.stableNorm();
        const Vec3d center = source + 0.5f * scale * the_vector.normalized();

        Vec3d rotation_axis;
        double rotation_angle;
        Matrix3d rotation_matrix;
        Geometry::rotation_from_two_vectors(Vec3d(1.0f, 0.0f, 0.0f), the_vector, rotation_axis, rotation_angle, &rotation_matrix);
        Matrix4d final_rotation_matrix{ Matrix4d::Identity() };
        for (int iColumn = 0; iColumn < 3; ++iColumn) {
            for (int jRow = 0; jRow < 3; ++jRow) {
                final_rotation_matrix.data()[4 * iColumn + jRow] = rotation_matrix.data()[3 * iColumn + jRow];
            }
        }

        model_matrix = Geometry::translation_transform(center.cast<double>()).matrix()
            * final_rotation_matrix
            * Geometry::scale_transform({ scale, scale, scale }).matrix();
    };

    const float fullsize = get_grabber_size();
    const auto& p_flat_shader = wxGetApp().get_shader("flat");
    if (p_flat_shader && (is_render_z_grabber || is_render_x_grabber)) {
        wxGetApp().bind_shader(p_flat_shader);

        const auto& view_matrix = camera.get_view_matrix();
        const auto& proj_matrix = camera.get_projection_matrix();

        p_flat_shader->set_uniform("projection_matrix", proj_matrix);

        const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
        p_ogl_manager->set_line_width(m_hover_id != -1 ? 2.0f : 1.5f);

        // to do: remove deprecated api: glLineStipple
#ifdef __APPLE__
        const auto& gl_info = p_ogl_manager->get_gl_info();
        const auto formated_gl_version = gl_info.get_formated_gl_version();
        if (formated_gl_version < 30)
#endif
        {
            glLineStipple(1, 0x0FFF);
            glEnable(GL_LINE_STIPPLE);
        }

        if (is_render_z_grabber) {

            Transform3d model_matrix{ Transform3d::Identity() };
            calculate_model_matrix(m_plane_center, m_move_z_grabber.center, model_matrix);
            p_flat_shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
            m_grabber_model.render_geometry();
        }

        m_move_x_grabber.center = m_plane_center + m_plane_x_direction * radius;
        if (is_render_x_grabber) {

            Transform3d model_matrix{ Transform3d::Identity() };
            calculate_model_matrix(m_plane_center, m_move_x_grabber.center, model_matrix);
            p_flat_shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
            m_grabber_model.render_geometry();
        }
#ifdef __APPLE__
        if (formated_gl_version < 30)
#endif
        {
            glDisable(GL_LINE_STIPPLE);
        }
    }

    // BBS set to fixed size grabber
    // float fullsize = 2 * (dragging ? get_dragging_half_size(size) : get_half_size(size));
    GLModel &cube_z = m_move_z_grabber.get_cube();

    const auto get_render_color = [this](unsigned int id)->std::array<float, 4> {
        bool                 hover = (m_hover_id == id);
        std::array<float, 4> render_color;
        if (hover) {
            render_color = GrabberHoverColor;
        }
        else
            render_color = GrabberColor;
        return render_color;
    };

    if (is_render_z_grabber) {
        Vec3d t_z_dir = m_move_z_grabber.center - m_plane_center;
        const auto scale_z = t_z_dir.stableNorm();
        const Vec3d target_z = m_plane_center + scale_z * t_z_dir.normalized();
        Transform3d cube_mat_z = Geometry::translation_transform(target_z) * m_rotate_matrix * Geometry::scale_transform(fullsize); //
        m_move_z_grabber.m_matrix = cube_mat_z;
        std::array<float, 4> render_color = get_render_color(c_cube_z_move_id);
        render_glmodel(cube_z, render_color, view_matrix * m_move_z_grabber.m_matrix, projection_matrix);
    }

    if (is_render_x_grabber) {
        GLModel& cube_x = m_move_x_grabber.get_cube();
        Vec3d t_x_dir = m_move_x_grabber.center - m_plane_center;
        const auto scale_x = t_x_dir.stableNorm();
        const Vec3d target_x = m_plane_center + scale_x * t_x_dir.normalized();
        Transform3d cube_mat_x = Geometry::translation_transform(target_x) * m_rotate_matrix * Geometry::scale_transform(fullsize); //
        m_move_x_grabber.m_matrix = cube_mat_x;

        std::array<float, 4> render_color = get_render_color(c_cube_x_move_id);
        render_glmodel(cube_x, render_color, view_matrix * m_move_x_grabber.m_matrix, projection_matrix);
    }
    // Should be placed at last, because GLGizmoRotate3D clears depth buffer
    // Orio : anneaux de rotation centrés sur le pivot (origine choisie, sinon centre du plan)
    GLGizmoRotate3D::set_center(rotation_pivot());
    on_render_rotate_gizmos();//replace GLGizmoRotate3D::on_render();
}

void GLGizmoAdvancedCut::on_render_rotate_gizmos() {

    if (m_is_dragging) {
        if (m_hover_id == 0)
            m_gizmos[X].render();
        if ( m_hover_id == 1)
            m_gizmos[Y].render();
        if (m_hover_id == 2)
            m_gizmos[Z].render();
    }
    else {
        Transform3d rotate_tran = Geometry::translation_transform(rotation_pivot()) * m_rotate_matrix;
        for (int i = 0; i < 3; i++) {
            m_gizmos[i].set_custom_tran(rotate_tran);
        }
        m_gizmos[X].render();
        m_gizmos[Y].render();
        m_gizmos[Z].render();
    }
}

void GLGizmoAdvancedCut::render_connectors()
{
    ::glEnable(GL_DEPTH_TEST);
    if (cut_line_processing() || m_cut_mode != CutMode::cutPlanar || !m_c->selection_info())
        return;
    const ModelObject *mo      = m_c->selection_info()->model_object();
    auto               inst_id = m_c->selection_info()->get_active_instance();
    if (inst_id < 0) return;

    const CutConnectors &connectors = mo->cut_connectors;
    if (connectors.size() != m_selected.size()) {
        clear_selection();
        m_selected.resize(connectors.size(), false);
    }

    const ModelInstance *mi              = mo->instances[inst_id];
    const Vec3d &        instance_offset = mi->get_offset();
    const double         sla_shift       = double(m_c->selection_info()->get_sla_shift());

    ColorRGBA  render_color    = CONNECTOR_DEF_COLOR;
    const bool looking_forward = is_looking_forward();

    const Camera& camera = wxGetApp().plater()->get_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();

    for (size_t i = 0; i < connectors.size(); ++i) {
        const CutConnector &connector = connectors[i];

        float height = connector.height;
        // recalculate connector position to world position
        Vec3d pos = connector.pos + instance_offset + sla_shift * Vec3d::UnitZ();

        // First decide about the color of the point.
        assert(std::is_sorted(m_invalid_connectors_idxs.begin(), m_invalid_connectors_idxs.end()));
        const bool conflict_connector = std::binary_search(m_invalid_connectors_idxs.begin(), m_invalid_connectors_idxs.end(), i);
        if (conflict_connector) {
            render_color = CONNECTOR_ERR_COLOR;
        } else // default connector color
            render_color = connector.attribs.type == CutConnectorType::Dowel ? DOWEL_COLOR : PLAG_COLOR;

        if (!m_connectors_editing)
            render_color = CONNECTOR_ERR_COLOR;
        else if (size_t(m_hover_id - c_connectors_group_id) == i)
            render_color = conflict_connector ? HOVERED_ERR_COLOR : connector.attribs.type == CutConnectorType::Dowel ? HOVERED_DOWEL_COLOR : HOVERED_PLAG_COLOR;
        else if (m_selected[i])
            render_color = connector.attribs.type == CutConnectorType::Dowel ? SELECTED_DOWEL_COLOR : SELECTED_PLAG_COLOR;

        deal_connector_pos_by_type(pos, height, connector.attribs.type, connector.attribs.style, looking_forward, m_connectors_editing, m_clp_normal); // BBL

        Transform3d translate_tf = Transform3d::Identity();
        translate_tf.translate(pos);

        Transform3d scale_tf = Transform3d::Identity();
        scale_tf.scale(Vec3f(connector.radius, connector.radius, height).cast<double>());

        const Transform3d view_model_matrix = view_matrix * translate_tf * m_rotate_matrix * scale_tf;

        render_glmodel(m_shapes[connector.attribs], render_color.get_data(), view_model_matrix, projection_matrix);
    }
}

void GLGizmoAdvancedCut::render_clipper_cut()
{
    if (!m_connectors_editing)
        ::glDisable(GL_DEPTH_TEST);

    GLboolean cull_face = GL_FALSE;
    ::glGetBooleanv(GL_CULL_FACE, &cull_face);
    ::glDisable(GL_CULL_FACE);
    if (m_part_selection) {
        m_c->object_clipper()->render_cut(m_part_selection->get_ignored_contours_ptr());
    }

    if (cull_face)
        ::glEnable(GL_CULL_FACE);

    if (!m_connectors_editing)
        ::glEnable(GL_DEPTH_TEST);
}

void GLGizmoAdvancedCut::render_cut_line()
{
    if (!cut_line_processing() || m_cut_line_end.isApprox(Vec3d::Zero()))
        return;
    const auto& p_flat_shader = wxGetApp().get_shader("flat");
    if (!p_flat_shader) {
        return;
    }

    if (!m_cut_line_model.is_initialized()) {
        GLModel::Geometry geo;
        geo.format.type = GLModel::PrimitiveType::Lines;
        geo.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3;

        geo.add_vertex(Vec3f{ -0.5f, 0.0f, 0.0f });
        geo.add_vertex(Vec3f{ 0.5f, 0.0f, 0.0f });

        geo.add_line(0, 1);

        m_cut_line_model.init_from(std::move(geo));
    }

    Transform3d model_matrix{ Transform3d::Identity() };
    Vec3d the_vector = m_cut_line_end - m_cut_line_begin;
    const auto scale = the_vector.stableNorm();
    Vec3d center = (m_cut_line_end + m_cut_line_begin) * 0.5f;

    Vec3d rotation_axis;
    double rotation_angle;
    Matrix3d rotation_matrix;
    Geometry::rotation_from_two_vectors(Vec3d(1.0f, 0.0f, 0.0f), the_vector, rotation_axis, rotation_angle, &rotation_matrix);
    Matrix4d final_rotation_matrix{ Matrix4d::Identity() };
    for (int iColumn = 0; iColumn < 3; ++iColumn) {
        for (int jRow = 0; jRow < 3; ++jRow) {
            final_rotation_matrix.data()[4 * iColumn + jRow] = rotation_matrix.data()[3 * iColumn + jRow];
        }
    }

    model_matrix = Geometry::translation_transform(center.cast<double>()).matrix()
        * final_rotation_matrix
        * Geometry::scale_transform({ scale, scale, scale }).matrix();

    m_cut_line_model.set_color({ 0.0f, 1.0f, 0.0f, 1.0f});

#ifdef __APPLE__
    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    const auto& gl_info = p_ogl_manager->get_gl_info();
    const auto formated_gl_version = gl_info.get_formated_gl_version();
    if (formated_gl_version < 30)
#endif
    {
        glsafe(::glEnable(GL_LINE_STIPPLE));
    }

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    wxGetApp().bind_shader(p_flat_shader);

    const Camera& camera = wxGetApp().plater()->get_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& proj_matrix = camera.get_projection_matrix();
    p_flat_shader->set_uniform("projection_matrix", proj_matrix);
    p_flat_shader->set_uniform("view_model_matrix", view_matrix * model_matrix);

    m_cut_line_model.render_geometry();

    wxGetApp().unbind_shader();

#ifdef __APPLE__
    if (formated_gl_version < 30)
#endif
    {
        glsafe(::glDisable(GL_LINE_STIPPLE));
    }
}

void GLGizmoAdvancedCut::clear_selection()
{
    m_selected.clear();
    m_selected_count = 0;
}

void GLGizmoAdvancedCut::init_connector_shapes()
{
    // --- ADDED OUR THREAD TO THE CACHE GENERATOR ---
    for (const CutConnectorType &type : {CutConnectorType::Snap, CutConnectorType::Dowel, CutConnectorType::Plug, CutConnectorType::Thread})
        for (const CutConnectorStyle &style : {CutConnectorStyle::Frustum, CutConnectorStyle::Prizm})
            for (const CutConnectorShape &shape : {CutConnectorShape::Circle, CutConnectorShape::Hexagon, CutConnectorShape::Square, CutConnectorShape::Triangle}) {
                CutConnectorAttributes     attribs = {type, style, shape};
                CutConnectorParas          paras   = {m_snap_space_proportion, m_snap_bulge_proportion};
                const indexed_triangle_set its     = ModelObject::get_connector_mesh(attribs, paras);
                m_shapes[attribs].init_from(its);
            }
}

void GLGizmoAdvancedCut::set_connectors_editing(bool connectors_editing)
{
    if (m_connectors_editing == connectors_editing)
        return;

    m_connectors_editing = connectors_editing;
    if (connectors_editing)
        m_connectors_before_editing = m_c->selection_info()->model_object()->cut_connectors;
    // Orio : motif abandonné, sections recalculées à la prochaine image
    m_pattern_mode = PatternMode::None;
    m_pattern_line.clear();
    m_pattern_preview.clear();
    m_pattern_center_set = false;
    m_mirror_picking     = false;
    m_mirror_has_first   = false;
    m_sections_key.clear();
    m_hover_anchor = -1;
    m_hover_island = -1;
    m_c->object_clipper()->set_behaviour(m_connectors_editing, m_connectors_editing, double(m_contour_width));
    m_parent.request_extra_frame();
    // todo: zhimin need a better method
    // after change render mode, need update for scene
    on_render();
}

void GLGizmoAdvancedCut::reset_connectors()
{
    m_c->selection_info()->model_object()->cut_connectors.clear();
    clear_selection();
    check_and_update_connectors_state();
}

void GLGizmoAdvancedCut::update_connector_shape()//update mesh
{
    CutConnectorAttributes attribs = { m_connector_type, CutConnectorStyle(m_connector_style), CutConnectorShape(m_connector_shape_id)};
    CutConnectorParas       paras  = {m_snap_space_proportion, m_snap_bulge_proportion};
    if (m_connector_type == CutConnectorType::Snap) {
        indexed_triangle_set its = ModelObject::get_connector_mesh(attribs, paras);
        m_shapes[attribs].reset();
        m_shapes[attribs].init_from(its);
    }
}

void GLGizmoAdvancedCut::apply_selected_connectors(std::function<void(size_t idx)> apply_fn)
{
    for (size_t idx = 0; idx < m_selected.size(); idx++)
        if (m_selected[idx])
            apply_fn(idx);
    check_and_update_connectors_state();
}

void GLGizmoAdvancedCut::select_all_connectors()
{
    std::fill(m_selected.begin(), m_selected.end(), true);
    m_selected_count = int(m_selected.size());
}

void GLGizmoAdvancedCut::unselect_all_connectors()
{
    std::fill(m_selected.begin(), m_selected.end(), false);
    m_selected_count = 0;
    validate_connector_settings();
}

void GLGizmoAdvancedCut::validate_connector_settings()
{
    if (m_connector_depth_ratio < 0.f)
        m_connector_depth_ratio = 3.f;
    // Orio : tolérances par défaut 0,15 mm (CUT_CONNECTOR_TOLERANCE)
    if (m_connector_depth_ratio_tolerance < 0.f)
        m_connector_depth_ratio_tolerance = CUT_CONNECTOR_TOLERANCE;
    if (m_connector_size < 0.f)
        m_connector_size = 2.5f;
    if (m_connector_size_tolerance < 0.f)
        m_connector_size_tolerance = CUT_CONNECTOR_TOLERANCE;

    if (m_connector_type == CutConnectorType::Undef)
        m_connector_type = CutConnectorType::Plug;
    if (m_connector_style == size_t(CutConnectorStyle::Undef))
        m_connector_style = size_t(CutConnectorStyle::Prizm);
    if (m_connector_shape_id == size_t(CutConnectorShape::Undef))
        m_connector_shape_id = size_t(CutConnectorShape::Circle);
}

bool GLGizmoAdvancedCut::add_connector(CutConnectors &connectors, const Vec2d &mouse_position)
{
    m_add_connector_ok = false;
    if (!m_connectors_editing)
        return false;

    Vec3d pos;
    Vec3d pos_world;
    if (unproject_on_cut_plane(mouse_position.cast<double>(), pos, pos_world)) {
        // Orio : magnétisme des ancrages (ancrage le plus proche, sinon le point cliqué).
        // L'ancien « Milieu de la géométrie » visait le centre du plan, pas celui de la section, et
        // posait le connecteur dans le repère du plan au lieu de celui de l'objet.
        if (snap_to_section(pos_world, false))
            pos = world_to_connector_pos(pos_world);

        // un filetage se visse en tournant : un seul par face coupée. S'il y en a déjà un sur la face
        // cliquée, il vient ici, avec les réglages du panneau, au lieu d'en ajouter un second ; sur une
        // autre face (pièce en Y : deux faces), un nouveau filetage est posé.
        const bool thread     = m_connector_type == CutConnectorType::Thread;
        const int  thread_idx = thread ? thread_on_section(connectors, section_of(world_to_plane2d(pos_world))) : -1;

        Plater::TakeSnapshot snapshot(wxGetApp().plater(), thread_idx >= 0 ? "Move connector" : "Add connector");
        unselect_all_connectors();

        // filetage : diamètre et longueur minimale (2 pas) donnés par la désignation ISO
        const float radius = thread ? float(0.5 * ISO_METRIC_THREADS[m_thread_designation].size) : m_connector_size * 0.5f;
        const float height = thread ? std::max(m_connector_depth_ratio, float(2. * ISO_METRIC_THREADS[m_thread_designation].pitch)) : m_connector_depth_ratio;
        if (thread_idx >= 0) {
            CutConnector &connector    = connectors[thread_idx];
            connector.pos              = pos;
            connector.rotation_m       = m_rotate_matrix;
            connector.radius           = radius;
            connector.height           = height;
            connector.radius_tolerance = m_connector_size_tolerance;
            connector.height_tolerance = m_connector_depth_ratio_tolerance;
            connector.paras            = current_connector_paras();
            if (m_selected.size() != connectors.size())
                m_selected.assign(connectors.size(), false);
            m_selected[thread_idx] = true;
            m_selected_count       = 1;
            m_parent.set_as_dirty();
            check_and_update_connectors_state();
            m_add_connector_ok = true;
            return true;
        }
        connectors.emplace_back(pos, m_rotate_matrix, radius, height, m_connector_size_tolerance, m_connector_depth_ratio_tolerance,
                                CutConnectorAttributes(CutConnectorType(m_connector_type), CutConnectorStyle(m_connector_style), CutConnectorShape(m_connector_shape_id)));
        connectors.back().paras = current_connector_paras();
        m_selected.push_back(true);
        m_selected_count = 1;
        assert(m_selected.size() == connectors.size());
        m_parent.set_as_dirty();
        check_and_update_connectors_state();
        m_add_connector_ok = true;
        return true;
    }
    return false;
}

bool GLGizmoAdvancedCut::delete_selected_connectors()
{
    CutConnectors &connectors = m_c->selection_info()->model_object()->cut_connectors;
    if (connectors.empty())
        return false;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Delete connector");

    // remove  connectors
    for (int i = int(connectors.size()) - 1; i >= 0; i--)
        if (m_selected[i]) connectors.erase(connectors.begin() + i);
    // remove selections
    m_selected.erase(std::remove_if(m_selected.begin(), m_selected.end(),
        [](const auto &selected) {
            return selected;}),
        m_selected.end());

    m_selected_count = 0;

    assert(m_selected.size() == connectors.size());
    m_parent.set_as_dirty();
    return true;
}

bool GLGizmoAdvancedCut::is_outside_of_cut_contour(size_t idx, const CutConnectors &connectors, const Vec3d cur_pos)
{
    // check if connector pos is out of clipping plane
    if (m_c->object_clipper() && m_c->object_clipper()->is_projection_inside_cut(cur_pos) == -1) {
        m_info_stats.outside_cut_contour++;
        return true;
    }

    // check if connector bottom contour is out of clipping plane
    const CutConnector &    cur_connector = connectors[idx];
    const CutConnectorShape shape         = CutConnectorShape(cur_connector.attribs.shape);
    const int   sectorCount = shape == CutConnectorShape::Triangle  ? 3 :
                              shape == CutConnectorShape::Square    ? 4 :
                              shape == CutConnectorShape::Circle    ? 60: // supposably, 60 points are enough for conflict detection
                              shape == CutConnectorShape::Hexagon   ? 6 : 1 ;

    indexed_triangle_set mesh;
    auto &               vertices = mesh.vertices;
    vertices.reserve(sectorCount + 1);

    float fa  = 2 * PI / sectorCount;
    auto  vec = Eigen::Vector2f(0, cur_connector.radius);
    for (float angle = 0; angle < 2.f * PI; angle += fa) {
        Vec2f p = Eigen::Rotation2Df(angle) * vec;
        vertices.emplace_back(Vec3f(p(0), p(1), 0.f));
    }
    its_transform(mesh, Geometry::translation_transform(cur_pos) * m_rotate_matrix);

    for (const Vec3f &vertex : vertices) {
        if (m_c->object_clipper()) {
            int  contour_idx = m_c->object_clipper()->is_projection_inside_cut(vertex.cast<double>());
            bool is_invalid  = (contour_idx == -1);
            if (m_part_selection && m_part_selection->valid() && !is_invalid) {
                assert(contour_idx >= 0);
                const std::vector<size_t> &ignored = *(m_part_selection->get_ignored_contours_ptr());
                is_invalid                         = (std::find(ignored.begin(), ignored.end(), size_t(contour_idx)) != ignored.end());
            }
            if (is_invalid) {
                m_info_stats.outside_cut_contour++;
                return true;
            }
        }
    }

    return false;
}

bool GLGizmoAdvancedCut::is_conflict_for_connector(size_t idx, const CutConnectors &connectors, const Vec3d cur_pos)
{
    if (is_outside_of_cut_contour(idx, connectors, cur_pos))
        return true;

    const CutConnector &cur_connector = connectors[idx];

    Transform3d translate_tf = Transform3d::Identity();
    translate_tf.translate(cur_pos);
    Transform3d scale_tf = Transform3d::Identity();
    scale_tf.scale(Vec3f(cur_connector.radius, cur_connector.radius, cur_connector.height).cast<double>());
    const Transform3d   matrix  = translate_tf * m_rotate_matrix * scale_tf;

    const BoundingBoxf3 cur_tbb = m_shapes[cur_connector.attribs].get_bounding_box().transformed(matrix);

    // check if connector's bounding box is inside the object's bounding box
    if (!bounding_box().contains(cur_tbb)) {
        m_info_stats.outside_bb++;
        return true;
    }

    // check if connectors are overlapping
    for (size_t i = 0; i < connectors.size(); ++i) {
        if (i == idx) continue;
        const CutConnector &connector = connectors[i];

        if ((connector.pos - cur_connector.pos).norm() < double(connector.radius + cur_connector.radius)) {
            m_info_stats.is_overlap = true;
            return true;
        }
    }

    return false;
}

void GLGizmoAdvancedCut::switch_to_mode(CutMode new_mode) {
    m_cut_mode = new_mode;
    if (m_cut_mode == CutMode::cutTongueAndGroove) {
        m_cut_to_parts = false;//into Groove function,cancel m_cut_to_parts
    }
    apply_color_clip_plane_colors();
    if (auto oc = m_c->object_clipper()) {
        m_contour_width = m_cut_mode == CutMode::cutTongueAndGroove ? 0.f : 0.4f;
        oc->set_behaviour(m_connectors_editing, m_connectors_editing, double(m_contour_width)); // for debug
    }
    update_plane_model();
    reset_cut_by_contours();
}

void GLGizmoAdvancedCut::flip_cut_plane()
{
    m_rotate_matrix            = m_rotate_matrix * Geometry::rotation_transform(PI * Vec3d::UnitX());
    m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotate_matrix);
    // Orio : angles d'Euler et champs Rotation X/Y/Z resynchronisés avec la matrice. Sans cela
    // les champs restaient périmés, et taper ensuite un angle rebâtissait le plan depuis eux en
    // annulant le retournement (journal des bugs, entrée [39]).
    m_rotation = Geometry::extract_euler_angles(m_rotate_matrix);
    update_buffer_data();

    // instantané pris ici, après le changement de matrice : il enregistre m_start_dragging_m (on_save),
    // encore la matrice d'avant le retournement => Ctrl+Z revient au plan non retourné
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), ("Flip cut plane"));
    m_start_dragging_m = m_rotate_matrix;

    update_clipper();
    if (m_part_selection) {
        m_part_selection->turn_over_selection();
    }
    if (m_cut_mode == CutMode::cutTongueAndGroove)
        reset_cut_by_contours();
}

void GLGizmoAdvancedCut::update_plane_model()
{
    m_plane.reset();
    init_picking_models();
}

static double get_grabber_mean_size(const BoundingBoxf3 &bb) {
    return (bb.size().x() + bb.size().y() + bb.size().z()) / 30.;
}

void GLGizmoAdvancedCut::init_picking_models()
{
    if (!m_plane.is_initialized() && !m_connectors_editing) {
        if (m_cut_mode == CutMode::cutTongueAndGroove) {
            indexed_triangle_set its = its_make_groove_plane(m_groove, m_radius, m_groove_vertices);
            m_plane.init_from(its);
            // m_plane.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));//build error
        } else if (m_cut_mode == CutMode::cutPlanar) {
            const double         cp_width = 0.02 * get_grabber_mean_size(m_bounding_box);
            indexed_triangle_set its      = its_make_frustum_dowel((double) m_cut_plane_radius_koef * m_radius, cp_width, m_cut_plane_as_circle ? 180 : 4);
            m_plane.init_from(its);
        }
    }
}

bool GLGizmoAdvancedCut::has_valid_groove() const
{
    if (m_cut_mode != CutMode::cutTongueAndGroove)
        return true;

    const float flaps_width = -2.f * m_groove.depth / tan(m_groove.flaps_angle);
    if (flaps_width > m_groove.width) return false;

    const Selection &selection = m_parent.get_selection();
    const auto &     list      = selection.get_volume_idxs();
    // is more volumes selected?
    if (list.empty())
        return false;

    const Transform3d cp_matrix = Geometry::translation_transform(m_plane_center) * m_rotate_matrix;
    if (!m_c->raycaster()) {
        if (!m_c->selection_info()) {
            m_c->update(get_requirements());
        }
        wxBusyCursor wait;
        m_c->raycaster_ptr()->update();
    }

    for (size_t id = 0; id < m_groove_vertices.size(); id += 2) {
        const Vec3d beg = cp_matrix * m_groove_vertices[id];
        const Vec3d end = cp_matrix * m_groove_vertices[id + 1];

        bool intersection = false;
        for (const unsigned int volume_idx : list) {
            const GLVolume *glvol = selection.get_volume(volume_idx);
            if (!glvol->is_modifier && m_c->raycaster()) {
                for (size_t i = 0; i < m_c->raycaster()->raycasters().size(); i++) {
                    if (m_c->raycaster()->raycasters()[i]->intersects_line(beg, end - beg, glvol->world_matrix())) { return true; }
                }
            }
        }
        if (!intersection)
            return false;
    }
    return true;
}

bool GLGizmoAdvancedCut::has_valid_contour() const
{
    const auto clipper = m_c->object_clipper();
    return clipper && clipper->has_valid_contour();
}

void GLGizmoAdvancedCut::reset_cut_by_contours()
{
    update_buffer_data();
    update_plane_normal();

    m_part_selection.reset(new PartSelection());

    if (m_cut_mode == CutMode::cutTongueAndGroove) {
        if (m_dragging || m_groove_editing || !has_valid_groove())
            return;
        process_contours();
    } else {
        toggle_model_objects_visibility();
    }
}

void GLGizmoAdvancedCut::process_contours()
{
    const Selection &      selection     = m_parent.get_selection();
    const ModelObjectPtrs &model_objects = selection.get_model()->objects;

    const int instance_idx = selection.get_instance_idx();
    if (instance_idx < 0)
        return;
    const int object_idx = selection.get_object_idx();

    wxBusyCursor wait;

    if (m_cut_mode == CutMode::cutTongueAndGroove) {
        if (has_valid_groove()) {
            Cut                    cut(model_objects[object_idx], instance_idx, get_cut_matrix(selection));
            const ModelObjectPtrs &new_objects = cut.perform_with_groove(m_groove, m_rotate_matrix, true);
            if (!new_objects.empty()) {
                m_part_selection.reset(new PartSelection(new_objects.front(), instance_idx));
            }
        }
    } else {
        if (m_c->object_clipper()) {
            m_part_selection.reset(new PartSelection(model_objects[object_idx], get_cut_matrix(selection), instance_idx, m_plane_center, m_plane_normal, *m_c->object_clipper()));
        }
    }

    toggle_model_objects_visibility();
}

void GLGizmoAdvancedCut::render_flip_plane_button(bool disable_pred /*=false*/)
{
    ImGui::SameLine();

    if (m_hover_id == c_plate_move_id)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonHovered));

    m_imgui->disabled_begin(disable_pred);
    if (m_imgui->button(_L("Flip cut plane")))
        flip_cut_plane();
    m_imgui->disabled_end();

    if (m_hover_id == c_plate_move_id)
        ImGui::PopStyleColor();
}


void GLGizmoAdvancedCut::toggle_model_objects_visibility(bool show_in_3d)
{
    if (m_part_selection && m_part_selection->valid() && show_in_3d == false && (m_is_dragging == false || m_connectors_editing)) // BBL
        m_parent.toggle_model_objects_visibility(false);
    else // if (!m_part_selection.valid())
    {
        const Selection &      selection     = m_parent.get_selection();
        const ModelObjectPtrs &model_objects = selection.get_model()->objects;
        const int idx = selection.get_object_idx();
        if (idx < 0) {
            return;
        }
        if (idx >= model_objects.size()) {
            return;
        }
        m_parent.toggle_model_objects_visibility(true, model_objects[idx], selection.get_instance_idx());
    }
}

void GLGizmoAdvancedCut::deal_connector_pos_by_type(
    Vec3d &pos, float &height, CutConnectorType connector_type, CutConnectorStyle connector_style, bool looking_forward, bool is_edit, const Vec3d &clp_normal)
{//deal pos and height,out :pos and height
    if (connector_type == CutConnectorType::Dowel && connector_style == CutConnectorStyle::Prizm) {
        if (is_edit) {
            height = 0.05f;
            if (!looking_forward) pos += 0.05 * m_clp_normal;
        } else {
            if (looking_forward)
                pos -= static_cast<double>(height) * m_clp_normal;
            else
                pos += static_cast<double>(height) * m_clp_normal;
            height *= 2;
        }
    } else if (!looking_forward) {
        pos += 0.05 * m_clp_normal;
    }
}

void GLGizmoAdvancedCut::update_bb()
{
    const BoundingBoxf3 box = bounding_box();
    if (!box.defined) return;
    if (!m_max_pos.isApprox(box.max) || !m_min_pos.isApprox(box.min)) {
        m_bounding_box = box;
        // check, if mode is set to Planar, when object has a connectors
        if (const int object_idx = m_parent.get_selection().get_object_idx(); object_idx >= 0 && !wxGetApp().plater()->model().objects[object_idx]->cut_connectors.empty())
            m_cut_mode = CutMode::cutPlanar;

        invalidate_cut_plane();
        // Orio : la pièce a bougé ou changé : l'origine et l'axe d'angle, en coordonnées
        // monde, ne valent plus
        if (is_picking())
            stop_picking();
        set_origin_bottom();
        m_hinge.active = false;

        m_max_pos                  = box.max;
        m_min_pos                  = box.min;
        m_bb_center                = box.center();
        m_transformed_bounding_box = transformed_bounding_box(m_bb_center);
        if (box.contains(m_center_offset))
            set_center_pos(m_bb_center + m_center_offset); // update m_plane_center
        else
            set_center_pos(m_bb_center);
        reset_cut_by_contours();
        apply_color_clip_plane_colors();
        m_contour_width = m_cut_mode == CutMode::cutTongueAndGroove ? 0.f : 0.4f;

        m_radius                 = box.radius();
        m_grabber_connection_len = 0.5 * m_radius; // std::min<double>(0.75 * m_radius, 35.0);
        m_grabber_radius         = m_grabber_connection_len * 0.85;

        //// input params for cut with tongue and groove
        m_groove.depth = m_groove.depth_init = std::max(1.f, 0.5f * float(get_grabber_mean_size(m_bounding_box)));
        m_groove.width = m_groove.width_init = 4.0f * m_groove.depth;
        m_groove.flaps_angle = m_groove.flaps_angle_init = float(PI) / 3.f;
        m_groove.angle = m_groove.angle_init = 0.f;
        m_plane.reset();
        clear_selection();
        if (CommonGizmosDataObjects::SelectionInfo *selection = m_c->selection_info(); selection && selection->model_object())
            m_selected.resize(selection->model_object()->cut_connectors.size(), false);
    }
}

void GLGizmoAdvancedCut::check_and_update_connectors_state()
{
    m_info_stats.invalidate();
    m_invalid_connectors_idxs.clear();
    if (m_cut_mode != CutMode::cutPlanar)
        return;
    if (m_c == nullptr || m_c->selection_info() == nullptr) {
        return;
    }
    const ModelObject *mo      = m_c->selection_info()->model_object();
    auto               inst_id = m_c->selection_info()->get_active_instance();
    if (inst_id < 0) return;
    const CutConnectors &connectors      = mo->cut_connectors;
    const ModelInstance *mi              = mo->instances[inst_id];
    const Vec3d &        instance_offset = mi->get_offset();
    const double         sla_shift       = double(m_c->selection_info()->get_sla_shift());

    for (size_t i = 0; i < connectors.size(); ++i) {
        const CutConnector &connector = connectors[i];
        Vec3d               pos       = connector.pos + instance_offset + sla_shift * Vec3d::UnitZ(); // recalculate connector position to world position
        if (is_conflict_for_connector(i, connectors, pos))
            m_invalid_connectors_idxs.emplace_back(i);
    }
}

void GLGizmoAdvancedCut::set_center(const Vec3d &center, bool update_tbb)
{
    set_center_pos(center, update_tbb);
    check_and_update_connectors_state();
    update_clipper();
}

bool GLGizmoAdvancedCut::set_center_pos(const Vec3d &center_pos, bool update_tbb)
{
    BoundingBoxf3 tbb = m_transformed_bounding_box;
    if (update_tbb) {
        Vec3d normal = m_rotate_matrix.inverse() * Vec3d(m_plane_center - center_pos);
        tbb.translate(normal);
    }

    // Orio : le plan ne sort plus de la pièce. Bambu refusait le déplacement au-delà d'une
    // marge de 0,5 mm (hauteur négative possible, et blocage quelques mm avant le bord quand on
    // glisse vite) ; le plan est maintenant ramené pile sur l'extrémité de la pièce, le long de sa
    // normale. tbb est exprimée dans le repère du plan : la pièce s'étend de tbb.min.z() à
    // tbb.max.z() de part et d'autre du plan.
    Vec3d new_center = center_pos;
    if (tbb.defined) {
        double shift = 0.0;
        if (tbb.min.z() > 0.0)
            shift = tbb.min.z();
        else if (tbb.max.z() < 0.0)
            shift = tbb.max.z();
        if (shift != 0.0) {
            new_center += shift * current_plane_normal();
            tbb.translate(Vec3d(0.0, 0.0, -shift));
        }
    }

    m_transformed_bounding_box = tbb;
    m_plane_center             = new_center;
    m_center_offset            = m_plane_center - m_bb_center;
    return true;
}

void GLGizmoAdvancedCut::invalidate_cut_plane()
{
    m_rotate_matrix = Transform3d::Identity();
    m_plane_center  = Vec3d::Zero();
    m_min_pos       = Vec3d::Zero();
    m_max_pos       = Vec3d::Zero();
    m_bb_center     = Vec3d::Zero();
    m_center_offset = Vec3d::Zero();
}

void GLGizmoAdvancedCut::rotate_vec3d_around_plane_center(Vec3d &vec, const Transform3d &rotate_matrix, const Vec3d &center) {
    vec = Geometry::Transformation(Geometry::translation_transform(center) * rotate_matrix * Geometry::translation_transform(-center)).get_matrix() * vec;
}

Transform3d GLGizmoAdvancedCut::get_cut_matrix(const Selection &selection)
{
    return get_cut_matrix(selection, m_plane_center, m_rotate_matrix);
}

Transform3d GLGizmoAdvancedCut::get_cut_matrix(const Selection &selection, const Vec3d &center, const Transform3d &rotation)
{
    const int    instance_idx = selection.get_instance_idx();
    const int    object_idx   = selection.get_object_idx();
    ModelObject *mo           = selection.get_model()->objects[object_idx];
    if (!mo) return Transform3d::Identity();

    // m_cut_z is the distance from the bed. Subtract possible SLA elevation.
    const double sla_shift_z = selection.get_first_volume()->get_sla_shift_z();

    const Vec3d instance_offset   = mo->instances[instance_idx]->get_offset();
    Vec3d       cut_center_offset = center - instance_offset;
    cut_center_offset[Z] -= sla_shift_z;

    return Geometry::translation_transform(cut_center_offset) * rotation;
}

void GLGizmoAdvancedCut::update_buffer_data() {
    m_buffered_rotation = {Geometry::rad2deg(m_rotation(0)), Geometry::rad2deg(m_rotation(1)), Geometry::rad2deg(m_rotation(2))};
    m_buffered_movement = m_movement;
    m_buffered_height   = m_plane_center.z();
    // Orio
    m_buffered_position    = get_position();
    m_buffered_hinge_angle = get_hinge_angle();
}

bool GLGizmoAdvancedCut::render_cut_mode_combo(double label_width, float item_width)
{
    ImGui::AlignTextToFramePadding();
    size_t                   selection_idx = int(m_cut_mode);
    std::vector<std::string> modes         = {_u8L("Planar"), _u8L("Dovetail")};
    bool                     is_changed    = false;

    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    if (render_combo(_u8L("Mode"), modes, selection_idx, label_width, item_width)) {
        is_changed = true;
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Change cut mode");
        switch_to_mode((CutMode) selection_idx);
        check_and_update_connectors_state();
    }
    ImGuiWrapper::pop_combo_style();
    return is_changed;
}

void GLGizmoAdvancedCut::render_color_marker(float size, const ColorRGBA &color)
{
    ImGui::SameLine();
    const float radius = 0.5f * size;
    ImVec2      pos    = ImGui::GetCurrentWindow()->DC.CursorPos;
    pos.x += 3 * m_imgui->scaled(1 / 15.0f);
    pos.y += 1.25f * radius;
    ImGui::GetCurrentWindow()->DrawList->AddNgonFilled(pos, radius, ImGuiWrapper::to_ImU32(color), 6);
}

void GLGizmoAdvancedCut::render_cut_plane_input_window(float x, float y, float bottom_limit)
{
    // float unit_size = m_imgui->get_style_scaling() * 48.0f;
    float        space_size        = m_imgui->get_style_scaling() * 8;
    float        movement_cap      = m_imgui->calc_text_size(_L("Movement:")).x;
    float        rotate_cap        = m_imgui->calc_text_size(_L("Rotate")).x;
    float        groove_angle_size = m_imgui->calc_text_size(_L("Groove Angle")).x;
    float        anchors_cap       = m_imgui->calc_text_size(_L("Plane from anchors")).x; // Orio
    float        caption_size      = std::max({movement_cap, groove_angle_size, anchors_cap}) + 2 * space_size;
    m_label_width                  = caption_size + 1 * space_size;
    m_imperial_units               = wxGetApp().app_config->get("use_inches") == "1";
    unsigned int current_active_id = ImGui::GetActiveID();
    char  buf[3][64];
    float buf_size[3];
    float vec_max = 0, unit_size = 0;
    for (int i = 0; i < 3; i++) {
        ImGui::DataTypeFormatString(buf[i], IM_ARRAYSIZE(buf[i]), ImGuiDataType_Double, (void *) &m_buffered_rotation[i], "%.2f");
        buf_size[i] = ImGui::CalcTextSize(buf[i]).x;
        vec_max     = std::max(buf_size[i], vec_max);
    }

    float buf_size_max = ImGui::CalcTextSize("-100.00").x;
    if (vec_max < buf_size_max) {
        unit_size = buf_size_max + ImGui::GetStyle().FramePadding.x * 2.0f;
    } else {
        unit_size = vec_max + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    // Orio : pendant un choix sur la pièce, le panneau ne montre que les consignes
    if (is_picking()) {
        render_origin_and_anchors(caption_size, space_size, unit_size);
        m_last_active_item_imgui = current_active_id;
        return;
    }

    bool has_connectors = false;
    if (m_c) {
        const auto& p_selection_info = m_c->selection_info();
        if (p_selection_info) {
            const auto& p_model_object = p_selection_info->model_object();
            if (p_model_object) {
                const auto& t_commectors = p_model_object->cut_connectors;
                has_connectors = !t_commectors.empty();
            }
        }
    }

    render_planes_row(caption_size, space_size);

    m_imgui->disabled_begin(has_connectors || several_planes());
    if (render_cut_mode_combo(caption_size + 1 * space_size, 4 * unit_size + 0.5 * space_size)) {
        ;
    }
    ImGui::Separator();
    m_imgui->disabled_end();

    ImGui::PushItemWidth(caption_size);
    ImGui::Dummy(ImVec2(caption_size, -1));
    ImGui::SameLine(caption_size + 1 * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::TextAlignCenter("X");
    ImGui::SameLine(caption_size + 1 * unit_size + 2 * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::TextAlignCenter("Y");
    ImGui::SameLine(caption_size + 2 * unit_size + 3 * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::TextAlignCenter("Z");

    ImGui::AlignTextToFramePadding();

    // Rotation input box
    ImGui::PushItemWidth(caption_size);
    m_imgui->text(_L("Rotation") + " ");
    ImGui::SameLine(caption_size + 1 * space_size);
    ImGui::PushItemWidth(unit_size);
    for (int i = 0; i < m_buffered_rotation.size(); i++) {
        if (abs(m_buffered_rotation[i]) < 1e-3) {
            m_buffered_rotation[i] = 0;
        }
    }
    ImGui::BBLInputDouble("##cut_rotation_x", &m_buffered_rotation[0], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_size + 1 * unit_size + 2 * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble("##cut_rotation_y", &m_buffered_rotation[1], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_size + 2 * unit_size + 3 * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble("##cut_rotation_z", &m_buffered_rotation[2], 0.0f, 0.0f, "%.2f");
    if (m_last_active_item_imgui != current_active_id && (std::abs(Geometry::rad2deg(m_rotation(0)) - m_buffered_rotation(0)) > EPSILON ||
        std::abs(Geometry::rad2deg(m_rotation(1)) - m_buffered_rotation(1)) > EPSILON ||
        std::abs(Geometry::rad2deg(m_rotation(2)) - m_buffered_rotation(2)) > EPSILON )) {
        m_rotation(0) = Geometry::deg2rad(m_buffered_rotation(0));
        m_rotation(1) = Geometry::deg2rad(m_buffered_rotation(1));
        m_rotation(2) = Geometry::deg2rad(m_buffered_rotation(2));

        Geometry::Transformation tran;
        tran.set_rotation(m_rotation);
        rotate_plane_to(tran.get_matrix()); // Orio : autour du pivot, plan gardé sur la pièce
        reset_cut_by_contours();
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Rotate cut plane");
        m_start_dragging_m = m_rotate_matrix; // for takeshot
        m_ar_plane_center  = m_plane_center;  // Orio
    }

    // Orio : « Mouvement » (relatif, remis à 0 après chaque saisie) et « Hauteur » (Z du
    // centre du plan, grisée dès que le plan est incliné) sont remplacés par une « Position »
    // absolue, mesurée le long de la normale du plan depuis son origine, à tout angle
    ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 10.0f));
    render_position_input(caption_size, space_size, unit_size, current_active_id);
    ImGui::PopStyleVar(1);
    render_origin_and_anchors(caption_size, space_size, unit_size);

    ImGui::Separator();
    if (m_cut_mode == CutMode::cutPlanar) {
        m_imgui->disabled_begin(!m_keep_upper || !m_keep_lower || m_cut_to_parts || dropped_pieces() > 0); // Orio : ou un morceau décoché
        // Orio : bouton un peu plus grand (une demi-ligne de plus en hauteur, un caractère de plus
        // de chaque côté), centré sur la largeur de la fenêtre (celle de l'image précédente : la fenêtre
        // s'ajuste à son contenu, le bouton, moins large qu'elle, ne la fait pas grandir)
        const wxString connectors_label = has_connectors ? _L("Edit connectors") : _L("Add connectors");
        const float    font             = m_imgui->get_font_size();
        const ImVec2   connectors_size(m_imgui->calc_text_size(connectors_label).x + 2.f * ImGui::GetStyle().FramePadding.x + 2.f * font,
                                       ImGui::GetFrameHeight() + 0.5f * font);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, 0.5f * (ImGui::GetContentRegionAvail().x - connectors_size.x)));
        if (m_imgui->button(connectors_label, connectors_size.x, connectors_size.y))
            set_connectors_editing(true);
        m_imgui->disabled_end();
    } else if (m_cut_mode == CutMode::cutTongueAndGroove) {
        m_is_slider_editing_done = false;
        m_imgui->text(_L("Groove") + ": "); // ImGuiWrapper::text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, m_labels_map["Groove"] + ": ");

        bool is_changed{false};
        is_changed |= render_slider_double_input(_u8L("Depth"), m_groove.depth, m_groove.depth_tolerance);
        is_changed |= render_slider_double_input(_u8L("Width"), m_groove.width, m_groove.width_tolerance);
        is_changed |= render_slider_double_input_by_format(_u8L("Flap Angle"), m_groove.flaps_angle, 30.f, 120.f, DoubleShowType::DEGREE);
        is_changed |= render_slider_double_input_by_format(_u8L("Groove Angle"), m_groove.angle, 0.f, 15.f, DoubleShowType::DEGREE);

        if (is_changed) {
            update_plane_model();
            reset_cut_by_contours();
        }

        if (m_is_slider_editing_done) {
            m_groove_editing = false;
            reset_cut_by_contours();
        }
    }
    ImGui::Separator();

    // Orio : l'orientation de chaque partie gardée (Maintenir l'orientation / Placer sur la coupe /
    // Retourner, choix unique) est un menu déroulant aligné sur les champs, de la largeur du menu Mode (ou
    // plus si un choix ne tient pas) : la fenêtre est moins large qu'avec trois cases par ligne
    const std::vector<std::string> orientations = {_u8L("Keep orientation"), _u8L("Place on cut"), _u8L("Flip")};
    float                          orientation_width = 4 * unit_size + 0.5f * space_size;
    for (const std::string &orientation : orientations) // le cadre de BBLBeginCombo : largeur - 2 flèches, sa flèche à gauche
        orientation_width = std::max(orientation_width, ImGui::CalcTextSize(orientation.c_str()).x + 3.f * ImGui::GetFrameHeight() + 2.f * ImGui::GetStyle().FramePadding.x);
    auto render_part_action_line = [this, space_size, caption_size, has_connectors, &orientations, orientation_width](const wxString &label, const wxString &suffix, bool &keep_part, bool &place_on_cut_part, bool &rotate_part) {
        bool keep = true;
        m_imgui->disabled_begin(has_connectors || m_cut_to_parts);
        ImGui::AlignTextToFramePadding();
        auto text = m_cut_to_parts ? _L("Part") : _L("Object") + label;
        m_imgui->bbl_checkbox(text, has_connectors ? keep : keep_part);

        float marker_size = m_imgui->calc_text_size(text).y;
        render_color_marker(marker_size, suffix == "##upper" ? UPPER_PART_COLOR : LOWER_PART_COLOR);
        m_imgui->disabled_end();
        m_imgui->disabled_begin(!keep_part || m_cut_to_parts);

        float text_size = m_imgui->calc_text_size(text).x;
        text_size += m_imgui->scaled(2.5f);
        float checkbox_size = m_imgui->calc_text_size(text).y;

        float new_label_width = checkbox_size + text_size + 2 * space_size;
        ImGui::SameLine(std::max(new_label_width, caption_size + 1 * space_size));
        ImGui::SetNextItemWidth(orientation_width);
        const size_t orientation = place_on_cut_part ? 1 : rotate_part ? 2 : 0;
        ImGui::PushID(&keep_part); // un menu par partie
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        if (ImGui::BBLBeginCombo("##orientation", orientations[orientation].c_str(), 0)) {
            // seule la ligne survolée s'allume, le choix en cours se lit dans le champ (comme les Modificateurs)
            for (size_t i = 0; i < orientations.size(); ++i)
                if (ImGui::Selectable(orientations[i].c_str(), false)) {
                    place_on_cut_part = i == 1;
                    rotate_part       = i == 2;
                }
            ImGui::EndCombo();
        }
        ImGuiWrapper::pop_combo_style();
        ImGui::PopID();
        m_imgui->disabled_end();
    };

    // Orio : les connecteurs des autres plans comptent aussi (cases des morceaux, « Couper en parties »)
    bool any_connectors = has_connectors;
    for (int i = 0; i < int(m_multi.planes.size()); ++i)
        any_connectors = any_connectors || (i != m_multi.active && !m_multi.planes[i].connectors.empty());

    m_imgui->text(_L("After cut") + ": ");
    if (several_planes()) { // Orio : une case par morceau (décoché = supprimé), un seul menu d'orientation pour tous
        const bool          locked = any_connectors || m_cut_to_parts; // tout est gardé, comme avec un seul plan
        std::vector<size_t> families;
        uint32_t            flips = 0;
        cut_planes(plane_frames(), families, flips);
        const std::vector<uint32_t> cells = plane_cells();
        const auto label = [this](size_t i) { return (m_cut_to_parts ? _L("Part") : _L("Object")) + " " + from_u8(std::to_string(i + 1)); };
        // sur 2 colonnes (1 à gauche, 2 à droite, 3 à gauche…) : la fenêtre grandissait trop vite. Celle de droite est
        // alignée sur les champs, ou plus loin si une case de gauche, pastille comprise, la dépasse (comme render_part_action_line)
        float column = caption_size + 1 * space_size;
        for (size_t i = 0; i < cells.size(); i += 2) {
            const ImVec2 size = m_imgui->calc_text_size(label(i));
            column            = std::max(column, size.y + size.x + m_imgui->scaled(2.5f) + 2 * space_size);
        }
        m_imgui->disabled_begin(locked);
        for (size_t i = 0; i < cells.size(); ++i) {
            const auto     unchecked = std::find(m_multi.dropped.begin(), m_multi.dropped.end(), cells[i]);
            bool           keep      = locked || unchecked == m_multi.dropped.end();
            const wxString text      = label(i);
            if (i % 2 == 1)
                ImGui::SameLine(column);
            ImGui::AlignTextToFramePadding();
            if (m_imgui->bbl_checkbox(text, keep) && !locked) {
                if (keep)
                    m_multi.dropped.erase(unchecked);
                else
                    m_multi.dropped.push_back(cells[i]);
            }
            render_color_marker(m_imgui->calc_text_size(text).y, cell_color(cells[i] ^ flips, families));
            if (i % 2 == 1 || i + 1 == cells.size())
                ImGui::NewLine(); // render_color_marker laisse la ligne ouverte
        }
        m_imgui->disabled_end();
        m_imgui->disabled_begin(m_cut_to_parts);
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("All the parts"));
        ImGui::SameLine(caption_size + 1 * space_size);
        ImGui::SetNextItemWidth(orientation_width);
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        if (ImGui::BBLBeginCombo("##orientation_all", orientations[m_multi.place_on_cut ? 1 : 0].c_str(), 0)) {
            for (size_t i = 0; i < 2; ++i) // sans « Retourner »
                if (ImGui::Selectable(orientations[i].c_str(), false))
                    m_multi.place_on_cut = i == 1;
            ImGui::EndCombo();
        }
        ImGuiWrapper::pop_combo_style();
        m_imgui->disabled_end();
    } else {
        render_part_action_line( "A", "##upper", m_keep_upper, m_place_on_cut_upper, m_rotate_upper);
        render_part_action_line( "B", "##lower", m_keep_lower, m_place_on_cut_lower, m_rotate_lower);
    }

    if (m_part_selection && m_part_selection->has_modified_cut_parts()) {
        m_cut_to_parts = false;
    }
    m_imgui->disabled_begin(any_connectors || (m_part_selection && m_part_selection->has_modified_cut_parts() && m_part_selection->valid()) ||
                            m_cut_mode == CutMode::cutTongueAndGroove);
    m_imgui->bbl_checkbox(_L("Cut to parts"), m_cut_to_parts);
    if (m_cut_to_parts) {
        m_keep_upper = m_keep_lower = true;
        m_place_on_cut_upper = m_place_on_cut_lower = false;
        m_rotate_upper = m_rotate_lower = false;
        m_multi.place_on_cut = false;
    }
    m_imgui->disabled_end();

#if 0
    // Auto segment input
    ImGui::PushItemWidth(m_imgui->get_style_scaling() * 150.0);
    m_imgui->checkbox(_L("Auto Segment"), m_do_segment);
    m_imgui->disabled_begin(!m_do_segment);
    ImGui::InputDouble("smoothing_alpha", &m_segment_smoothing_alpha, 0.0f, 0.0f, "%.2f");
    m_segment_smoothing_alpha = std::max(0.1, std::min(100.0, m_segment_smoothing_alpha));
    ImGui::InputInt("segment number", &m_segment_number);
    m_segment_number = std::max(1, m_segment_number);
    m_imgui->disabled_end();

    ImGui::Separator();
#endif
    ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    // Orio: under the window where it is (it can be moved).
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + ImGui::GetWindowPos().y;
    show_tooltip_information(ImGui::GetWindowPos().x, get_cur_y);

    float f_scale = m_parent.get_main_toolbar_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    // Cut button
    m_imgui->disabled_begin(!can_perform_cut());
    if (m_imgui->button(_L("Perform cut")))
        perform_cut(m_parent.get_selection());
    m_imgui->disabled_end();
    ImGui::SameLine();
    const bool reset_clicked = m_imgui->button(_L("Reset"));
    ImGui::PopStyleVar(2);
    if (reset_clicked) { reset_all(); }
    m_last_active_item_imgui = current_active_id;
}

void GLGizmoAdvancedCut::init_connectors_input_window_data()
{
    if (!m_c) {
        return;
    }
    const auto p_selection_info = m_c->selection_info();
    if (!p_selection_info) {
        return;
    }
    const auto& p_model_object = p_selection_info->model_object();
    if (!p_model_object) {
        return;
    }

    CutConnectors &connectors = p_model_object->cut_connectors;

    float connectors_cap    = m_imgui->calc_text_size(_L("Connectors")).x;
    float type_cap          = m_imgui->calc_text_size(_L("Type")).x;
    float style_cap         = m_imgui->calc_text_size(_L("Style")).x;
    float shape_cap         = m_imgui->calc_text_size(_L("Shape")).x;
    float depth_ratio_cap   = m_imgui->calc_text_size(_L("Depth ratio")).x;
    float size_cap          = m_imgui->calc_text_size(_L("Size")).x;
    float max_lable_size = std::max(std::max(std::max(connectors_cap, type_cap), std::max(style_cap, shape_cap)), std::max(depth_ratio_cap, size_cap));
    // Orio : libellés du filetage ISO, des coordonnées et des motifs
    for (const wxString &label : {_L("Thread type"), _L("Designation"), _L("Class"), _L("Direction"), _L("Clearance"), _L("Length"), _L("Connector position"),
                                  _L("Pattern"), _L("Section"), _L("Center"), _L("Number"), _L("Radius"), _L("Start angle"), _L("Columns"), _L("Rows"),
                                  _L("Edge margin"), _L("Edge distance"), _L("Start offset"), _L("Spacing X"), _L("Spacing Y")})
        max_lable_size = std::max(max_lable_size, m_imgui->calc_text_size(label).x);

    m_label_width   = double(max_lable_size + 3 + ImGui::GetStyle().WindowPadding.x);


    if (m_connectors_editing && m_selected_count > 0) {
        float             depth_ratio           {UndefFloat};
        float             depth_ratio_tolerance {UndefFloat};
        float             radius                {UndefFloat};
        float             radius_tolerance      {UndefFloat};
        CutConnectorType  type{CutConnectorType::Undef};
        CutConnectorStyle style{CutConnectorStyle::Undef};
        CutConnectorShape shape{CutConnectorShape::Undef};

        bool is_init = false;
        for (size_t idx = 0; idx < m_selected.size(); idx++)
            if (m_selected[idx]) {
                const CutConnector &connector = connectors[idx];
                if (!is_init) {
                    depth_ratio           = connector.height;
                    depth_ratio_tolerance = connector.height_tolerance;
                    radius                = connector.radius;
                    radius_tolerance      = connector.radius_tolerance;
                    type                  = connector.attribs.type;
                    style                 = connector.attribs.style;
                    shape                 = connector.attribs.shape;

                    if (m_selected_count == 1) break;
                    is_init = true;
                } else {
                    if (!is_approx(depth_ratio, connector.height))
                        depth_ratio = UndefFloat;
                    if (!is_approx(depth_ratio_tolerance, connector.height_tolerance))
                        depth_ratio_tolerance = UndefFloat;
                    if (!is_approx(radius, connector.radius))
                        radius = UndefFloat;
                    if (!is_approx(radius_tolerance, connector.radius_tolerance))
                        radius_tolerance = UndefFloat;

                    if (type != connector.attribs.type)
                        type = CutConnectorType::Undef;
                    if (style != connector.attribs.style)
                        style = CutConnectorStyle::Undef;
                    if (shape != connector.attribs.shape)
                        shape = CutConnectorShape::Undef;
                }
            }

        m_connector_depth_ratio           = depth_ratio;
        m_connector_depth_ratio_tolerance = depth_ratio_tolerance;
        m_connector_size                  = 2.f * radius;
        m_connector_size_tolerance        = radius_tolerance;
        m_connector_type                  = type;
        m_connector_style                 = size_t(style);
        m_connector_shape_id              = size_t(shape);

        // Orio : filetage du (premier) connecteur sélectionné
        if (type == CutConnectorType::Thread)
            for (size_t idx = 0; idx < m_selected.size() && idx < connectors.size(); ++idx)
                if (m_selected[idx]) {
                    const CutConnectorParas &paras = connectors[idx].paras;
                    m_thread_designation = size_t(iso_metric_thread_valid(paras.thread_designation) ? paras.thread_designation :
                                                                                                       iso_metric_thread_closest(2. * connectors[idx].radius));
                    m_thread_class       = size_t(std::clamp(paras.thread_class, 0, 1));
                    m_thread_hand        = paras.thread_left_hand ? 1 : 0;
                    break;
                }
    }
}

void GLGizmoAdvancedCut::render_connectors_input_window(float x, float y, float bottom_limit)
{
    if (!m_c) {
        return;
    }
    const auto p_selection_info = m_c->selection_info();
    if (!p_selection_info) {
        return;
    }
    const auto& p_model_object = p_selection_info->model_object();
    if (!p_model_object) {
        return;
    }
    CutConnectors &connectors = p_model_object->cut_connectors;

    // update when change input window
    m_imgui->set_requires_extra_frame();

    ImGui::AlignTextToFramePadding();
    // Orio : en orange seulement quand il y a des connecteurs (ce que ↺ efface)
    if (connectors.empty())
        m_imgui->text(_L("Connectors"));
    else
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _L("Connectors"));

    // ↺ seulement quand il y a des connecteurs à retirer (comme les autres réglages modifiés)
    if (!connectors.empty()) {
        ImGui::SameLine(m_label_width);
        if (render_reset_button("connectors", _u8L("Remove connectors")))
            reset_connectors();
    }

    render_flip_plane_button(m_connectors_editing && connectors.empty());

    m_imgui->text(_L("Type"));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.00f, 0.00f, 0.00f, 1.00f));
    bool type_changed = render_connect_type_radio_button(CutConnectorType::Plug);
    type_changed |= render_connect_type_radio_button(CutConnectorType::Dowel);
    type_changed |= render_connect_type_radio_button(CutConnectorType::Snap);
    type_changed |= render_connect_type_radio_button(CutConnectorType::Thread);
    // Orio : un seul filetage par face coupée (il se visse en tournant ; une pièce en Y a deux
    // faces). Passer la sélection en filetage n'en convertit qu'un par face : le premier sélectionné
    // d'une face qui n'en a pas encore. Si aucune face sélectionnée n'en manque, leurs filetages sont
    // sélectionnés à la place (rien n'est converti, le panneau montre leurs réglages).
    if (type_changed && m_connector_type == CutConnectorType::Thread && m_selected_count > 0 && m_selected.size() == connectors.size()) {
        update_sections();
        std::vector<bool> convert(connectors.size(), false), existing(connectors.size(), false);
        std::set<int>     faces_done;
        for (size_t idx = 0; idx < connectors.size(); ++idx)
            if (m_selected[idx] && faces_done.insert(connector_section(connectors[idx])).second) {
                const int thread_idx = thread_on_section(connectors, connector_section(connectors[idx]));
                if (thread_idx >= 0)
                    existing[thread_idx] = true;
                else
                    convert[idx] = true;
            }
        const bool any_convert = std::find(convert.begin(), convert.end(), true) != convert.end();
        m_selected             = any_convert ? convert : existing;
        m_selected_count       = int(std::count(m_selected.begin(), m_selected.end(), true));
        if (!any_convert)
            type_changed = false;
    }
    if (type_changed)
        apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.type = CutConnectorType(m_connector_type); });
    ImGui::PopStyleColor(1);
    const bool  is_thread  = m_connector_type == CutConnectorType::Thread;
    const float wrap_width = 20.0f * m_imgui->get_font_size();

    // Orio : passage au filetage : diamètre et paramètres de la désignation ISO en cours,
    // longueur d'au moins deux pas
    if (type_changed && is_thread) {
        if (m_thread_designation >= size_t(ISO_METRIC_THREADS_COUNT))
            m_thread_designation = size_t(iso_metric_thread_closest(10.));
        const IsoMetricThread &thread = ISO_METRIC_THREADS[m_thread_designation];
        m_connector_size              = float(thread.size);
        if (m_connector_depth_ratio > 0.f)
            m_connector_depth_ratio = std::max(m_connector_depth_ratio, float(2. * thread.pitch));
        apply_selected_connectors([this, &connectors, &thread](size_t idx) {
            connectors[idx].paras  = current_connector_paras();
            connectors[idx].radius = float(0.5 * thread.size);
            connectors[idx].height = std::max(connectors[idx].height, float(2. * thread.pitch));
        });
    }

    std::vector<std::string> connector_styles = {_u8L("Prizm"), _u8L("Frustum")};
    std::vector<std::string> connector_shapes = { _u8L("Triangle"), _u8L("Square"), _u8L("Hexagon"), _u8L("Circle") };

    if (type_changed && m_connector_type == CutConnectorType::Dowel) {
        m_connector_style = size_t(CutConnectorStyle::Prizm);
        apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.style = CutConnectorStyle(m_connector_style); });
    }
    // un filetage ou un clip est rond
    if (type_changed && (m_connector_type == CutConnectorType::Snap || is_thread)) {
        m_connector_shape_id = int(CutConnectorShape::Circle);
        apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.shape = CutConnectorShape(m_connector_shape_id); });
    }

    if (is_thread) {
        // Orio : filetage ISO métrique (type, taille, désignation, classe, sens, jeu)
        render_thread_input();
    } else {
        m_imgui->disabled_begin(m_connector_type == CutConnectorType::Dowel || m_connector_type == CutConnectorType::Snap);
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        if (render_combo(_u8L("Style"), connector_styles, m_connector_style, m_label_width, m_editing_window_width))
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.style = CutConnectorStyle(m_connector_style); });
        ImGuiWrapper::pop_combo_style();
        m_imgui->disabled_end();

        m_imgui->disabled_begin(m_connector_type == CutConnectorType::Snap);
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        if (render_combo(_u8L("Shape"), connector_shapes, m_connector_shape_id, m_label_width, m_editing_window_width))
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].attribs.shape = CutConnectorShape(m_connector_shape_id); });
        ImGuiWrapper::pop_combo_style();
        m_imgui->disabled_end();
    }

    // profondeur ; pour un filetage, sa longueur (au moins deux pas)
    if (render_slider_double_input(is_thread ? _u8L("Length") : _u8L("Depth ratio"), m_connector_depth_ratio, m_connector_depth_ratio_tolerance)) {
        if (is_thread && m_connector_depth_ratio > 0.f)
            m_connector_depth_ratio = std::max(m_connector_depth_ratio, float(2. * ISO_METRIC_THREADS[m_thread_designation].pitch));
        apply_selected_connectors([this, &connectors](size_t idx) {
            if (m_connector_depth_ratio > 0)
                connectors[idx].height = m_connector_depth_ratio;
            if (m_connector_depth_ratio_tolerance >= 0)
                connectors[idx].height_tolerance = m_connector_depth_ratio_tolerance;
        });
    }

    // taille libre (un filetage prend celle de sa désignation ISO)
    if (!is_thread && render_slider_double_input(_u8L("Size"), m_connector_size, m_connector_size_tolerance))
        apply_selected_connectors([this, &connectors](size_t idx) {
            if (m_connector_size > 0)
                connectors[idx].radius = 0.5f * m_connector_size;
            if (m_connector_size_tolerance >= 0)
                connectors[idx].radius_tolerance = m_connector_size_tolerance;
        });

    // Orio : « Magnétisme des ancrages » (ex « Milieu de la géométrie ») : ancrages de la
    // section ; inactif, le connecteur se pose exactement où l'on clique
    m_imgui->bbl_checkbox(_L("Anchor magnetism"), m_auto_center_connector);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("The connector snaps to the remarkable points of the section near the mouse, always at least 5 mm from the edges: "
                            "centroid, farthest point from the edges, corners and middles of the sides, rays to the corners and to the sides, "
                            "foci of an ellipse, medians and diagonals of round or rectangular sections. Elsewhere it stays free."),
                         wrap_width);
    if (m_auto_center_connector && m_hover_anchor >= 0 && m_hover_anchor < int(m_section_anchors.size()))
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, anchor_name(m_section_anchors[m_hover_anchor]));

    // coordonnées du connecteur sélectionné dans le repère de sa section
    render_selected_connector_coordinates();

    if (m_connector_type == CutConnectorType::Snap) {
        m_imgui->text(_L("Snap global parameters") +": ");
        const std::string format = "%.0f %%";
        bool is_changed = false;
        if (render_slider_double_input_by_format(_u8L("Bulge"), m_snap_bulge_proportion, 5.f, 100.f * m_snap_space_proportion, DoubleShowType::PERCENTAGE)) {
            is_changed = true;
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].paras.snap_bulge_proportion = m_snap_bulge_proportion; });
        }
        if (render_slider_double_input_by_format(_u8L("Gap"), m_snap_space_proportion, 10.f, 50.f, DoubleShowType::PERCENTAGE)) {
            is_changed = true;
            apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].paras.snap_space_proportion = m_snap_space_proportion; });
        }
        if (is_changed) {
            update_connector_shape();
        }
    }
    // Orio : motifs (cercle, grille, le long du bord, entre deux points, répartition) et
    // symétrie des connecteurs sélectionnés
    render_patterns_input();
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    // Orio: under the window where it is (it can be moved).
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + ImGui::GetWindowPos().y;
    show_tooltip_information(ImGui::GetWindowPos().x, get_cur_y);

    float f_scale = m_parent.get_main_toolbar_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    if (m_imgui->button(_L("Confirm connectors"))) {
        unselect_all_connectors();
        set_connectors_editing(false);
    }

    ImGui::SameLine(m_label_width + m_editing_window_width - m_imgui->calc_text_size(_L("Cancel")).x - m_imgui->get_style_scaling() * 8);

    if (m_imgui->button(_L("Cancel"))) {
        // Orio : remet les connecteurs d'avant l'édition (Bambu retirait tout, même ceux déjà confirmés)
        m_c->selection_info()->model_object()->cut_connectors = m_connectors_before_editing;
        clear_selection();
        check_and_update_connectors_state();
        set_connectors_editing(false);
    }

    ImGui::PopStyleVar(2);
}

void GLGizmoAdvancedCut::render_input_window_warning() const
{
    if (!m_invalid_connectors_idxs.empty()) {
        wxString out = /*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Invalid connectors detected") + ":";
        if (m_info_stats.outside_cut_contour > size_t(0))
            out += "\n - " + std::to_string(m_info_stats.outside_cut_contour) +
                   (m_info_stats.outside_cut_contour == 1 ? _L("connector is out of cut contour") : _L("connectors are out of cut contour"));
        if (m_info_stats.outside_bb > size_t(0))
            out += "\n - " + std::to_string(m_info_stats.outside_bb) +
                   (m_info_stats.outside_bb == 1 ? _L("connector is out of object") : _L("connectors is out of object"));
        if (m_info_stats.is_overlap)
            out += "\n - " + _L("Some connectors are overlapped");
        m_imgui->warning_text(out);
    }
    if ((!m_keep_upper && !m_keep_lower) || all_pieces_dropped()) {
        m_imgui->warning_text(/*wxString(ImGui::WarningMarkerSmall)*/ _L("Warning") + ": " + _L("Invalid state. \nNo one part is selected to keep after cut"));
    }
    if (several_planes())
        for (int i = 0; i < int(m_multi.planes.size()); ++i)
            if (!plane_crosses_piece(i))
                m_imgui->warning_text(_L("Warning") + ": " + format_wxstr(_L("Plane %1% does not cut the part."), i + 1));
}

bool GLGizmoAdvancedCut::render_reset_button(const std::string &label_id, const std::string &tooltip) const
{
    const ImGuiStyle &style = ImGui::GetStyle();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {1, style.ItemSpacing.y});

    ImGui::PushStyleColor(ImGuiCol_Button, {0.25f, 0.25f, 0.25f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.4f, 0.4f, 0.4f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.4f, 0.4f, 0.4f, 1.0f});

    bool revert = m_imgui->button(wxString(ImGui::RevertBtn));

    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered())
        m_imgui->tooltip(tooltip.c_str(), ImGui::GetFontSize() * 20.0f);

    ImGui::PopStyleVar();

    return revert;
}

bool GLGizmoAdvancedCut::render_connect_type_radio_button(CutConnectorType type)
{
    // Orio: the first one in the column of the controls, the others right after it (narrower window).
    if (type == CutConnectorType::Plug)
        ImGui::SameLine(m_label_width);
    else
        ImGui::SameLine();
    ImGui::PushItemWidth(m_control_width);

    wxString radio_name;
    switch (type) {
    case CutConnectorType::Plug:
        radio_name = _L("Plug");
        break;
    case CutConnectorType::Dowel:
        radio_name = _L("Dowel");
        break;
    case CutConnectorType::Snap:
        radio_name = _L("Snap");
        break;
        case CutConnectorType::Thread:
        radio_name = _L("Thread");
        break;
    default:
        break;
    }

    if (m_imgui->radio_button(radio_name, m_connector_type == type)) {
        m_connector_type = type;
        return true;
    }
    return false;
}

bool GLGizmoAdvancedCut::render_slider_double_input(const std::string &label, float &value_in, float &tolerance_in)
{
    // -------- [ ] -------- [ ]
    // slider_with + item_in_gap + first_input_width + item_out_gap + slider_with + item_in_gap + second_input_width
    double slider_with          = 0.24 * m_editing_window_width;
    double item_in_gap          = 0.01 * m_editing_window_width;
    double item_out_gap         = 0.04 * m_editing_window_width;
    double first_input_width    = 0.37  * m_editing_window_width;
    double second_input_width   = 0.37  * m_editing_window_width;
#ifdef __APPLE__
    first_input_width = 0.5 * m_editing_window_width;
    second_input_width   = 0.5  * m_editing_window_width;
#endif
    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(slider_with);

    double left_width = m_label_width + slider_with + item_in_gap;

    float value = value_in;
    if (m_imperial_units) value *= float(units_mm_to_in);
    float old_val = value;

    constexpr float UndefMinVal = -0.1f;

    const BoundingBoxf3 bbox      = bounding_box();
    float               mean_size = float((bbox.size().x() + bbox.size().y() + bbox.size().z()) / 2.0);
    float               min_size  = value_in < 0.f ? UndefMinVal : 1.f;
    if (m_imperial_units) {
        mean_size *= float(units_mm_to_in);
        min_size *= float(units_mm_to_in);
    }
    std::string format = value_in < 0.f ? " " : m_imperial_units ? "%.4f  " + _u8L("in") : "%.2f  " + _u8L("mm");

    m_imgui->bbl_slider_float_style(("##" + label).c_str(), &value, min_size, mean_size, format.c_str());

    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(first_input_width);
    ImGui::BBLDragFloat(("##input_" + label).c_str(), &value, 0.05f, min_size, mean_size, format.c_str());

    value_in = value * float(m_imperial_units ? units_in_to_mm : 1.0);

    left_width += (first_input_width + item_out_gap);
    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(slider_with);

    float tolerance = tolerance_in;
    if (m_imperial_units)
        tolerance *= float(units_mm_to_in);
    float old_tolerance = tolerance;
    //std::string format_t      = tolerance_in < 0.f ? " " : "%.f %%";
    float       min_tolerance = tolerance_in < 0.f ? UndefMinVal : 0.f;

    m_imgui->bbl_slider_float_style(("##tolerance_" + label).c_str(), &tolerance, min_tolerance, 2.f, format.c_str(), 1.f, true, _L("Tolerance"));

    left_width += (slider_with + item_in_gap);
    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(second_input_width);
    ImGui::BBLDragFloat(("##tolerance_input_" + label).c_str(), &tolerance, 0.05f, min_tolerance, 2.f, format.c_str());

    tolerance_in = tolerance * float(m_imperial_units ? units_in_to_mm : 1.0);

    return !is_approx(old_val, value) || !is_approx(old_tolerance, tolerance);
}

bool GLGizmoAdvancedCut::render_slider_double_input_by_format(const std::string &label, float &value_in, float value_min, float value_max, DoubleShowType show_type)
{
    // slider_with + item_in_gap + first_input_width + item_out_gap
    double slider_with       = 0.24 * m_editing_window_width;
    double item_in_gap       = 0.01 * m_editing_window_width;
    double item_out_gap      = 0.01 * m_editing_window_width;
    double first_input_width = 0.29 * m_editing_window_width;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    ImGui::PushItemWidth(slider_with);

    double      left_width = m_label_width + slider_with + item_in_gap;
    float       old_val    = value_in; // (show_type == DoubleShowType::Normal)
    float       value      = value_in; // (show_type == DoubleShowType::Normal)
    std::string format     = "%.0f";
    if (show_type == DoubleShowType::PERCENTAGE) {
        format  = "%.0f %%";
        old_val = value_in;
        value   = value_in * 100;
    } else if (show_type == DoubleShowType::DEGREE) {
        format  = "%.0f " + _u8L("°");
        old_val = value_in;
        value   = Geometry::rad2deg(value_in);
    }

    if (m_imgui->bbl_slider_float_style(("##" + label).c_str(), &value, value_min, value_max, format.c_str())) {
        if (show_type == DoubleShowType::PERCENTAGE) {
            value_in = value * 0.01f;
        } else if (show_type == DoubleShowType::DEGREE) {
            value_in = Geometry::deg2rad(value);
        } else { //(show_type == DoubleShowType::Normal)
            value_in = value;
        }
    }

    ImGui::SameLine(left_width);
    ImGui::PushItemWidth(first_input_width);
    if (ImGui::BBLDragFloat(("##input_" + label).c_str(), &value, 0.05f, value_min, value_max, format.c_str())) {
        if (show_type == DoubleShowType::PERCENTAGE) {
            value_in = value * 0.01f;
        } else if (show_type == DoubleShowType::DEGREE) {
            value_in = Geometry::deg2rad(value);
        } else { //(show_type == DoubleShowType::Normal)
            value_in = value;
        }
    }
    return !is_approx(old_val, value_in);
}

bool GLGizmoAdvancedCut::cut_line_processing() const {
    return m_cut_line_begin != Vec3d::Zero();
}

void GLGizmoAdvancedCut::discard_cut_line_processing()
{
    m_cut_line_begin = m_cut_line_end = Vec3d::Zero();
}

bool GLGizmoAdvancedCut::process_cut_line(SLAGizmoEventType action, const Vec2d &mouse_position)
{
    const Camera &camera = wxGetApp().plater()->get_camera();

    Vec3d pt;
    Vec3d dir;
    MeshRaycaster::line_from_mouse_pos_static(mouse_position, Transform3d::Identity(), camera, pt, dir);
    dir.normalize();
    pt += dir; // Move the pt along dir so it is not clipped.

    if (action == SLAGizmoEventType::LeftDown && !cut_line_processing()) {
        m_cut_line_begin = pt;
        m_cut_line_end   = pt;
        return true;
    }

    if (cut_line_processing()) {
        if (m_cut_mode == CutMode::cutTongueAndGroove)
            m_groove_editing = true;

        m_cut_line_end = pt;
        if (action == SLAGizmoEventType::LeftUp) {
            Vec3d line_dir = m_cut_line_end - m_cut_line_begin;
            if (line_dir.norm() < 3.0) {
                discard_cut_line_processing();
                return true;
            }

            Vec3d              cross_dir = line_dir.cross(dir).normalized();
            Eigen::Quaterniond q;
            Transform3d        m         = Transform3d::Identity();
            m.matrix().block(0, 0, 3, 3) = q.setFromTwoVectors(Vec3d::UnitZ(), cross_dir).toRotationMatrix();

            const Vec3d new_plane_center = m_bb_center + cross_dir * cross_dir.dot(pt - m_bb_center);
            // update transformed bb
            const auto      new_tbb         = transformed_bounding_box(new_plane_center, m);
            const GLVolume *first_volume    = m_parent.get_selection().get_first_volume();
            Vec3d           instance_offset = first_volume->get_instance_offset();
            instance_offset[Z] += first_volume->get_sla_shift_z();

            const Vec3d trans_center_pos = m.inverse() * (new_plane_center - instance_offset) + new_tbb.center();
            if (new_tbb.contains(trans_center_pos)) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Cut by line");
                // Orio : rotation posée AVANT le centre (set_center le ramène sur la pièce le
                // long de la normale du plan) ; l'axe d'angle ne vaut plus
                m_hinge.active                       = false;
                m_start_dragging_m = m_rotate_matrix = m;
                m_rotation                           = Geometry::extract_euler_angles(m_rotate_matrix);
                m_transformed_bounding_box           = new_tbb;
                set_center(new_plane_center);
                m_plane_normal                       = m_rotate_matrix * Vec3d::UnitZ();
                m_ar_plane_center                    = m_plane_center;
                reset_cut_by_contours();
            }

            discard_cut_line_processing();

            if (m_cut_mode == CutMode::cutTongueAndGroove) {
                m_groove_editing = false;
                if (new_tbb.contains(trans_center_pos)) {
                    reset_cut_by_contours();
                }
            }
        } else if (action == SLAGizmoEventType::Moving) {
            this->set_dirty();
        }
        return true;
    }
    return false;
}

// =============================================================================================
// Orio : position absolue du plan, origine (pivot) et plans définis par des ancrages
// =============================================================================================

static const ColorRGBA CUT_PICK_HOVER_COLOR  = {0.0f, 1.0f, 0.0f, 1.0f};  // ancrage survolé (vert, comme l'outil Mesure)
static const ColorRGBA CUT_PICK_CHOSEN_COLOR = {1.0f, 0.55f, 0.0f, 1.0f}; // ancrages choisis, origine

Vec3d GLGizmoAdvancedCut::current_plane_normal() const
{
    return (m_rotate_matrix.linear() * Vec3d::UnitZ()).normalized();
}

// Centre des rotations du plan : l'origine choisie, sinon le centre du plan (comme Bambu).
Vec3d GLGizmoAdvancedCut::rotation_pivot() const
{
    return m_origin_type == OriginType::Custom ? m_origin : m_plane_center;
}

// Sens de mesure de la position : la normale du plan, tournée vers le haut quand l'origine est le
// bas de la pièce (« Retourner le plan de coupe » ne change alors pas la position affichée).
Vec3d GLGizmoAdvancedCut::position_axis() const
{
    const Vec3d normal = current_plane_normal();
    return (m_origin_type == OriginType::Bottom && normal.z() < -EPSILON) ? Vec3d(-normal) : normal;
}

// Étendue de la pièce le long du sens de mesure, de part et d'autre du plan (lo <= 0 <= hi quand
// le plan coupe la pièce). m_transformed_bounding_box est dans le repère du plan.
void GLGizmoAdvancedCut::object_extent_along_axis(double &lo, double &hi) const
{
    lo = hi = 0.0;
    if (!m_transformed_bounding_box.defined)
        return;
    const bool reversed = position_axis().dot(current_plane_normal()) < 0.0;
    lo = reversed ? -m_transformed_bounding_box.max.z() : m_transformed_bounding_box.min.z();
    hi = reversed ? -m_transformed_bounding_box.min.z() : m_transformed_bounding_box.max.z();
}

// Position du plan : sa distance à l'origine, le long du sens de mesure.
double GLGizmoAdvancedCut::get_position() const
{
    if (m_origin_type == OriginType::Custom)
        return (m_plane_center - m_origin).dot(position_axis());
    double lo, hi;
    object_extent_along_axis(lo, hi);
    return -lo; // distance au point le plus bas de la pièce
}

// Positions possibles : le plan doit toucher la pièce.
void GLGizmoAdvancedCut::get_position_range(double &lo, double &hi) const
{
    const double position = get_position();
    object_extent_along_axis(lo, hi);
    lo += position;
    hi += position;
}

// Place le plan à « position » (bornée à la pièce). Pas d'instantané d'annulation ici :
// l'appelant le prend une fois le geste fini.
void GLGizmoAdvancedCut::set_position(double position)
{
    double lo, hi;
    get_position_range(lo, hi);
    const double delta = std::clamp(position, lo, hi) - get_position();
    if (std::abs(delta) > EPSILON)
        set_center(m_plane_center + delta * position_axis(), true);
}

// Les valeurs archivées pour l'annulation rejoignent le plan courant (après un instantané).
void GLGizmoAdvancedCut::sync_archived_plane()
{
    m_ar_plane_center  = m_plane_center;
    m_start_dragging_m = m_rotate_matrix;
}

// Orio : plusieurs plans de coupe. Le plan actif vit dans les membres habituels de l'outil
// (m_plane_center, m_rotate_matrix, origine) ; sa case de m_multi n'est à jour qu'après store_active_plane().
const ModelInstance *GLGizmoAdvancedCut::selected_instance() const
{
    const Selection &selection    = m_parent.get_selection();
    const int        object_idx   = selection.get_object_idx();
    const int        instance_idx = selection.get_instance_idx();
    const ModelObjectPtrs &objects = selection.get_model()->objects;
    if (object_idx < 0 || object_idx >= int(objects.size()) || instance_idx < 0 || instance_idx >= int(objects[object_idx]->instances.size()))
        return nullptr;
    return objects[object_idx]->instances[instance_idx];
}

void GLGizmoAdvancedCut::store_active_plane()
{
    if (!several_planes())
        return;
    SavedPlane &plane = m_multi.planes[m_multi.active];
    plane.center      = m_plane_center;
    plane.rotation    = m_rotate_matrix;
    plane.origin_type = int(m_origin_type);
    plane.origin      = m_origin;
    plane.origin_kind = int(m_origin_kind);
    plane.hinge       = m_hinge;
    // ses connecteurs vivent dans la pièce (outil de Bambu) ; plus sélectionnée, restore_planes les relira
    if (const ModelInstance *instance = selected_instance(); instance != nullptr && instance->get_object()->id() == m_multi.object)
        plane.connectors = instance->get_object()->cut_connectors;
}

void GLGizmoAdvancedCut::load_plane(int idx)
{
    m_multi.active          = idx;
    const SavedPlane &plane = m_multi.planes[idx];
    m_rotate_matrix = plane.rotation;
    m_origin_type   = OriginType(plane.origin_type);
    m_origin        = plane.origin;
    m_origin_kind   = PickKind(plane.origin_kind);
    m_hinge         = plane.hinge;
    m_transformed_bounding_box = transformed_bounding_box(plane.center, m_rotate_matrix);
    // l'outil de Bambu édite les connecteurs de la pièce : ceux de ce plan y entrent
    if (const ModelInstance *instance = selected_instance(); instance != nullptr && instance->get_object()->id() == m_multi.object) {
        instance->get_object()->cut_connectors = plane.connectors;
        clear_selection();
        m_selected.resize(plane.connectors.size(), false);
    }
    set_center(plane.center); // met aussi la normale à jour (update_clipper)
    check_and_update_connectors_state(); // set_center les teste avant update_clipper, sur la coupe du plan précédent
    m_rotation = Geometry::extract_euler_angles(m_rotate_matrix);
    m_movement = 0;
    sync_archived_plane();
    reset_cut_by_contours(); // met aussi les champs à jour (update_buffer_data)
    m_parent.request_extra_frame();
}

void GLGizmoAdvancedCut::add_plane()
{
    const ModelInstance *instance = selected_instance();
    if (instance == nullptr)
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Add cut plane");
    if (!several_planes()) {
        m_multi.object   = instance->get_object()->id();
        m_multi.instance = instance->get_matrix();
        m_multi.planes.resize(1);
        m_multi.active   = 0;
        m_keep_upper = m_keep_lower = true;
    }
    store_active_plane();
    m_multi.dropped.clear(); // les morceaux changent : tout est recoché
    SavedPlane plane   = m_multi.planes[m_multi.active];
    plane.hinge.active = false; // la copie décalée ne contient plus l'axe
    plane.connectors.clear();
    // à mi-chemin du bord le plus éloigné : toujours dans la pièce
    const BoundingBoxf3 &box = m_transformed_bounding_box;
    plane.center += 0.5 * (box.max.z() >= -box.min.z() ? box.max.z() : box.min.z()) * current_plane_normal();
    m_multi.planes.push_back(plane);
    load_plane(int(m_multi.planes.size()) - 1);
}

void GLGizmoAdvancedCut::delete_active_plane()
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Delete cut plane");
    m_multi.planes.erase(m_multi.planes.begin() + m_multi.active);
    m_multi.dropped.clear(); // les côtés des plans suivants changent de rang
    load_plane(std::min(m_multi.active, int(m_multi.planes.size()) - 1));
    if (m_multi.planes.size() == 1)
        forget_planes();
}

void GLGizmoAdvancedCut::forget_planes()
{
    m_multi = PlaneList{};
    m_parent.set_color_clip_sections({}); // sans attendre update_clipper : la pièce suivante n'a pas une image aux couleurs de celle-ci
}

// m_multi reste rempli : l'instantané pris en quittant l'outil (LeavingGizmoWithAction) arrive juste après
// l'état Off, et une annulation qui rouvre l'outil doit y retrouver les plans.
void GLGizmoAdvancedCut::park_planes()
{
    if (!several_planes())
        return;
    store_active_plane();
    m_parked_planes[m_multi.object.id] = m_multi;
}

void GLGizmoAdvancedCut::restore_planes()
{
    forget_planes(); // la liste gardée pour l'instantané de sortie de l'outil
    const ModelInstance *instance = selected_instance();
    if (instance == nullptr)
        return;
    const auto parked = m_parked_planes.find(instance->get_object()->id().id);
    if (parked == m_parked_planes.end())
        return;
    m_multi = parked->second;
    m_parked_planes.erase(parked);
    m_multi.planes[m_multi.active].connectors = instance->get_object()->cut_connectors; // la pièce fait foi (annulation…)
    // La pièce a pu bouger entre-temps : les plans la suivent, sauf mise à l'échelle ou miroir
    const Transform3d moved  = instance->get_matrix() * m_multi.instance.inverse();
    const Matrix3d    linear = moved.linear();
    if (!(linear * linear.transpose()).isApprox(Matrix3d::Identity(), 1e-6) || linear.determinant() < 0.) {
        forget_planes();
        return;
    }
    if (!moved.isApprox(Transform3d::Identity(), 1e-6))
        for (SavedPlane &plane : m_multi.planes) {
            plane.center            = moved * plane.center;
            plane.rotation.linear() = linear * plane.rotation.linear();
            plane.origin_type       = int(OriginType::Bottom);
            plane.hinge.active      = false;
            for (CutConnector &connector : plane.connectors) // positions relatives au décalage de l'instance
                connector.pos = linear * connector.pos;
        }
    m_multi.instance = instance->get_matrix();
    m_keep_upper = m_keep_lower = true; // une autre pièce a pu les décocher, leurs lignes sont masquées avec plusieurs plans
    if (m_cut_mode != CutMode::cutPlanar)
        switch_to_mode(CutMode::cutPlanar);
    load_plane(m_multi.active);
}

// Le plan traverse-t-il la pièce ? (posé sur une extrémité, il n'a rien à couper)
// ponytail: boîte recalculée à chaque image pour les plans en attente, à garder en cache si la fenêtre rame
bool GLGizmoAdvancedCut::plane_crosses_piece(int idx) const
{
    const BoundingBoxf3 box = idx == m_multi.active ? m_transformed_bounding_box :
                                                      transformed_bounding_box(m_multi.planes[idx].center, m_multi.planes[idx].rotation);
    return !box.defined || (box.min.z() < -EPSILON && box.max.z() > EPSILON);
}

// Orio : une couleur par section (gouraud.fs). Les plans parallèles forment une famille orientée dans un même
// sens, de poids 2^famille modulo 7 (1, 2, 4, 1…) : traverser un plan change toujours de couleur, et des plans
// parallèles colorent leurs tranches dans l'ordre (magenta, cyan, orange…).
// ponytail: 16 plans au plus (MAX_CUT_PLANES de gouraud.fs), les suivants ne changent plus les couleurs ; agrandir le tableau du shader s'il en faut plus
GLVolumeCollection::ColorClipSections GLGizmoAdvancedCut::color_clip_sections() const
{
    GLVolumeCollection::ColorClipSections sections;
    sections.colors = SECTION_COLORS;
    std::vector<size_t> families;
    uint32_t            flips  = 0;
    const auto          planes = cut_planes(plane_frames(), families, flips);
    for (size_t i = 0; i < std::min<size_t>(planes.size(), 16); ++i) {
        const double sign   = ((flips >> i) & 1) ? -1. : 1.;
        const Vec3d  normal = sign * planes[i].first;
        sections.planes.push_back({float(normal.x()), float(normal.y()), float(normal.z()), float(sign * planes[i].second)});
        sections.weights.push_back(float(1 << (families[i] % 3)));
    }
    return sections;
}

// Centre et normale de chaque plan, le plan actif lu dans l'outil (sa case de m_multi est périmée)
std::vector<std::pair<Vec3d, Vec3d>> GLGizmoAdvancedCut::plane_frames() const
{
    std::vector<std::pair<Vec3d, Vec3d>> frames;
    for (int i = 0; i < int(m_multi.planes.size()); ++i) {
        const bool active = i == m_multi.active;
        frames.emplace_back(active ? m_plane_center : m_multi.planes[i].center,
                            Vec3d((active ? m_rotate_matrix : m_multi.planes[i].rotation).linear() * Vec3d::UnitZ()));
    }
    return frames;
}

// Morceaux que donneront les plans (list_cells), gardés tant que les plans et la pièce ne bougent pas.
// ponytail: refait à chaque image pendant qu'un plan bouge ; ne le refaire qu'au lâcher si de gros maillages font ramer
const std::vector<uint32_t> &GLGizmoAdvancedCut::plane_cells() const
{
    const ModelInstance *instance = selected_instance();
    std::vector<size_t>  families;
    uint32_t             flips  = 0;
    const auto           planes = cut_planes(plane_frames(), families, flips);
    if (instance == nullptr)
        m_cells_cache = CellsCache{};
    else if (planes != m_cells_cache.planes || instance->get_object()->id() != m_cells_cache.object || !instance->get_matrix().isApprox(m_cells_cache.instance)) {
        m_cells_cache.planes   = planes;
        m_cells_cache.object   = instance->get_object()->id();
        m_cells_cache.instance = instance->get_matrix();
        m_cells_cache.cells    = list_cells(*instance->get_object(), instance->get_matrix(), planes, families, flips);
    }
    return m_cells_cache.cells;
}

// Morceaux décochés dans « Après la coupe » ; aucun avec des connecteurs ou « Couper en parties » : tout est gardé, comme
// avec un seul plan, et les cases décochées reviennent sans eux
size_t GLGizmoAdvancedCut::dropped_pieces() const
{
    const ModelInstance *instance = selected_instance();
    if (!several_planes() || instance == nullptr)
        return 0;
    bool connectors = !instance->get_object()->cut_connectors.empty(); // ceux du plan actif
    for (int i = 0; i < int(m_multi.planes.size()); ++i)
        connectors = connectors || (i != m_multi.active && !m_multi.planes[i].connectors.empty());
    size_t count = 0;
    if (!connectors && !m_cut_to_parts)
        for (const uint32_t cell : plane_cells())
            count += size_t(std::count(m_multi.dropped.begin(), m_multi.dropped.end(), cell));
    return count;
}

void GLGizmoAdvancedCut::render_planes_row(float caption_size, float space_size)
{
    const float left = caption_size + 1 * space_size, pill = ImGui::GetFrameHeight(), tip_width = 20.0f * m_imgui->get_font_size();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Cut planes"));
    ImGui::SameLine(left);
    int clicked = -1;
    for (int i = 0; i < std::max(1, int(m_multi.planes.size())); ++i) {
        if (i % 8 != 0)
            ImGui::SameLine();
        else if (i > 0)
            ImGui::SetCursorPosX(left);
        const bool active  = i == m_multi.active;
        const bool invalid = m_cut_mode == CutMode::cutPlanar && !plane_crosses_piece(i);
        if (invalid) {
            ImVec4 red = ImGuiWrapper::COL_RED;
            if (!active)
                red.w = 0.45f; // le plan actif reste repérable
            ImGui::PushStyleColor(ImGuiCol_Button, red);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, red);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, red);
        } else if (active)
            ImGuiWrapper::push_confirm_button_style();
        if (m_imgui->button(from_u8(std::to_string(i + 1)), pill, pill))
            clicked = i;
        if (invalid)
            ImGui::PopStyleColor(3);
        else if (active)
            ImGuiWrapper::pop_confirm_button_style();
        if (invalid && ImGui::IsItemHovered()) // après les couleurs : le style actif écrit en blanc
            m_imgui->tooltip(format_wxstr(_L("Plane %1% does not cut the part."), i + 1), tip_width);
    }
    ImGui::SameLine();
    const bool can_add = m_cut_mode == CutMode::cutPlanar;
    m_imgui->disabled_begin(!can_add);
    const bool add = m_imgui->button("+", pill, pill);
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        m_imgui->tooltip(can_add ? _L("Add a cut plane") : _L("Available in Planar mode"), tip_width);
    bool remove = false;
    if (several_planes()) {
        ImGui::SameLine();
        if (m_delete_icon == nullptr) {
            const unsigned px = unsigned(std::max(16.f, 2.f * pill));
            if (!IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/delete.svg", px, px, m_delete_icon))
                m_delete_icon = nullptr;
        }
        const float padding = std::max(1.f, 0.12f * pill);
        remove = m_delete_icon != nullptr ?
            m_imgui->image_button(m_delete_icon, ImVec2(pill - 2.f * padding, pill - 2.f * padding), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), int(padding)) :
            m_imgui->button(_L("Delete"));
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Delete this cut plane"), tip_width);
    }
    if (add)
        add_plane();
    else if (remove)
        delete_active_plane();
    else if (clicked >= 0 && clicked != m_multi.active) {
        store_active_plane();
        load_plane(clicked);
    }
}

// Le plan 1 coupe la pièce, le plan 2 chaque morceau qu'il traverse, et ainsi de suite. La géométrie reste
// celle de Cut (peinture et réglages suivent). Les matrices restent valables d'un plan au suivant : Cut pose
// les morceaux dans le repère de l'instance sans décalage, et aucun placement n'a lieu pendant la chaîne.
void GLGizmoAdvancedCut::cut_by_planes(ModelObject &object, const PlaneList &list, int instance_idx, const Selection &selection, Model &result)
{
    // Un plan confondu avec un précédent ne coupe rien (avec « Couper en parties », il ferait des éclats)
    std::vector<Transform3d> matrices;
    std::vector<size_t>      plane_of; // plan de chaque matrice, pour ses connecteurs
    bool                     with_connectors = false;
    for (size_t i = 0; i < list.planes.size(); ++i) {
        with_connectors = with_connectors || !list.planes[i].connectors.empty();
        const SavedPlane &plane  = list.planes[i];
        const Vec3d       normal = plane.rotation.linear() * Vec3d::UnitZ();
        bool repeated = false;
        for (size_t j = 0; j < i && !repeated; ++j) {
            const SavedPlane &other = list.planes[j];
            repeated = std::abs(normal.dot(other.rotation.linear() * Vec3d::UnitZ())) > 1. - 1e-9 && std::abs((plane.center - other.center).dot(normal)) < 1e-3;
        }
        if (!repeated) {
            matrices.push_back(get_cut_matrix(selection, plane.center, plane.rotation));
            plane_of.push_back(i);
        }
    }
    const bool to_parts = m_cut_to_parts && !with_connectors; // comme perform_cut : les connecteurs l'emportent
    const auto to_plane = [instance_idx](const ModelObject &piece, const ModelVolume &volume, const Transform3d &cut_matrix) -> Transform3d {
        return cut_matrix.inverse() * piece.instances[instance_idx]->get_transformation().get_matrix_no_offset() * volume.get_matrix();
    };
    const auto crossed = [&to_plane](const ModelObject &piece, const Transform3d &cut_matrix) {
        BoundingBoxf3 box;
        for (const ModelVolume *volume : piece.volumes)
            if (volume->is_model_part())
                box.merge(volume->mesh().transformed_bounding_box(to_plane(piece, *volume, cut_matrix)));
        return box.defined && box.min.z() < -0.01 && box.max.z() > 0.01;
    };
    ModelObjectCutAttributes attributes = only_if(true, ModelObjectCutAttribute::KeepUpper) | only_if(true, ModelObjectCutAttribute::KeepLower) |
                                          only_if(to_parts, ModelObjectCutAttribute::CutToParts); // pas const : l'opérateur | d'enum_bitmask ne l'est pas
    result.add_object(object);
    result.objects.front()->cut_connectors.clear(); // ceux du plan actif : chaque plan pose les siens ci-dessous
    // Les connecteurs d'un plan vont au morceau qui contient leur centre, juste avant que ce plan le coupe.
    // ponytail: un connecteur à cheval sur un autre plan suit son centre, un connecteur hors de tout morceau
    // (ou d'un plan confondu avec un autre) est perdu, et seuls ceux du plan actif sont dessinés et vérifiés
    Model dowels; // chevilles, rangées après les morceaux
    for (size_t m = 0; m < matrices.size(); ++m) {
        const Transform3d &cut_matrix = matrices[m];
        const SavedPlane  &plane      = list.planes[plane_of[m]];
        const Vec3d        normal     = (plane.rotation.linear() * Vec3d::UnitZ()).normalized();
        Model next;
        for (ModelObject *piece : result.objects) {
            if (!to_parts && !crossed(*piece, cut_matrix)) {
                next.add_object(*piece);
                continue;
            }
            int dowels_count = 0;
            if (!plane.connectors.empty()) {
                // une partie à la fois, dans le repère des connecteurs (instance sans décalage) : le tenon d'un plan
                // précédent, posé sur la face de coupe, la doublerait et fausserait le rayon
                const Transform3d trafo = piece->instances[instance_idx]->get_transformation().get_matrix_no_offset();
                std::vector<bool> inside(plane.connectors.size(), false);
                for (const ModelVolume *volume : piece->volumes)
                    if (volume->is_model_part()) {
                        TriangleMesh mesh = volume->mesh();
                        mesh.transform(trafo * volume->get_matrix(), true);
                        const AABBMesh tree(mesh);
                        for (size_t c = 0; c < inside.size(); ++c)
                            inside[c] = inside[c] || tree.query_ray_hit(plane.connectors[c].pos, normal).is_inside();
                    }
                for (size_t c = 0; c < inside.size(); ++c)
                    if (inside[c])
                        piece->cut_connectors.push_back(plane.connectors[c]);
                piece->cut_id.copy(object.cut_id); // numérotation des connecteurs suivie d'un morceau à l'autre
                apply_connectors_in_model(piece, dowels_count, plane.rotation, normal);
                object.cut_id.copy(piece->cut_id);
            }
            Cut cut(piece, instance_idx, cut_matrix, attributes | only_if(dowels_count > 0, ModelObjectCutAttribute::CreateDowels));
            const ModelObjectPtrs &parts = cut.perform_with_plane();
            for (size_t i = 0; i < parts.size(); ++i) // Cut rend les chevilles en dernier, une par connecteur Cheville
                (i + size_t(dowels_count) < parts.size() ? next : dowels).add_object(*parts[i]);
        }
        result = std::move(next);
    }
    if (to_parts) { // un seul objet : un suffixe _A ou _B par coupe sur chaque partie
        const size_t suffix = 2 * matrices.size();
        std::map<std::string, int> counts;
        for (ModelVolume *volume : result.objects.front()->volumes)
            if (volume->is_model_part() && volume->name.size() >= suffix) {
                const std::string base = volume->name.substr(0, volume->name.size() - suffix);
                volume->name = base + "_" + std::to_string(++counts[base]);
            }
        return;
    }
    // numérotés comme les cases « Objet n » de l'outil (list_cells, mêmes plans, même instance)
    std::vector<std::pair<Vec3d, Vec3d>> frames;
    for (const SavedPlane &plane : list.planes)
        frames.emplace_back(plane.center, Vec3d(plane.rotation.linear() * Vec3d::UnitZ()));
    std::vector<size_t>         families;
    uint32_t                    flips  = 0;
    const auto                  planes = cut_planes(frames, families, flips);
    const std::vector<uint32_t> listed = list_cells(object, object.instances[instance_idx]->get_matrix(), planes, families, flips);
    std::vector<std::pair<size_t, ModelObject *>> order; // rang dans listed, morceau
    size_t extra = listed.size(); // morceau tout intérieur, sans case : après les autres
    for (ModelObject *piece : result.objects) {
        const size_t at = size_t(std::find(listed.begin(), listed.end(), piece_cell(*piece, instance_idx, planes)) - listed.begin());
        order.emplace_back(at < listed.size() ? at : extra++, piece);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (size_t i = 0; i < order.size(); ++i) {
        result.objects[i]     = order[i].second;
        order[i].second->name = object.name + "_" + std::to_string(order[i].first + 1);
    }
    // ceux qu'on y a décochés partent, sauf avec des connecteurs (comme avec un seul plan) ou s'il n'en restait aucun
    std::vector<ModelObject *> unchecked;
    for (const auto &[at, piece] : order)
        if (at < listed.size() && std::find(list.dropped.begin(), list.dropped.end(), listed[at]) != list.dropped.end())
            unchecked.push_back(piece);
    if (with_connectors || unchecked.size() == order.size())
        unchecked.clear();
    for (ModelObject *piece : unchecked)
        result.delete_object(piece);
    if (list.place_on_cut)
        for (ModelObject *piece : result.objects) {
            double      best = 0.;
            Transform3d lay  = Transform3d::Identity();
            for (const Transform3d &cut_matrix : matrices) {
                double faces_up = 0., faces_down = 0.; // face de coupe vers le haut : morceau sous le plan
                bool   pegged   = false;               // des tenons sortent de sa face du haut : pas posé dessus
                for (const ModelVolume *volume : piece->volumes) {
                    if (!volume->is_model_part())
                        continue;
                    const Transform3d trafo = to_plane(*piece, *volume, cut_matrix);
                    if (volume->cut_info.is_connector) { // tenon : connecteur devenu partie du morceau du dessous
                        pegged = pegged || std::abs(volume->mesh().transformed_bounding_box(trafo).min.z()) < 1e-3;
                        continue;
                    }
                    const indexed_triangle_set &its = volume->mesh().its;
                    for (const auto &face : its.indices) {
                        const Vec3d a = trafo * its.vertices[face[0]].cast<double>();
                        const Vec3d b = trafo * its.vertices[face[1]].cast<double>();
                        const Vec3d c = trafo * its.vertices[face[2]].cast<double>();
                        if (std::abs(a.z()) < 1e-3 && std::abs(b.z()) < 1e-3 && std::abs(c.z()) < 1e-3) {
                            const double area = (b - a).cross(c - a).z();
                            (area > 0. ? faces_up : faces_down) += std::abs(area);
                        }
                    }
                }
                const Transform3d inverse = Geometry::Transformation(cut_matrix).get_rotation_matrix().inverse();
                if (faces_down > best) {
                    best = faces_down;
                    lay  = inverse;
                }
                if (faces_up > best && !pegged) {
                    best = faces_up;
                    lay  = Geometry::rotation_transform(PI * Vec3d::UnitX()) * inverse;
                }
            }
            for (size_t i = 0; i < piece->instances.size(); ++i) {
                ModelInstance *instance = piece->instances[i];
                Transform3d    matrix   = lay;
                if (int(i) != instance_idx) // comme Cut : les autres instances gardent leur rotation Z
                    matrix = Geometry::rotation_transform(instance->get_rotation().z() * Vec3d::UnitZ()) * lay;
                instance->set_rotation(Geometry::Transformation(matrix).get_rotation());
            }
        }
    for (ModelObject *dowel : dowels.objects) { // nommées comme avec un seul plan
        dowel->name = object.name + "-Dowel-" + dowel->volumes.front()->name;
        result.add_object(*dowel);
    }
    // en rangée centrée sur la pièce, gardée dans sa plaque si elle y tient : partie du bord gauche de la pièce, elle
    // débordait du plateau (bandeau rouge au build du 10/11 12h57). En Z, load_model_objects les pose sur le plateau.
    const BoundingBoxf3 source = object.instance_bounding_box(instance_idx);
    double length = -5.;
    for (const ModelObject *piece : result.objects)
        length += piece->instance_bounding_box(instance_idx).size().x() + 5.;
    double         x      = source.center().x() - length / 2.;
    PartPlateList &plates = wxGetApp().plater()->get_partplate_list();
    if (PartPlate *plate = plates.get_plate(plates.find_instance_belongs(selection.get_object_idx(), instance_idx))) {
        const BoundingBoxf3 &bed = plate->get_bounding_box();
        if (length <= bed.size().x())
            x = std::clamp(x, bed.min.x(), bed.max.x() - length);
    }
    for (ModelObject *piece : result.objects) {
        const BoundingBoxf3 box = piece->instance_bounding_box(instance_idx);
        piece->translate_instances(Vec3d(x - box.min.x(), source.center().y() - box.center().y(), 0.));
        x += box.size().x() + 5.;
    }
    // update_object_cut_id a compté 2 morceaux, sauf si une case est décochée (même test que dropped_pieces dans perform_cut)
    const bool dropping = !with_connectors && std::any_of(listed.begin(), listed.end(), [&list](uint32_t cell) {
        return std::find(list.dropped.begin(), list.dropped.end(), cell) != list.dropped.end();
    });
    if (!dropping && object.cut_id.id().valid() && result.objects.size() > 2)
        object.cut_id.increase_check_sum(result.objects.size() - 2);
}

// Tourne le plan vers « rotation » : autour de l'origine choisie s'il y en a une, sinon autour de
// son centre ; le plan reste sur la pièce.
void GLGizmoAdvancedCut::rotate_plane_to(const Transform3d &rotation)
{
    if (m_origin_type == OriginType::Custom) {
        const Matrix3d relative    = rotation.linear() * m_rotate_matrix.linear().transpose();
        m_plane_center             = m_origin + relative * (m_plane_center - m_origin);
        m_transformed_bounding_box = transformed_bounding_box(m_plane_center, m_rotate_matrix);
    }
    update_plate_normal_boundingbox_clipper(rotation);
    check_and_update_connectors_state();
}

void GLGizmoAdvancedCut::set_origin_bottom()
{
    m_origin_type = OriginType::Bottom;
    m_origin_kind = PickKind::None;
    update_buffer_data();
}

void GLGizmoAdvancedCut::set_origin(const Vec3d &point, PickKind kind)
{
    m_origin_type = OriginType::Custom;
    m_origin      = point;
    m_origin_kind = kind;
    update_buffer_data();
}

// Place le plan : normale « normal », passant par « point » (ramené sur la pièce si besoin).
// « x_dir » oriente l'axe X local du plan (rainure, anneau de rotation X) ; sans lui, l'axe X
// actuel est gardé autant que possible. L'axe d'angle est oublié.
void GLGizmoAdvancedCut::apply_plane(const Vec3d &normal, const Vec3d &point, const Vec3d *x_dir)
{
    const Vec3d z = normal.normalized();
    Vec3d       x = x_dir != nullptr ? *x_dir : Vec3d(m_rotate_matrix.linear() * Vec3d::UnitX());
    x -= x.dot(z) * z;
    if (x.squaredNorm() < 1e-12) {
        x = std::abs(z.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
        x -= x.dot(z) * z;
    }
    x.normalize();
    Matrix3d axes;
    axes.col(0) = x;
    axes.col(1) = z.cross(x);
    axes.col(2) = z;
    Transform3d rotation = Transform3d::Identity();
    rotation.linear()    = axes;

    m_hinge.active             = false;
    m_rotate_matrix            = rotation;
    m_rotation                 = Geometry::extract_euler_angles(m_rotate_matrix);
    m_transformed_bounding_box = transformed_bounding_box(point, m_rotate_matrix);
    set_center(point);
    reset_cut_by_contours();
    m_parent.request_extra_frame();
}

// Axe d'angle : le plan contient l'axe ; à 0° sa normale est ref0, à 90° ref90.
void GLGizmoAdvancedCut::set_hinge(const Vec3d &point, const Vec3d &axis, const Vec3d &ref0, const Vec3d &ref90, double angle_deg)
{
    const double angle  = Geometry::deg2rad(angle_deg);
    const Vec3d  normal = (std::cos(angle) * ref0 + std::sin(angle) * ref90).normalized();
    apply_plane(normal, point, &axis);
    m_hinge.active = true;
    m_hinge.point  = point;
    m_hinge.axis   = axis.normalized();
    m_hinge.ref0   = ref0;
    m_hinge.ref90  = ref90;
    update_buffer_data();
}

// L'axe d'angle ne sert que tant que le plan contient sa direction (sinon le plan a été tourné
// autrement depuis).
bool GLGizmoAdvancedCut::hinge_usable() const
{
    return m_hinge.active && std::abs(current_plane_normal().dot(m_hinge.axis)) < 1e-4;
}

double GLGizmoAdvancedCut::get_hinge_angle() const
{
    const Vec3d normal = current_plane_normal();
    return Geometry::rad2deg(std::atan2(normal.dot(m_hinge.ref90), normal.dot(m_hinge.ref0)));
}

// Fait tourner le plan autour de l'axe d'angle ; sa distance à l'axe ne change pas.
void GLGizmoAdvancedCut::set_hinge_angle(double angle_deg)
{
    if (!hinge_usable())
        return;
    const double angle    = Geometry::deg2rad(angle_deg);
    const Vec3d  normal   = (std::cos(angle) * m_hinge.ref0 + std::sin(angle) * m_hinge.ref90).normalized();
    const double distance = (m_plane_center - m_hinge.point).dot(current_plane_normal());
    const Hinge  hinge    = m_hinge;
    apply_plane(normal, hinge.point + distance * normal, &hinge.axis);
    m_hinge = hinge;
    update_buffer_data();
}

// Ligne « Position » (curseur borné à la pièce + champ), plage possible, et « Angle » quand un
// axe d'angle est actif.
void GLGizmoAdvancedCut::render_position_input(float caption_size, float space_size, float unit_size, unsigned int current_active_id)
{
    const float wrap_width = 20.0f * m_imgui->get_font_size();
    double      lo, hi;
    get_position_range(lo, hi);
    const double position = get_position();

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Cut position") + " ");
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Distance of the plane from its origin, along its normal. Arrows Up / Down: 0.1 mm (Shift: 1 mm)."), wrap_width);
    ImGui::SameLine(caption_size + 1 * space_size);
    const float row_width    = 3 * unit_size + 2 * space_size;
    const float slider_width = 0.55f * row_width;
    ImGui::SetNextItemWidth(slider_width);
    float slider_value = float(position);
    if (m_imgui->bbl_slider_float_style("##cut_position_slider", &slider_value, float(lo), float(hi), "%.2f") &&
        std::abs(double(slider_value) - position) > EPSILON) {
        if (m_cut_mode == CutMode::cutTongueAndGroove)
            m_groove_editing = true; // la rainure est recalculée au relâchement
        set_position(slider_value);
        reset_cut_by_contours();
        m_position_slider_editing = true;
    }
    if (m_position_slider_editing && !ImGui::IsItemActive()) {
        m_position_slider_editing = false;
        m_groove_editing          = false;
        reset_cut_by_contours();
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Move cut plane");
        sync_archived_plane();
    }

    ImGui::SameLine(caption_size + 1 * space_size + slider_width + space_size);
    ImGui::SetNextItemWidth(row_width - slider_width - space_size);
    ImGui::BBLInputDouble("##cut_position", &m_buffered_position, 0.0f, 0.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        set_position(m_buffered_position);
        reset_cut_by_contours();
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Move cut plane");
        sync_archived_plane();
    }
    if (!ImGui::IsItemActive())
        m_buffered_position = get_position();

    // ↺ : plan remis comme à l'ouverture de l'outil (position, rotation, origine, axe d'angle) ;
    // les options de coupe et les connecteurs restent (« Réinitialiser » les remet aussi).
    // Affiché seulement quand le plan n'est plus comme à l'ouverture.
    const bool plane_changed = (m_plane_center - m_bb_center).norm() > EPSILON ||
                               !m_rotate_matrix.linear().isApprox(Matrix3d::Identity()) ||
                               m_origin_type == OriginType::Custom || m_hinge.active;
    if (plane_changed) {
        ImGui::SameLine();
        ImGui::PushID("cut_plane_reset");
        if (render_reset_button("cut_plane_reset", _u8L("Put the plane back as it was when the tool was opened (position, rotation, origin)."))) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset cut plane");
            reset_cut_plane();
        }
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(caption_size, 0.0f));
    ImGui::SameLine(caption_size + 1 * space_size);
    ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, format_wxstr(_L("from %1% to %2% mm"), format(float(lo), 2), format(float(hi), 2)));

    if (hinge_usable()) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Angle") + " ");
        ImGui::SameLine(caption_size + 1 * space_size);
        ImGui::SetNextItemWidth(row_width);
        ImGui::BBLInputDouble("##cut_hinge_angle", &m_buffered_hinge_angle, 0.0f, 0.0f, "%.2f");
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Turns the plane around the chosen edge or circle axis (degrees)."), wrap_width);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Turn cut plane around an axis");
            set_hinge_angle(m_buffered_hinge_angle);
            sync_archived_plane();
        }
        if (!ImGui::IsItemActive())
            m_buffered_hinge_angle = get_hinge_angle();
    }
}

// Origine de la position, menu des plans par ancrages ; pendant un choix sur la pièce, les
// consignes à la place.
void GLGizmoAdvancedCut::render_origin_and_anchors(float caption_size, float space_size, float unit_size)
{
    const float wrap_width = 20.0f * m_imgui->get_font_size();

    if (is_picking()) {
        m_imgui->text(m_pick_mode == PickMode::Origin ? _L("Choose the origin") : _L("Plane from anchors"));
        ImGui::Separator();
        m_imgui->text_wrapped(pick_instructions(), wrap_width);
        if (m_pick_hover.valid() && pick_accepts(m_pick_hover.kind))
            m_imgui->text(format_wxstr(_L("Under the mouse: %1%"), pick_kind_name(m_pick_hover.kind)));
        else
            m_imgui->text(" ");
        if (!m_pick_warning.empty())
            m_imgui->warning_text_wrapped(m_pick_warning, wrap_width);
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Right click: undo the last point. Esc: cancel."));
        if (m_imgui->button(_L("Cancel")))
            stop_picking();
        return;
    }

    // origine de la position (et pivot des rotations). Au début de la colonne des champs, juste à gauche
    // de la valeur, « Choisir sur la pièce » : le bouton à icône de curseur de « Sélectionner » des
    // Modificateurs (même image, même taille)
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Origin") + " ");
    ImGui::SameLine(caption_size + 1 * space_size);
    const float  icon_size = ImGui::GetFrameHeight();
    ImTextureID &icon      = m_is_dark_mode ? m_select_icon_dark : m_select_icon;
    if (icon == nullptr) {
        const unsigned px = unsigned(std::max(16.f, 2.f * icon_size));
        if (!IMTexture::load_from_svg_file(Slic3r::resources_dir() + (m_is_dark_mode ? "/images/modifier_select_dark.svg" : "/images/modifier_select.svg"), px, px, icon))
            icon = nullptr;
    }
    const float padding = std::max(1.f, 0.12f * icon_size);
    ImGui::PushID("origin_choose");
    const bool choose = icon != nullptr ?
                            m_imgui->image_button(icon, ImVec2(icon_size - 2.f * padding, icon_size - 2.f * padding), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), int(padding)) :
                            m_imgui->button(_L("Choose on the part"));
    ImGui::PopID();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Choose on the part") + "\n" + _L("The position of the plane is measured from this point, and the plane turns around it."), wrap_width);
    if (choose)
        start_picking(PickMode::Origin);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_origin_type == OriginType::Custom ? pick_kind_name(m_origin_kind) : _L("Bottom of the part"));
    // ↺ rouge : origine seule ramenée au bas de la pièce, le plan ne bouge pas (le ↺ de la ligne
    // Position, lui, remet tout le plan comme à l'ouverture). L'icône orange, teintée par la couleur du
    // texte, devient rouge.
    if (m_origin_type == OriginType::Custom) {
        ImGui::SameLine();
        ImGui::PushID("origin_bottom");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_RED);
        const bool bottom = render_reset_button("origin_bottom", _u8L("Measure again from the bottom of the part (the plane does not move)."));
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (bottom) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset cut plane origin");
            set_origin_bottom();
        }
    }

    // plans définis par des ancrages pris sur la pièce : un menu déroulant (une ligne au lieu de trois),
    // de la largeur du menu Mode ; choisir une ligne lance le choix sur la pièce
    struct AnchorPlane { PickMode mode; std::string label; std::string tooltip; };
    const AnchorPlane anchor_planes[] = {
        {PickMode::Face, _u8L("Face"), _u8L("Click a flat face: the plane lies on it.")},
        {PickMode::ThreePoints, _u8L("3 points"),
         _u8L("Click 3 points of the part (vertices, middles of edges, centers of circles, points of faces): the plane goes through them.")},
        {PickMode::EdgePoint, _u8L("Edge + point"), _u8L("Click an edge, then a point: the plane goes through both.")},
        {PickMode::EdgeAngle, _u8L("Edge + angle"), _u8L("Click an edge, on the side of a face: the plane turns around this edge, from this face (Angle field).")},
        {PickMode::Circle, _u8L("Circle"), _u8L("Click a circle (edge of a hole, of a cylinder): the plane lies on it.")},
        {PickMode::CircleAxis, _u8L("Circle axis"), _u8L("Click a circle: the plane contains its axis and turns around it (Angle field).")},
    };
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Plane from anchors") + " ");
    ImGui::SameLine(caption_size + 1 * space_size);
    ImGui::SetNextItemWidth(4 * unit_size + 0.5f * space_size);
    const std::string *hovered_tooltip = nullptr;
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    if (ImGui::BBLBeginCombo("##plane_from_anchors", _u8L("Choose...").c_str(), 0)) {
        for (const AnchorPlane &plane : anchor_planes) {
            if (ImGui::Selectable(plane.label.c_str(), false))
                start_picking(plane.mode);
            if (ImGui::IsItemHovered())
                hovered_tooltip = &plane.tooltip;
        }
        ImGui::EndCombo();
    }
    ImGuiWrapper::pop_combo_style();
    // infobulle montrée hors du style du menu : son fond clair rendrait illisible le texte blanc
    if (hovered_tooltip != nullptr)
        m_imgui->tooltip(*hovered_tooltip, wrap_width);
}

void GLGizmoAdvancedCut::start_picking(PickMode mode)
{
    if (mode == PickMode::None) {
        stop_picking();
        return;
    }
    m_pick_mode  = mode;
    m_pick_hover = PickHover();
    m_picked.clear();
    m_pick_warning.clear();
    if (m_pick_volumes.empty())
        init_pick_volumes();
    // pièce montrée telle quelle, sans les couleurs de part et d'autre du plan
    m_parent.set_use_color_clip_plane(false);
    m_parent.request_extra_frame();
}

void GLGizmoAdvancedCut::stop_picking()
{
    m_pick_mode  = PickMode::None;
    m_pick_hover = PickHover();
    m_picked.clear();
    m_pick_warning.clear();
    if (m_state == On) {
        m_parent.set_use_color_clip_plane(true);
        update_clipper();
    }
    m_parent.request_extra_frame();
}

// Maillages de la pièce pour le choix d'ancrages : un lanceur de rayons et un découpage en
// faces planes (moteur de l'outil Mesure) par partie pleine.
void GLGizmoAdvancedCut::init_pick_volumes()
{
    m_pick_volumes.clear();
    if (m_c == nullptr || m_c->selection_info() == nullptr)
        return;
    const ModelObject *mo       = m_c->selection_info()->model_object();
    const int          inst_idx = m_c->selection_info()->get_active_instance();
    if (mo == nullptr || inst_idx < 0 || inst_idx >= int(mo->instances.size()))
        return;

    wxBusyCursor      wait;
    const Transform3d instance_trafo = Geometry::translation_transform(double(m_c->selection_info()->get_sla_shift()) * Vec3d::UnitZ()) *
                                       mo->instances[inst_idx]->get_matrix();
    for (const ModelVolume *volume : mo->volumes) {
        if (!volume->is_model_part())
            continue;
        PickVolume pick_volume;
        pick_volume.mesh      = &volume->mesh();
        pick_volume.trafo     = instance_trafo * volume->get_matrix();
        pick_volume.raycaster = std::make_unique<MeshRaycaster>(volume->mesh());
        pick_volume.measuring = std::make_unique<Measure::Measuring>(volume->mesh().its);
        m_pick_volumes.emplace_back(std::move(pick_volume));
    }
}

// Taille d'un pixel de l'écran, en mm, à la profondeur de « point ».
double GLGizmoAdvancedCut::pixel_size_at(const Vec3d &point) const
{
    const Camera &camera  = wxGetApp().plater()->get_camera();
    const Vec2d   mouse   = m_parent.get_local_mouse_position();
    const Vec3d   forward = camera.get_dir_forward();
    auto          at_depth = [&camera, &point, &forward](const Vec2d &screen) {
        Vec3d origin, direction;
        MeshRaycaster::line_from_mouse_pos_static(screen, Transform3d::Identity(), camera, origin, direction);
        const double den = direction.dot(forward);
        return std::abs(den) < 1e-12 ? origin : Vec3d(origin + direction * ((point - origin).dot(forward) / den));
    };
    return (at_depth(mouse + Vec2d(1.0, 0.0)) - at_depth(mouse)).norm();
}

// Ancrage sous la souris. Le triangle survolé donne sa face plane (découpage de l'outil Mesure) ;
// les arêtes et cercles de cette face attirent la souris à 10 pixels près, par ordre de
// priorité : sommet, centre de cercle / polygone, milieu d'arête, point d'arête, point de face.
void GLGizmoAdvancedCut::update_pick_hover()
{
    m_pick_hover = PickHover();
    if (m_pick_volumes.empty())
        return;

    const Camera &camera = wxGetApp().plater()->get_camera();
    const Vec2d   mouse  = m_parent.get_local_mouse_position();

    int    hit_volume = -1;
    size_t hit_facet  = 0;
    Vec3f  hit_local  = Vec3f::Zero();
    double hit_dist   = std::numeric_limits<double>::max();
    for (size_t i = 0; i < m_pick_volumes.size(); ++i) {
        const PickVolume &pick_volume = m_pick_volumes[i];
        Vec3f             position, normal;
        size_t            facet = 0;
        if (pick_volume.raycaster->unproject_on_mesh(mouse, pick_volume.trafo, camera, position, normal, nullptr, &facet, false)) {
            const double dist = (camera.get_position() - pick_volume.trafo * position.cast<double>()).squaredNorm();
            if (dist < hit_dist) {
                hit_dist   = dist;
                hit_volume = int(i);
                hit_facet  = facet;
                hit_local  = position;
            }
        }
    }
    if (hit_volume < 0)
        return;

    const PickVolume                            &pick_volume   = m_pick_volumes[hit_volume];
    const std::optional<Measure::SurfaceFeature> plane_feature = pick_volume.measuring->get_feature(hit_facet, hit_local.cast<double>(), Transform3d::Identity(), true);
    if (!plane_feature.has_value() || plane_feature->get_type() != Measure::SurfaceFeatureType::Plane)
        return;

    const Transform3d &trafo         = pick_volume.trafo;
    const Matrix3d     normal_matrix = trafo.linear().inverse().transpose();
    auto               to_world      = [&trafo](const Vec3d &p) { return Vec3d(trafo * p); };
    auto               dir_to_world  = [&normal_matrix](const Vec3d &n) { return Vec3d((normal_matrix * n).normalized()); };

    const Vec3d  hit       = to_world(hit_local.cast<double>());
    const double tolerance = std::max(0.5, 10.0 * pixel_size_at(hit));

    PickHover face;
    face.volume       = hit_volume;
    face.plane        = std::get<0>(plane_feature->get_plane());
    face.face_normal  = dir_to_world(std::get<1>(plane_feature->get_plane()));
    face.point        = hit;
    const auto &tri   = pick_volume.mesh->its.indices[hit_facet];
    const auto &verts = pick_volume.mesh->its.vertices;
    face.facet_center = to_world((verts[tri[0]] + verts[tri[1]] + verts[tri[2]]).cast<double>() / 3.0);

    if (m_pick_mode == PickMode::Face) {
        face.kind    = PickKind::Face;
        m_pick_hover = face;
        return;
    }

    const bool want_points  = m_pick_mode == PickMode::Origin || m_pick_mode == PickMode::ThreePoints ||
                             (m_pick_mode == PickMode::EdgePoint && !m_picked.empty());
    const bool want_edges   = m_pick_mode == PickMode::EdgeAngle || (m_pick_mode == PickMode::EdgePoint && m_picked.empty());
    const bool want_circles = m_pick_mode == PickMode::Circle || m_pick_mode == PickMode::CircleAxis;

    PickHover best;
    int       best_rank = std::numeric_limits<int>::max();
    double    best_dist = std::numeric_limits<double>::max();
    auto      consider  = [&](const PickHover &candidate, PickKind kind, int rank, double dist, const Vec3d &point) {
        if (dist > tolerance || rank > best_rank || (rank == best_rank && dist >= best_dist))
            return;
        best       = candidate;
        best.kind  = kind;
        best.point = point;
        best_rank  = rank;
        best_dist  = dist;
    };

    for (const Measure::SurfaceFeature &feature : pick_volume.measuring->get_plane_features(face.plane)) {
        if (feature.get_type() == Measure::SurfaceFeatureType::Edge) {
            const auto [a_local, b_local] = feature.get_edge();
            PickHover edge                = face;
            edge.edge_a                   = to_world(a_local);
            edge.edge_b                   = to_world(b_local);
            const Vec3d  ab               = edge.edge_b - edge.edge_a;
            const double len2             = ab.squaredNorm();
            if (len2 < 1e-12)
                continue;
            const Vec3d  on_edge   = edge.edge_a + std::clamp((hit - edge.edge_a).dot(ab) / len2, 0.0, 1.0) * ab;
            const double edge_dist = (hit - on_edge).norm();
            const Vec3d  middle    = 0.5 * (edge.edge_a + edge.edge_b);
            if (want_edges)
                consider(edge, PickKind::Edge, 0, edge_dist, middle);
            if (want_points) {
                consider(edge, PickKind::Vertex, 0, (hit - edge.edge_a).norm(), edge.edge_a);
                consider(edge, PickKind::Vertex, 0, (hit - edge.edge_b).norm(), edge.edge_b);
                consider(edge, PickKind::EdgeMid, 2, (hit - middle).norm(), middle);
                consider(edge, PickKind::EdgePoint, 3, edge_dist, on_edge);
            }
            // côté d'un polygone régulier de 5 à 8 côtés : son centre
            if (const std::optional<Vec3d> &center_local = feature.get_extra_point(); center_local.has_value()) {
                PickHover polygon     = face;
                polygon.circle_center = to_world(*center_local);
                polygon.circle_normal = face.face_normal;
                polygon.circle_radius = (edge.edge_a - polygon.circle_center).norm();
                const double center_dist = (hit - polygon.circle_center).norm();
                if (want_points)
                    consider(polygon, PickKind::PolygonCenter, 1, center_dist, polygon.circle_center);
                if (want_circles)
                    consider(polygon, PickKind::Circle, 0, std::min(edge_dist, center_dist), polygon.circle_center);
            }
        } else if (feature.get_type() == Measure::SurfaceFeatureType::Circle) {
            const auto [center_local, radius_local, normal_local] = feature.get_circle();
            PickHover circle                                      = face;
            circle.circle_center                                  = to_world(center_local);
            circle.circle_normal                                  = dir_to_world(normal_local);
            const Vec3d side = normal_local.cross(std::abs(normal_local.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
            circle.circle_radius = (to_world(center_local + radius_local * side) - circle.circle_center).norm();
            const Vec3d  v        = hit - circle.circle_center;
            const double height   = v.dot(circle.circle_normal);
            const double rim_dist = std::hypot((v - height * circle.circle_normal).norm() - circle.circle_radius, height);
            const double dist     = std::min(rim_dist, v.norm());
            if (want_points)
                consider(circle, PickKind::CircleCenter, 1, dist, circle.circle_center);
            if (want_circles)
                consider(circle, PickKind::Circle, 0, dist, circle.circle_center);
        }
    }

    if (best.valid())
        m_pick_hover = best;
    else if (want_points) {
        face.kind    = PickKind::FacePoint;
        m_pick_hover = face;
    }
}

bool GLGizmoAdvancedCut::pick_accepts(PickKind kind) const
{
    const bool is_point = kind == PickKind::Vertex || kind == PickKind::EdgeMid || kind == PickKind::EdgePoint || kind == PickKind::CircleCenter ||
                          kind == PickKind::PolygonCenter || kind == PickKind::FacePoint;
    switch (m_pick_mode) {
    case PickMode::Origin:
    case PickMode::ThreePoints: return is_point;
    case PickMode::EdgePoint: return m_picked.empty() ? kind == PickKind::Edge : is_point;
    case PickMode::EdgeAngle: return kind == PickKind::Edge;
    case PickMode::Face: return kind == PickKind::Face;
    case PickMode::Circle:
    case PickMode::CircleAxis: return kind == PickKind::Circle;
    default: return false;
    }
}

// Clic sur l'ancrage survolé : il est retenu, et la construction s'applique dès qu'elle est
// complète. Chaque construction prend son instantané d'annulation AVANT de modifier le plan et
// l'origine (voir on_save), puis aligne les valeurs archivées.
// Les plans par ancrages ne touchent PAS à l'origine (demande de l'utilisateur, tests du 04/10
// 16h39) : seul « Choisir sur la pièce » la change ; la Position se mesure toujours depuis
// l'origine en cours (bas de la pièce par défaut).
void GLGizmoAdvancedCut::on_pick_click()
{
    const PickHover hover = m_pick_hover;
    if (!hover.valid() || !pick_accepts(hover.kind))
        return;
    m_pick_warning.clear();

    // normale orientée comme celle du plan actuel : les côtés A / B restent où ils étaient
    const Vec3d current_normal = current_plane_normal();
    auto        keep_side      = [&current_normal](const Vec3d &normal) { return normal.dot(current_normal) < 0.0 ? Vec3d(-normal) : normal; };
    // les points choisis ne doivent pas être alignés (sinus de l'angle > 0,001)
    auto        not_aligned    = [](const Vec3d &u, const Vec3d &v) { return u.cross(v).norm() > 1e-3 * std::max(u.norm() * v.norm(), 1e-9); };

    switch (m_pick_mode) {
    case PickMode::Origin: {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Set cut plane origin");
        set_origin(hover.point, hover.kind);
        set_position(0.0); // le plan passe par l'origine choisie (Position 0,00 mm)
        reset_cut_by_contours();
        sync_archived_plane();
        stop_picking();
        break;
    }
    case PickMode::Face: {
        // plan posé sur la face, normale vers la matière
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Cut plane parallel to a face");
        apply_plane(-hover.face_normal, hover.point, nullptr);
        sync_archived_plane();
        stop_picking();
        break;
    }
    case PickMode::Circle: {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Cut plane on a circle");
        apply_plane(-hover.circle_normal, hover.circle_center, nullptr);
        sync_archived_plane();
        stop_picking();
        break;
    }
    case PickMode::CircleAxis: {
        // plan contenant l'axe du cercle, tourné vers la caméra à 0°
        const Vec3d axis    = hover.circle_normal.normalized();
        const Vec3d forward = wxGetApp().plater()->get_camera().get_dir_forward();
        Vec3d       ref0    = -forward + forward.dot(axis) * axis;
        if (ref0.squaredNorm() < 1e-8)
            ref0 = axis.cross(std::abs(axis.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY());
        ref0.normalize();
        const Vec3d ref90 = axis.cross(ref0).normalized();
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Cut plane through a circle axis");
        set_hinge(hover.circle_center, axis, ref0, ref90, 0.0);
        sync_archived_plane();
        stop_picking();
        break;
    }
    case PickMode::EdgeAngle: {
        // à 0° le plan est sur la face survolée (normale vers la matière) ; les angles positifs
        // l'inclinent vers l'intérieur de cette face, donc dans la pièce
        const Vec3d axis = (hover.edge_b - hover.edge_a).normalized();
        Vec3d       ref0 = -hover.face_normal;
        ref0 -= ref0.dot(axis) * axis;
        ref0.normalize();
        Vec3d inward = hover.face_normal.cross(axis);
        if (inward.dot(hover.facet_center - hover.edge_a) < 0.0)
            inward = -inward;
        Vec3d ref90 = -inward;
        ref90 -= ref90.dot(axis) * axis + ref90.dot(ref0) * ref0;
        ref90.normalize();
        const Vec3d middle = 0.5 * (hover.edge_a + hover.edge_b);
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Cut plane around an edge");
        set_hinge(middle, axis, ref0, ref90, 45.0);
        sync_archived_plane();
        stop_picking();
        break;
    }
    case PickMode::ThreePoints: {
        m_picked.push_back(hover);
        if (m_picked.size() < 3)
            break;
        const Vec3d a = m_picked[0].point, b = m_picked[1].point, c = m_picked[2].point;
        if (!not_aligned(b - a, c - a)) {
            m_picked.pop_back();
            m_pick_warning = _L("These points are aligned: choose another third point.");
            break;
        }
        const Vec3d x_dir = b - a;
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Cut plane through 3 points");
        apply_plane(keep_side((b - a).cross(c - a)), a, &x_dir);
        sync_archived_plane();
        stop_picking();
        break;
    }
    case PickMode::EdgePoint: {
        m_picked.push_back(hover);
        if (m_picked.size() < 2)
            break;
        const Vec3d a = m_picked[0].edge_a, b = m_picked[0].edge_b, p = m_picked[1].point;
        if (!not_aligned(b - a, p - a)) {
            m_picked.pop_back();
            m_pick_warning = _L("This point is on the line of the edge: choose a point outside of it.");
            break;
        }
        const Vec3d middle = 0.5 * (a + b), x_dir = b - a;
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Cut plane through an edge and a point");
        apply_plane(keep_side((b - a).cross(p - a)), middle, &x_dir);
        sync_archived_plane();
        stop_picking();
        break;
    }
    default: break;
    }
}

void GLGizmoAdvancedCut::render_pick_sphere(const Vec3d &center, double radius, const ColorRGBA &color)
{
    if (!m_pick_sphere.is_initialized())
        m_pick_sphere.init_from(smooth_sphere(16, 1.0f));
    const Camera &camera = wxGetApp().plater()->get_camera();
    render_glmodel(m_pick_sphere, color.get_data(), camera.get_view_matrix() * Geometry::translation_transform(center) * Geometry::scale_transform(radius),
                   camera.get_projection_matrix(), false, 0.25f);
}

void GLGizmoAdvancedCut::render_pick_edge(const Vec3d &a, const Vec3d &b, double radius, const ColorRGBA &color)
{
    const Vec3d ab = b - a;
    if (ab.squaredNorm() < 1e-12)
        return;
    if (!m_pick_cylinder.is_initialized())
        m_pick_cylinder.init_from(smooth_cylinder(16, 1.0f, 1.0f));
    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Transform3d matrix = Geometry::translation_transform(a) * Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), ab) *
                               Geometry::scale_transform(Vec3d(radius, radius, ab.norm()));
    render_glmodel(m_pick_cylinder, color.get_data(), camera.get_view_matrix() * matrix, camera.get_projection_matrix(), false, 0.25f);
}

void GLGizmoAdvancedCut::render_pick_circle(const Vec3d &center, double radius, const Vec3d &normal, double thickness, const ColorRGBA &color)
{
    const std::array<double, 8> key{center.x(), center.y(), center.z(), radius, normal.x(), normal.y(), normal.z(), thickness};
    if (!m_pick_torus || key != m_pick_torus_key) {
        m_pick_torus     = init_torus_data(64, 16, center.cast<float>(), float(radius), float(thickness), normal.cast<float>(), Transform3f::Identity());
        m_pick_torus_key = key;
    }
    if (!m_pick_torus)
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();
    render_glmodel(*m_pick_torus, color.get_data(), camera.get_view_matrix(), camera.get_projection_matrix(), false, 0.25f);
}

// Repères 3D : pendant un choix, l'ancrage survolé (vert) et ceux déjà choisis (orange), reliés
// en triangle ; sinon l'origine choisie (orange), toujours visible comme les poignées.
void GLGizmoAdvancedCut::render_pick_markers()
{
    const bool picking = is_picking();
    if (!picking && m_origin_type != OriginType::Custom)
        return;

    const GLboolean depth_test = ::glIsEnabled(GL_DEPTH_TEST);
    if (picking)
        glsafe(::glEnable(GL_DEPTH_TEST));
    else
        glsafe(::glDisable(GL_DEPTH_TEST));

    if (!picking)
        render_pick_sphere(m_origin, 6.0 * pixel_size_at(m_origin), CUT_PICK_CHOSEN_COLOR);
    else {
        auto render_anchor = [this](const PickHover &anchor, const ColorRGBA &color) {
            const double px = pixel_size_at(anchor.point);
            switch (anchor.kind) {
            case PickKind::Face:
                if (anchor.volume >= 0 && anchor.volume < int(m_pick_volumes.size())) {
                    const PickVolume &pick_volume = m_pick_volumes[anchor.volume];
                    const std::pair<int, int> key{anchor.volume, anchor.plane};
                    if (!m_pick_face_model || key != m_pick_face_key) {
                        m_pick_face_model = init_plane_data(pick_volume.mesh->its, pick_volume.measuring->get_plane_triangle_indices(anchor.plane), 0.05f);
                        m_pick_face_key   = key;
                    }
                    if (m_pick_face_model) {
                        const Camera &camera = wxGetApp().plater()->get_camera();
                        render_glmodel(*m_pick_face_model, color.get_data(), camera.get_view_matrix() * pick_volume.trafo, camera.get_projection_matrix(), false, 0.25f);
                    }
                }
                break;
            case PickKind::Edge:
                render_pick_edge(anchor.edge_a, anchor.edge_b, 2.5 * px, color);
                break;
            case PickKind::Circle:
                render_pick_circle(anchor.circle_center, anchor.circle_radius, anchor.circle_normal, 2.5 * px, color);
                render_pick_sphere(anchor.circle_center, 4.0 * px, color);
                break;
            default:
                // un point ; l'arête ou le cercle d'où il vient, en fin
                if (anchor.kind == PickKind::Vertex || anchor.kind == PickKind::EdgeMid || anchor.kind == PickKind::EdgePoint)
                    render_pick_edge(anchor.edge_a, anchor.edge_b, 1.5 * px, color);
                render_pick_sphere(anchor.point, 6.0 * px, color);
                break;
            }
        };

        for (const PickHover &picked : m_picked)
            render_anchor(picked, CUT_PICK_CHOSEN_COLOR);
        const bool hover_ok = m_pick_hover.valid() && pick_accepts(m_pick_hover.kind);
        if (hover_ok)
            render_anchor(m_pick_hover, CUT_PICK_HOVER_COLOR);

        // aperçu du plan en construction : triangle des points (3 points), ou arête + point
        std::vector<Vec3d> outline;
        if (m_pick_mode == PickMode::ThreePoints) {
            for (const PickHover &picked : m_picked)
                outline.push_back(picked.point);
            if (hover_ok)
                outline.push_back(m_pick_hover.point);
        } else if (m_pick_mode == PickMode::EdgePoint && !m_picked.empty() && hover_ok) {
            outline = {m_picked.front().edge_a, m_pick_hover.point, m_picked.front().edge_b};
        }
        for (size_t i = 1; i < outline.size(); ++i)
            render_pick_edge(outline[i - 1], outline[i], 1.0 * pixel_size_at(outline[i]), CUT_PICK_CHOSEN_COLOR);
        if (outline.size() == 3)
            render_pick_edge(outline[2], outline[0], 1.0 * pixel_size_at(outline[0]), CUT_PICK_CHOSEN_COLOR);
    }

    if (depth_test)
        glsafe(::glEnable(GL_DEPTH_TEST));
    else
        glsafe(::glDisable(GL_DEPTH_TEST));
}

wxString GLGizmoAdvancedCut::pick_kind_name(PickKind kind) const
{
    switch (kind) {
    case PickKind::Vertex: return _L("Vertex of the part");
    case PickKind::EdgeMid: return _L("Middle of an edge");
    case PickKind::EdgePoint: return _L("Point on an edge");
    case PickKind::CircleCenter: return _L("Center of a circle");
    case PickKind::PolygonCenter: return _L("Center of a polygon");
    case PickKind::FacePoint: return _L("Point on a face");
    case PickKind::Edge: return _L("Edge of the part");
    case PickKind::Circle: return _L("Circle");
    case PickKind::Face: return _L("Face");
    default: return _L("Bottom of the part");
    }
}

wxString GLGizmoAdvancedCut::pick_instructions() const
{
    switch (m_pick_mode) {
    case PickMode::Origin: return _L("Click the new origin on the part: vertex, middle of edge, center of circle or point of a face.");
    case PickMode::Face: return _L("Click a flat face of the part.");
    case PickMode::ThreePoints:
        return format_wxstr(_L("Click point %1% of 3: vertex, middle of edge, center of circle or point of a face."), int(m_picked.size()) + 1);
    case PickMode::EdgePoint: return m_picked.empty() ? _L("Click an edge of the part.") : _L("Click a point outside of this edge.");
    case PickMode::EdgeAngle: return _L("Click an edge, on the side of the face the angle starts from.");
    case PickMode::Circle:
    case PickMode::CircleAxis: return _L("Click a circle of the part (edge of a hole, of a cylinder).");
    default: return {};
    }
}

// =============================================================================================
// Orio : connecteurs précis — sections de la face coupée et leurs ancrages (magnétisme),
// coordonnées du connecteur, motifs, symétrie, filetage ISO
// =============================================================================================

// Fractions (numérateur, dénominateur) proposées comme ancrages : grille longueur × largeur du
// rectangle de la section, et subdivisions plus fines le long de ses médianes et de ses diagonales.
static const Vec2i SECTION_GRID_FRACTIONS[] = {{1, 4}, {1, 3}, {1, 2}, {2, 3}, {3, 4}};
static const Vec2i SECTION_LINE_FRACTIONS[] = {{1, 10}, {1, 8}, {1, 6}, {1, 5}, {1, 4}, {3, 10}, {1, 3}, {3, 8}, {2, 5}, {1, 2},
                                               {3, 5},  {5, 8}, {2, 3}, {7, 10}, {3, 4}, {4, 5}, {5, 6}, {7, 8}, {9, 10}};

// Les ancrages restent dans la face, à au moins cette distance du bord (demande de l'utilisateur).
static const double SECTION_ANCHOR_MARGIN = 5.0;

static double      fraction_value(const Vec2i &f) { return double(f.x()) / double(f.y()); }
static std::string fraction_text(const Vec2i &f) { return std::to_string(f.x()) + "/" + std::to_string(f.y()); }

static const ColorRGBA CUT_ANCHOR_COLOR       = {0.62f, 0.62f, 0.62f, 1.0f}; // ancrages de la section
static const ColorRGBA CUT_SECTION_AXIS_COLOR = {0.25f, 0.65f, 1.0f, 1.0f};  // médianes de la section (longueur / largeur)
static const ColorRGBA CUT_SECTION_DIAGONAL_COLOR = {0.25f, 0.65f, 1.0f, 0.5f}; // diagonales de la section
static const ColorRGBA CUT_PATTERN_COLOR      = {0.25f, 0.65f, 1.0f, 0.55f}; // connecteurs du motif, avant ajout

// Mur minimal conseillé autour d'un connecteur (environ deux parois).
float GLGizmoAdvancedCut::connector_wall_margin() const { return 1.2f; }

Vec2d GLGizmoAdvancedCut::world_to_plane2d(const Vec3d &world) const
{
    const Vec3d local = m_rotate_matrix.linear().transpose() * (world - m_plane_center);
    return {local.x(), local.y()};
}

Vec3d GLGizmoAdvancedCut::plane2d_to_world(const Vec2d &point) const
{
    return m_plane_center + m_rotate_matrix.linear() * Vec3d(point.x(), point.y(), 0.);
}

// Position d'un connecteur (CutConnector::pos) : le monde moins le décalage de l'instance et le
// soulèvement SLA, comme unproject_on_cut_plane.
Vec3d GLGizmoAdvancedCut::world_to_connector_pos(const Vec3d &world) const
{
    const ModelObject *mo  = m_c->selection_info()->model_object();
    Vec3d              pos = world - mo->instances[m_c->selection_info()->get_active_instance()]->get_offset();
    pos.z() -= double(m_c->selection_info()->get_sla_shift());
    return pos;
}

Vec3d GLGizmoAdvancedCut::connector_pos_to_world(const Vec3d &pos) const
{
    const ModelObject *mo    = m_c->selection_info()->model_object();
    Vec3d              world = pos + mo->instances[m_c->selection_info()->get_active_instance()]->get_offset();
    world.z() += double(m_c->selection_info()->get_sla_shift());
    return world;
}

// Sections de la face coupée, recalculées quand le plan ou la pièce changent : tranche exacte des
// parties pleines moins les volumes négatifs, dans le repère du plan ; pour chaque section, son
// repère propre (barycentre ; longueur et largeur = côtés du plus petit rectangle englobant), le
// point le plus éloigné des bords, les centres des trous, puis les ancrages.
void GLGizmoAdvancedCut::update_sections()
{
    if (m_c == nullptr || m_c->selection_info() == nullptr || m_c->selection_info()->model_object() == nullptr)
        return;
    const ModelObject *mo       = m_c->selection_info()->model_object();
    const int          inst_idx = m_c->selection_info()->get_active_instance();
    if (inst_idx < 0 || inst_idx >= int(mo->instances.size()))
        return;
    const Transform3d instance_trafo = Geometry::translation_transform(double(m_c->selection_info()->get_sla_shift()) * Vec3d::UnitZ()) *
                                       mo->instances[inst_idx]->get_matrix();
    const Transform3d plane_trafo    = Geometry::translation_transform(m_plane_center) * m_rotate_matrix;

    std::vector<double> key;
    auto push_matrix = [&key](const Transform3d &t) {
        const double *d = t.matrix().data();
        key.insert(key.end(), d, d + 16);
    };
    push_matrix(plane_trafo);
    push_matrix(instance_trafo);
    for (const ModelVolume *volume : mo->volumes) {
        push_matrix(volume->get_matrix());
        key.push_back(double(int(volume->type())));
        key.push_back(double(reinterpret_cast<std::uintptr_t>(volume->mesh_ptr())));
    }
    if (key == m_sections_key)
        return;
    m_sections_key = std::move(key);
    m_sections.clear();
    m_section_anchors.clear();
    m_hover_anchor = -1;
    m_hover_island = -1;
    m_pattern_key.clear();

    const Transform3d to_plane = plane_trafo.inverse();
    Polygons          solid, negative;
    for (const ModelVolume *volume : mo->volumes) {
        if (!volume->is_model_part() && !volume->is_negative_volume())
            continue;
        MeshSlicingParams params;
        params.trafo   = to_plane * instance_trafo * volume->get_matrix();
        Polygons polys = slice_mesh(volume->mesh().its, 0.f, params);
        append(volume->is_model_part() ? solid : negative, std::move(polys));
    }
    ExPolygons expolys = negative.empty() ? union_ex(solid) : diff_ex(union_(solid), negative);

    for (const ExPolygon &expoly : expolys) {
        SectionIsland island;
        island.expoly = expoly;
        auto to_mm    = [](const Polygon &polygon) {
            std::vector<Vec2d> ring;
            ring.reserve(polygon.points.size());
            for (const Point &p : polygon.points)
                ring.emplace_back(unscale(p));
            return ring;
        };
        island.rings.push_back(to_mm(expoly.contour));
        for (const Polygon &hole : expoly.holes)
            island.rings.push_back(to_mm(hole));

        // aire et barycentre (contour direct, trous indirects : ils se soustraient)
        double a = 0., cx = 0., cy = 0.;
        for (const std::vector<Vec2d> &ring : island.rings)
            for (size_t i = 0; i < ring.size(); ++i) {
                const Vec2d &p = ring[i], &q = ring[(i + 1) % ring.size()];
                const double cross = p.x() * q.y() - q.x() * p.y();
                a += cross;
                cx += (p.x() + q.x()) * cross;
                cy += (p.y() + q.y()) * cross;
            }
        a *= 0.5;
        if (std::abs(a) < 1e-4)
            continue;
        island.area     = std::abs(a);
        island.centroid = Vec2d(cx / (6. * a), cy / (6. * a));

        // centres des trous (barycentre de chacun)
        for (size_t r = 1; r < island.rings.size(); ++r) {
            const std::vector<Vec2d> &ring = island.rings[r];
            double                    ha = 0., hx = 0., hy = 0.;
            for (size_t i = 0; i < ring.size(); ++i) {
                const Vec2d &p = ring[i], &q = ring[(i + 1) % ring.size()];
                const double cross = p.x() * q.y() - q.x() * p.y();
                ha += cross;
                hx += (p.x() + q.x()) * cross;
                hy += (p.y() + q.y()) * cross;
            }
            if (std::abs(ha) > 1e-8)
                island.hole_centers.emplace_back(hx / (3. * ha), hy / (3. * ha));
        }

        // repère propre : côtés du plus petit rectangle englobant (enveloppe convexe, un essai par
        // côté). À aire presque égale (cercle, polygone régulier), le plus proche de l'axe X du plan.
        std::vector<Vec2d> hull;
        const Polygon      hull_polygon = Geometry::convex_hull(expoly.contour.points);
        for (const Point &p : hull_polygon.points)
            hull.emplace_back(unscale(p));
        if (hull.size() < 3)
            continue;
        auto box_along = [&hull](const Vec2d &u, Vec2d &lo, Vec2d &hi) {
            const Vec2d v(-u.y(), u.x());
            lo = Vec2d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
            hi = -lo;
            for (const Vec2d &p : hull) {
                const Vec2d q(p.dot(u), p.dot(v));
                lo = lo.cwiseMin(q);
                hi = hi.cwiseMax(q);
            }
        };
        std::vector<std::pair<double, double>> candidates; // (angle dans [-45°, 45°), aire)
        auto add_candidate = [&](double angle) {
            angle = std::fmod(angle, 0.5 * PI);
            if (angle < -0.25 * PI)
                angle += 0.5 * PI;
            if (angle >= 0.25 * PI)
                angle -= 0.5 * PI;
            Vec2d lo, hi;
            box_along(Vec2d(std::cos(angle), std::sin(angle)), lo, hi);
            candidates.emplace_back(angle, (hi - lo).prod());
        };
        add_candidate(0.);
        for (size_t i = 0; i < hull.size(); ++i) {
            const Vec2d edge = hull[(i + 1) % hull.size()] - hull[i];
            if (edge.squaredNorm() > 1e-12)
                add_candidate(std::atan2(edge.y(), edge.x()));
        }
        double min_area = std::numeric_limits<double>::max();
        for (const auto &c : candidates)
            min_area = std::min(min_area, c.second);
        double best_angle = 0., best_abs = std::numeric_limits<double>::max();
        for (const auto &c : candidates)
            if (c.second <= min_area * 1.005 && std::abs(c.first) < best_abs) {
                best_abs   = std::abs(c.first);
                best_angle = c.first;
            }
        Vec2d u(std::cos(best_angle), std::sin(best_angle)), lo, hi;
        box_along(u, lo, hi);
        if (hi.y() - lo.y() > (hi.x() - lo.x()) * 1.0001) // la longueur suit le plus grand côté
            u = Vec2d(-u.y(), u.x());
        island.axis_u = u;
        island.axis_v = Vec2d(-u.y(), u.x());
        island.box_min = Vec2d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
        island.box_max = -island.box_min;
        for (const Vec2d &p : hull) {
            const Vec2d q((p - island.centroid).dot(island.axis_u), (p - island.centroid).dot(island.axis_v));
            island.box_min = island.box_min.cwiseMin(q);
            island.box_max = island.box_max.cwiseMax(q);
        }
        m_sections.push_back(std::move(island));
    }
    // la plus grande section d'abord (« Section 1 » dans les motifs)
    std::sort(m_sections.begin(), m_sections.end(), [](const SectionIsland &a, const SectionIsland &b) { return a.area > b.area; });

    for (int idx = 0; idx < int(m_sections.size()); ++idx) {
        SectionIsland &island = m_sections[idx];

        // point le plus éloigné des bords (pôle d'inaccessibilité, recherche par cellules)
        {
            struct Cell { Vec2d c; double h, d, max; };
            auto make_cell = [this, idx](const Vec2d &c, double h) {
                const double d = section_edge_distance(idx, c);
                return Cell{c, h, d, d + h * std::sqrt(2.)};
            };
            Vec2d lo = island.rings.front().front(), hi = lo;
            for (const Vec2d &p : island.rings.front()) {
                lo = lo.cwiseMin(p);
                hi = hi.cwiseMax(p);
            }
            const double cell_size = std::min(hi.x() - lo.x(), hi.y() - lo.y());
            Cell         best      = make_cell(island.centroid, 0.);
            if (cell_size > 0.) {
                auto cmp = [](const Cell &a, const Cell &b) { return a.max < b.max; };
                std::priority_queue<Cell, std::vector<Cell>, decltype(cmp)> queue(cmp);
                for (double x = lo.x(); x < hi.x(); x += cell_size)
                    for (double y = lo.y(); y < hi.y(); y += cell_size)
                        queue.push(make_cell(Vec2d(x + 0.5 * cell_size, y + 0.5 * cell_size), 0.5 * cell_size));
                const Cell box_cell = make_cell(0.5 * (lo + hi), 0.);
                if (box_cell.d > best.d)
                    best = box_cell;
                const double precision = std::max(0.01, 0.001 * std::max(hi.x() - lo.x(), hi.y() - lo.y()));
                for (int iter = 0; !queue.empty() && iter < 20000; ++iter) {
                    const Cell cell = queue.top();
                    queue.pop();
                    if (cell.d > best.d)
                        best = cell;
                    if (cell.max - best.d <= precision)
                        continue;
                    const double h = 0.5 * cell.h;
                    queue.push(make_cell(cell.c + Vec2d(-h, -h), h));
                    queue.push(make_cell(cell.c + Vec2d(h, -h), h));
                    queue.push(make_cell(cell.c + Vec2d(-h, h), h));
                    queue.push(make_cell(cell.c + Vec2d(h, h), h));
                }
            }
            island.pole        = best.c;
            island.pole_radius = std::max(0., best.d);
        }

        // ancrages, sans doublon (moins de 0,05 mm d'écart), toujours dans la face à au moins
        // SECTION_ANCHOR_MARGIN du bord ; les centres des trous servent seulement de centres de motif
        // et de points de symétrie
        auto add_anchor = [this, idx](const Vec2d &p, AnchorKind kind, const Vec2i &frac_a, const Vec2i &frac_b) {
            if (kind != AnchorKind::HoleCenter && section_edge_distance(idx, p) < SECTION_ANCHOR_MARGIN - 1e-3)
                return;
            for (const SectionAnchor &a : m_section_anchors)
                if ((a.pos - p).squaredNorm() < 0.05 * 0.05)
                    return;
            m_section_anchors.push_back({p, kind, idx, frac_a, frac_b});
        };
        const Vec2i none(0, 1);
        // point du rectangle de la section : fraction fu de la longueur, fv de la largeur
        auto at_box = [&island](double fu, double fv) {
            const double u = island.box_min.x() + fu * (island.box_max.x() - island.box_min.x());
            const double v = island.box_min.y() + fv * (island.box_max.y() - island.box_min.y());
            return Vec2d(island.centroid + u * island.axis_u + v * island.axis_v);
        };
        // sommets où un contour tourne d'au moins 30° (coins), et milieux des côtés entre deux coins
        // (mesurés le long du contour)
        auto corner_indices = [](const std::vector<Vec2d> &pts) {
            std::vector<size_t> out;
            const size_t        n = pts.size();
            for (size_t i = 0; n >= 3 && i < n; ++i) {
                const Vec2d d0 = pts[i] - pts[(i + n - 1) % n], d1 = pts[(i + 1) % n] - pts[i];
                if (d0.squaredNorm() > 0. && d1.squaredNorm() > 0. &&
                    std::acos(std::clamp(d0.normalized().dot(d1.normalized()), -1., 1.)) >= Geometry::deg2rad(30.))
                    out.push_back(i);
            }
            return out;
        };
        auto side_middles = [](const std::vector<Vec2d> &pts, const std::vector<size_t> &corners) {
            std::vector<Vec2d> out;
            const size_t       n = pts.size();
            for (size_t k = 0; corners.size() >= 2 && k < corners.size(); ++k) {
                const size_t i0 = corners[k], i1 = corners[(k + 1) % corners.size()];
                double       length = 0.;
                for (size_t i = i0; i != i1; i = (i + 1) % n)
                    length += (pts[(i + 1) % n] - pts[i]).norm();
                double half = 0.5 * length;
                for (size_t i = i0; i != i1; i = (i + 1) % n) {
                    const Vec2d &a = pts[i], &b = pts[(i + 1) % n];
                    const double l = (b - a).norm();
                    if (half <= l) {
                        out.push_back(l > 0. ? Vec2d(a + (b - a) * (half / l)) : a);
                        break;
                    }
                    half -= l;
                }
            }
            return out;
        };
        auto to_points_mm = [](const Polygon &polygon) {
            std::vector<Vec2d> pts;
            for (const Point &q : polygon.points)
                pts.emplace_back(unscale(q));
            return pts;
        };
        // droite (c, d) bornée au contour de la section
        auto clip_line = [&island](const Vec2d &c, const Vec2d &d, Vec2d &a, Vec2d &b) {
            double                    t0 = std::numeric_limits<double>::max(), t1 = -t0;
            const std::vector<Vec2d> &ring = island.rings.front();
            for (size_t i = 0; i < ring.size(); ++i) {
                const Vec2d  e   = ring[(i + 1) % ring.size()] - ring[i];
                const Vec2d  w   = ring[i] - c;
                const double den = d.x() * e.y() - d.y() * e.x();
                if (std::abs(den) < 1e-12)
                    continue;
                const double t = (w.x() * e.y() - w.y() * e.x()) / den;
                const double u = (w.x() * d.y() - w.y() * d.x()) / den;
                if (u < 0. || u > 1.)
                    continue;
                t0 = std::min(t0, t);
                t1 = std::max(t1, t);
            }
            if (t0 >= t1)
                return false;
            a = c + t0 * d;
            b = c + t1 * d;
            return true;
        };

        // foyers, quand le contour est une ellipse : ellipse de mêmes moments d'ordre 2 que le
        // contour plein (demi-axes 2·√λ), gardée si chaque sommet du contour en est à moins de 8 %
        bool  ellipse       = false; // ellipse non circulaire
        Vec2d ellipse_major = Vec2d::UnitX();
        {
            const std::vector<Vec2d> &ring = island.rings.front();
            double                    a2 = 0., cx = 0., cy = 0.;
            for (size_t i = 0; i < ring.size(); ++i) {
                const Vec2d &p = ring[i], &q = ring[(i + 1) % ring.size()];
                const double cross = p.x() * q.y() - q.x() * p.y();
                a2 += cross;
                cx += (p.x() + q.x()) * cross;
                cy += (p.y() + q.y()) * cross;
            }
            if (std::abs(a2) > 1e-6 && ring.size() >= 8) {
                const Vec2d c0(cx / (3. * a2), cy / (3. * a2));
                double      sxx = 0., syy = 0., sxy = 0.;
                for (size_t i = 0; i < ring.size(); ++i) {
                    const Vec2d  p = ring[i] - c0, q = ring[(i + 1) % ring.size()] - c0;
                    const double cross = p.x() * q.y() - q.x() * p.y();
                    sxx += (p.x() * p.x() + p.x() * q.x() + q.x() * q.x()) * cross;
                    syy += (p.y() * p.y() + p.y() * q.y() + q.y() * q.y()) * cross;
                    sxy += (p.x() * q.y() + 2. * p.x() * p.y() + 2. * q.x() * q.y() + q.x() * p.y()) * cross;
                }
                const double cxx = sxx / (6. * a2), cyy = syy / (6. * a2), cxy = sxy / (12. * a2);
                const double mean = 0.5 * (cxx + cyy), diff = std::sqrt(0.25 * (cxx - cyy) * (cxx - cyy) + cxy * cxy);
                const double l1 = mean + diff, l2 = mean - diff;
                if (l2 > 0.) {
                    const double semi_a = 2. * std::sqrt(l1), semi_b = 2. * std::sqrt(l2);
                    const double theta  = 0.5 * std::atan2(2. * cxy, cxx - cyy);
                    const Vec2d  axis_major(std::cos(theta), std::sin(theta)), axis_minor(-axis_major.y(), axis_major.x());
                    bool         is_ellipse = true;
                    for (const Vec2d &p : ring) {
                        const Vec2d  d = p - c0;
                        const double r = std::hypot(d.dot(axis_major) / semi_a, d.dot(axis_minor) / semi_b);
                        if (std::abs(r - 1.) > 0.08) {
                            is_ellipse = false;
                            break;
                        }
                    }
                    const double focal = 2. * std::sqrt(std::max(0., l1 - l2));
                    if (is_ellipse && focal > std::max(0.1, 0.02 * semi_a)) {
                        ellipse       = true;
                        ellipse_major = axis_major;
                        add_anchor(c0 + focal * axis_major, AnchorKind::Focus, none, none);
                        add_anchor(c0 - focal * axis_major, AnchorKind::Focus, none, none);
                    }
                }
            }
        }
        // section elliptique : son repère propre suit les axes de l'ellipse (le plus petit rectangle
        // englobant d'un polygone elliptique peut tourner de quelques degrés), pour des médianes,
        // diagonales et grilles symétriques ; ses bouts, très courbés, ne sont pas des coins
        if (ellipse) {
            island.axis_u  = ellipse_major;
            island.axis_v  = Vec2d(-ellipse_major.y(), ellipse_major.x());
            island.box_min = Vec2d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
            island.box_max = -island.box_min;
            for (const Vec2d &p : island.rings.front()) {
                const Vec2d q((p - island.centroid).dot(island.axis_u), (p - island.centroid).dot(island.axis_v));
                island.box_min = island.box_min.cwiseMin(q);
                island.box_max = island.box_max.cwiseMax(q);
            }
        }

        // contour simplifié (défauts du maillage ignorés) : coins et côtés de la section
        const double size   = std::max((island.box_max - island.box_min).norm(), 1e-3);
        Polygon      simple = island.expoly.contour;
        simple.douglas_peucker(scale_(std::max(0.05, 0.01 * size)));
        const std::vector<Vec2d>  outline       = to_points_mm(simple);
        const std::vector<size_t> outline_idx   = ellipse ? std::vector<size_t>() : corner_indices(outline);
        std::vector<Vec2d>        corners;
        for (size_t i : outline_idx)
            corners.push_back(outline[i]);
        const std::vector<Vec2d> sides = side_middles(outline, outline_idx);
        // aire du contour seul (trous non retirés) contre celle du rectangle : à 95 % au moins, la
        // section est rectangulaire et garde les médianes / diagonales du rectangle
        double contour_area = 0.;
        for (size_t i = 0; i < outline.size(); ++i)
            contour_area += 0.5 * (outline[i].x() * outline[(i + 1) % outline.size()].y() - outline[(i + 1) % outline.size()].x() * outline[i].y());
        island.polygonal = corners.size() >= 3 && std::abs(contour_area) < 0.95 * (island.box_max - island.box_min).prod();

        add_anchor(island.centroid, AnchorKind::Centroid, none, none);
        if (island.pole_radius > 0.)
            add_anchor(island.pole, AnchorKind::Pole, none, none);
        if (!island.polygonal)
            add_anchor(at_box(0.5, 0.5), AnchorKind::BoxCenter, none, none);

        // coins et milieux des côtés, à SECTION_ANCHOR_MARGIN des bords : sommets et milieux des côtés
        // du contour simplifié rentré de cette distance (coins en onglet)
        if (!corners.empty())
            for (const Polygon &inner : offset(simple, -float(scale_(SECTION_ANCHOR_MARGIN)), ClipperLib::jtMiter, 10.)) {
                const std::vector<Vec2d>  pts = to_points_mm(inner);
                const std::vector<size_t> idx_inner = corner_indices(pts);
                for (size_t i : idx_inner)
                    add_anchor(pts[i], AnchorKind::Corner, none, none);
                for (const Vec2d &m : side_middles(pts, idx_inner))
                    add_anchor(m, AnchorKind::SideMiddle, none, none);
            }
        for (const Vec2d &c : island.hole_centers)
            add_anchor(c, AnchorKind::HoleCenter, none, none);

        // axes de symétrie (sections à coins) : droites par le barycentre vers un coin, vers le milieu
        // d'un côté, ou le long de la longueur / largeur, qui renvoient le contour sur lui-même
        auto has_axis = [&island](const Vec2d &d) {
            for (const SectionGuide &g : island.guides) {
                const Vec2d e = (g.b - g.a).normalized();
                if (std::abs(d.x() * e.y() - d.y() * e.x()) < 0.01)
                    return true;
            }
            return false;
        };
        if (!corners.empty() && corners.size() <= 24) {
            std::vector<Vec2d> dirs = {island.axis_u, island.axis_v};
            for (const Vec2d &c : corners)
                dirs.push_back(c - island.centroid);
            for (const Vec2d &m : sides)
                dirs.push_back(m - island.centroid);
            size_t total = 0;
            for (const std::vector<Vec2d> &ring : island.rings)
                total += ring.size();
            const size_t       step = std::max<size_t>(1, total / 240);
            std::vector<Vec2d> samples;
            size_t             k = 0;
            for (const std::vector<Vec2d> &ring : island.rings)
                for (const Vec2d &p : ring)
                    if (k++ % step == 0)
                        samples.push_back(p);
            const double tol = std::max(0.1, 0.01 * size);
            for (Vec2d d : dirs) {
                if (d.squaredNorm() < 1e-12)
                    continue;
                d.normalize();
                if (has_axis(d))
                    continue;
                bool symmetric = true;
                for (const Vec2d &p : samples) {
                    const Vec2d r = p - island.centroid;
                    const Vec2d q = island.centroid + 2. * d.dot(r) * d - r;
                    if (std::abs(section_edge_distance(idx, q)) > tol) {
                        symmetric = false;
                        break;
                    }
                }
                Vec2d a, b;
                if (symmetric && clip_line(island.centroid, d, a, b))
                    island.guides.push_back({a, b, GuideKind::SymmetryAxis});
            }
        }
        if (ellipse)
            for (const Vec2d &d : {island.axis_u, island.axis_v}) {
                Vec2d a, b;
                if (clip_line(island.centroid, d, a, b))
                    island.guides.push_back({a, b, GuideKind::SymmetryAxis});
            }
        // axes de la longueur et de la largeur par le barycentre (s'ils ne sont pas déjà des axes de symétrie)
        for (int k = 0; k < 2; ++k) {
            const Vec2d d = k == 0 ? island.axis_u : island.axis_v;
            Vec2d       a, b;
            if (!has_axis(d) && clip_line(island.centroid, d, a, b))
                island.guides.push_back({a, b, k == 0 ? GuideKind::LengthAxis : GuideKind::WidthAxis});
        }

        if (island.polygonal) {
            // rayons du barycentre (ou du point le plus éloigné des bords s'il tombe dans un vide)
            // vers chaque coin et chaque milieu de côté, gardés s'ils ne traversent pas de vide ;
            // ancrages aux quarts, tiers, moitié, deux tiers et trois quarts
            const Vec2d hub        = section_edge_distance(idx, island.centroid) > 0. ? island.centroid : island.pole;
            auto        add_ray    = [&](const Vec2d &end, GuideKind guide_kind, AnchorKind anchor_kind) {
                for (int k = 1; k < 24; ++k)
                    if (section_edge_distance(idx, hub + (end - hub) * (double(k) / 24.)) <= 0.)
                        return;
                island.guides.push_back({hub, end, guide_kind});
                for (const Vec2i &f : SECTION_GRID_FRACTIONS)
                    add_anchor(hub + (end - hub) * fraction_value(f), anchor_kind, f, none);
            };
            for (const Vec2d &c : corners)
                add_ray(c, GuideKind::CornerRay, AnchorKind::CornerRay);
            for (const Vec2d &m : sides)
                add_ray(m, GuideKind::SideRay, AnchorKind::SideRay);
        } else {
            // sections rondes ou rectangulaires : médianes et diagonales du rectangle, subdivisées,
            // puis grille des quarts, tiers et moitiés de la longueur × largeur
            // (droites bornées au contour : une diagonale ne sort pas d'un cercle ou d'une ellipse)
            const Vec2d mid       = at_box(0.5, 0.5);
            auto        add_guide = [&](const Vec2d &p0, const Vec2d &p1, GuideKind kind) {
                Vec2d a, b;
                if ((p1 - p0).squaredNorm() > 1e-12 && clip_line(mid, (p1 - p0).normalized(), a, b))
                    island.guides.push_back({a, b, kind});
            };
            add_guide(at_box(0., 0.5), at_box(1., 0.5), GuideKind::LengthMedian);
            add_guide(at_box(0.5, 0.), at_box(0.5, 1.), GuideKind::WidthMedian);
            add_guide(at_box(0., 0.), at_box(1., 1.), GuideKind::Diagonal);
            add_guide(at_box(0., 1.), at_box(1., 0.), GuideKind::Diagonal);
            for (const Vec2i &f : SECTION_LINE_FRACTIONS) {
                const double t = fraction_value(f);
                add_anchor(at_box(t, 0.5), AnchorKind::LengthMedian, f, none);
                add_anchor(at_box(0.5, t), AnchorKind::WidthMedian, f, none);
            }
            for (const Vec2i &f : SECTION_LINE_FRACTIONS) {
                const double t = fraction_value(f);
                add_anchor(at_box(t, t), AnchorKind::Diagonal, f, none);
                add_anchor(at_box(t, 1. - t), AnchorKind::Diagonal, f, none);
            }
            for (const Vec2i &fu : SECTION_GRID_FRACTIONS)
                for (const Vec2i &fv : SECTION_GRID_FRACTIONS)
                    add_anchor(at_box(fraction_value(fu), fraction_value(fv)), AnchorKind::Fraction, fu, fv);
        }

        // section trop fine pour garder SECTION_ANCHOR_MARGIN : seul le point le plus éloigné des bords
        if (island.pole_radius > 0. && std::none_of(m_section_anchors.begin(), m_section_anchors.end(), [idx](const SectionAnchor &a) {
                return a.island == idx && a.kind != AnchorKind::HoleCenter;
            }))
            m_section_anchors.push_back({island.pole, AnchorKind::Pole, idx, none, none});
    }
}

wxString GLGizmoAdvancedCut::guide_name(const SectionGuide &guide) const
{
    switch (guide.kind) {
    case GuideKind::SymmetryAxis: return _L("Symmetry axis of the section");
    case GuideKind::LengthAxis: return _L("Length axis");
    case GuideKind::WidthAxis: return _L("Width axis");
    case GuideKind::CornerRay: return _L("Line through the centroid and a corner");
    case GuideKind::SideRay: return _L("Line through the centroid and the middle of a side");
    case GuideKind::LengthMedian: return _L("Length median");
    case GuideKind::WidthMedian: return _L("Width median");
    default: return _L("Diagonal");
    }
}

int GLGizmoAdvancedCut::section_at(const Vec2d &point) const
{
    const Point p = Point::new_scale(point);
    for (int i = 0; i < int(m_sections.size()); ++i)
        if (m_sections[i].expoly.contains(p))
            return i;
    return -1;
}

// Section (face coupée) d'un point du plan ; hors de toute section (bord, trou, connecteur sorti de
// la face), la plus proche. -1 s'il n'y a pas de section.
int GLGizmoAdvancedCut::section_of(const Vec2d &point) const
{
    int best = section_at(point);
    if (best >= 0)
        return best;
    double best_dist = -std::numeric_limits<double>::max();
    for (int i = 0; i < int(m_sections.size()); ++i)
        if (const double dist = section_edge_distance(i, point); dist > best_dist) { // négative hors de la section
            best      = i;
            best_dist = dist;
        }
    return best;
}

int GLGizmoAdvancedCut::connector_section(const CutConnector &connector) const
{
    return section_of(world_to_plane2d(connector_pos_to_world(connector.pos)));
}

// Un filetage se visse en tournant : un seul par face coupée (une pièce en Y en a deux). Rend
// l'indice du filetage déjà posé sur la section « island » (le connecteur « skip » mis à part), ou -1.
int GLGizmoAdvancedCut::thread_on_section(const CutConnectors &connectors, int island, int skip) const
{
    for (int i = 0; i < int(connectors.size()); ++i)
        if (i != skip && connectors[i].attribs.type == CutConnectorType::Thread && connector_section(connectors[i]) == island)
            return i;
    return -1;
}

// Distance signée d'un point au bord d'une section (positive dans la matière).
double GLGizmoAdvancedCut::section_edge_distance(int island, const Vec2d &point) const
{
    if (island < 0 || island >= int(m_sections.size()))
        return -1.;
    const SectionIsland &isl = m_sections[island];
    double               d2  = std::numeric_limits<double>::max();
    for (const std::vector<Vec2d> &ring : isl.rings)
        for (size_t i = 0; i < ring.size(); ++i) {
            const Vec2d &a = ring[i], &b = ring[(i + 1) % ring.size()];
            const Vec2d  ab = b - a;
            const double l2 = ab.squaredNorm();
            const double t  = l2 > 0. ? std::clamp((point - a).dot(ab) / l2, 0., 1.) : 0.;
            d2              = std::min(d2, (point - (a + t * ab)).squaredNorm());
        }
    const double d = std::sqrt(d2);
    return isl.expoly.contains(Point::new_scale(point)) ? d : -d;
}

// Magnétisme des ancrages : aimante un point de la face coupée sur l'ancrage le plus proche, à 10
// pixels près ; ailleurs le point reste libre. Rend vrai si le point a été aimanté.
bool GLGizmoAdvancedCut::snap_to_section(Vec3d &world, bool update_hover, bool allow_holes)
{
    update_sections();
    const Vec2d p = world_to_plane2d(world);
    if (update_hover) {
        m_hover_island = section_at(p);
        m_hover_anchor = -1;
    }
    if (!m_auto_center_connector || m_section_anchors.empty())
        return false;
    const double tolerance = std::max(0.3, 10.0 * pixel_size_at(world));
    int          best      = -1;
    int          best_rank = std::numeric_limits<int>::max();
    double       best_dist = std::numeric_limits<double>::max();
    for (int i = 0; i < int(m_section_anchors.size()); ++i) {
        const SectionAnchor &anchor = m_section_anchors[i];
        if (anchor.kind == AnchorKind::HoleCenter && !allow_holes) // pas de matière : jamais un connecteur
            continue;
        const double         dist   = (anchor.pos - p).norm();
        const int            rank   = anchor.kind >= AnchorKind::Fraction ? 1 : 0; // subdivisions après les points remarquables
        if (dist > tolerance || rank > best_rank || (rank == best_rank && dist >= best_dist))
            continue;
        best      = i;
        best_rank = rank;
        best_dist = dist;
    }
    if (update_hover)
        m_hover_anchor = best;
    if (best < 0)
        return false;
    world = plane2d_to_world(m_section_anchors[best].pos);
    return true;
}

wxString GLGizmoAdvancedCut::anchor_name(const SectionAnchor &anchor) const
{
    switch (anchor.kind) {
    case AnchorKind::Centroid: return _L("Centroid of the section");
    case AnchorKind::Pole: return _L("Farthest point from the edges");
    case AnchorKind::BoxCenter: return _L("Center of the section box");
    case AnchorKind::HoleCenter: return _L("Center of a hole");
    case AnchorKind::Focus: return _L("Focus of the ellipse");
    case AnchorKind::Corner: return format_wxstr(_L("Corner, %1% mm from the edges"), format(float(SECTION_ANCHOR_MARGIN), 0));
    case AnchorKind::SideMiddle: return format_wxstr(_L("Middle of a side, %1% mm from the edge"), format(float(SECTION_ANCHOR_MARGIN), 0));
    case AnchorKind::CornerRay: return format_wxstr(_L("%1% of the ray to a corner"), fraction_text(anchor.frac_a));
    case AnchorKind::SideRay: return format_wxstr(_L("%1% of the ray to the middle of a side"), fraction_text(anchor.frac_a));
    case AnchorKind::LengthMedian: return format_wxstr(_L("%1% of the length median"), fraction_text(anchor.frac_a));
    case AnchorKind::WidthMedian: return format_wxstr(_L("%1% of the width median"), fraction_text(anchor.frac_a));
    case AnchorKind::Diagonal: return format_wxstr(_L("%1% of a diagonal"), fraction_text(anchor.frac_a));
    default:
        return format_wxstr(_L("%1% of the length, %2% of the width"), fraction_text(anchor.frac_a), fraction_text(anchor.frac_b));
    }
}

CutConnectorParas GLGizmoAdvancedCut::current_connector_paras() const
{
    CutConnectorParas paras;
    paras.snap_space_proportion = m_snap_space_proportion;
    paras.snap_bulge_proportion = m_snap_bulge_proportion;
    paras.thread_designation    = int(m_thread_designation);
    paras.thread_class          = int(m_thread_class);
    paras.thread_left_hand      = m_thread_hand == 1;
    return paras;
}

// Ajoute des connecteurs (réglages en cours) aux points donnés du plan, et ne sélectionne qu'eux.
void GLGizmoAdvancedCut::add_connectors_at(const std::vector<Vec2d> &points)
{
    if (points.empty())
        return;
    CutConnectors &connectors = m_c->selection_info()->model_object()->cut_connectors;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Add connectors");
    unselect_all_connectors();
    if (m_selected.size() != connectors.size())
        m_selected.assign(connectors.size(), false);
    const bool  thread = m_connector_type == CutConnectorType::Thread;
    const float radius = thread ? float(0.5 * ISO_METRIC_THREADS[m_thread_designation].size) : 0.5f * std::max(m_connector_size, 0.1f);
    float       height = std::max(m_connector_depth_ratio, 0.1f);
    if (thread)
        height = std::max(height, float(2. * ISO_METRIC_THREADS[m_thread_designation].pitch));
    for (const Vec2d &p : points) {
        connectors.emplace_back(world_to_connector_pos(plane2d_to_world(p)), m_rotate_matrix, radius, height, std::max(m_connector_size_tolerance, 0.f),
                                std::max(m_connector_depth_ratio_tolerance, 0.f),
                                CutConnectorAttributes(CutConnectorType(m_connector_type), CutConnectorStyle(m_connector_style), CutConnectorShape(m_connector_shape_id)));
        connectors.back().paras = current_connector_paras();
        m_selected.push_back(true);
    }
    m_selected_count = int(points.size());
    check_and_update_connectors_state();
    m_parent.set_as_dirty();
}

// Repères dessinés sur la face coupée : ancrages (gris, celui sous la souris en vert), médianes et
// diagonales de la section survolée, points A / B du motif « entre deux points ». Les subdivisions
// (médianes, diagonales, grille) ne sont montrées que sur la section survolée.
void GLGizmoAdvancedCut::render_section_markers()
{
    if (!m_connectors_editing)
        return;
    update_sections();
    const GLboolean depth_test = ::glIsEnabled(GL_DEPTH_TEST);
    glsafe(::glDisable(GL_DEPTH_TEST));

    if (m_mirror_picking) {
        // symétrie : toutes les droites guides (axes de symétrie de la section plus marqués) et
        // les ancrages ; l'axe ou le point survolé en vert
        for (int i = 0; i < int(m_sections.size()); ++i)
            for (int j = 0; j < int(m_sections[i].guides.size()); ++j) {
                const SectionGuide &g  = m_sections[i].guides[j];
                const double        px = pixel_size_at(plane2d_to_world(0.5 * (g.a + g.b)));
                if (i == m_mirror_hover_island && j == m_mirror_hover_guide)
                    render_pick_edge(plane2d_to_world(g.a), plane2d_to_world(g.b), 1.6 * px, CUT_PICK_HOVER_COLOR);
                else
                    render_pick_edge(plane2d_to_world(g.a), plane2d_to_world(g.b), (g.kind == GuideKind::SymmetryAxis ? 1.0 : 0.6) * px,
                                     g.kind == GuideKind::SymmetryAxis ? CUT_SECTION_AXIS_COLOR : CUT_SECTION_DIAGONAL_COLOR);
            }
        for (int i = 0; i < int(m_section_anchors.size()); ++i) {
            const SectionAnchor &anchor = m_section_anchors[i];
            if (anchor.kind >= AnchorKind::Fraction && i != m_mirror_hover_anchor && anchor.island != m_hover_island)
                continue;
            const Vec3d  p  = plane2d_to_world(anchor.pos);
            const double px = pixel_size_at(p);
            render_pick_sphere(p, (i == m_mirror_hover_anchor ? 5.0 : 3.0) * px, i == m_mirror_hover_anchor ? CUT_PICK_HOVER_COLOR : CUT_ANCHOR_COLOR);
        }
        if (m_mirror_has_first) {
            // premier point, et l'axe qu'il forme avec le point sous la souris
            const Vec3d  w  = plane2d_to_world(m_mirror_first);
            const double px = pixel_size_at(w);
            render_pick_sphere(w, 4.0 * px, CUT_PICK_CHOSEN_COLOR);
            if ((m_mirror_hover_point - m_mirror_first).squaredNorm() > 1e-6)
                render_pick_edge(w, plane2d_to_world(m_mirror_hover_point), 1.2 * px, CUT_PICK_HOVER_COLOR);
        }
    } else if (m_auto_center_connector) {
        // section survolée : ses droites guides (sans les axes de la longueur / largeur, réservés à
        // la symétrie) et ses subdivisions ; les centres des trous seulement pour un centre de motif
        if (m_hover_island >= 0 && m_hover_island < int(m_sections.size())) {
            const SectionIsland &isl = m_sections[m_hover_island];
            const double         px  = pixel_size_at(plane2d_to_world(isl.centroid));
            for (const SectionGuide &g : isl.guides)
                if (g.kind != GuideKind::LengthAxis && g.kind != GuideKind::WidthAxis)
                    render_pick_edge(plane2d_to_world(g.a), plane2d_to_world(g.b), (g.kind == GuideKind::Diagonal ? 0.5 : 0.8) * px,
                                     g.kind == GuideKind::Diagonal ? CUT_SECTION_DIAGONAL_COLOR : CUT_SECTION_AXIS_COLOR);
        }
        const bool show_holes = m_pattern_mode == PatternMode::Circle || m_pattern_mode == PatternMode::Grid;
        for (int i = 0; i < int(m_section_anchors.size()); ++i) {
            const SectionAnchor &anchor    = m_section_anchors[i];
            const bool           secondary = anchor.kind >= AnchorKind::Fraction;
            if ((secondary && i != m_hover_anchor && anchor.island != m_hover_island) || (anchor.kind == AnchorKind::HoleCenter && !show_holes))
                continue;
            const Vec3d  p  = plane2d_to_world(anchor.pos);
            const double px = pixel_size_at(p);
            if (i == m_hover_anchor)
                render_pick_sphere(p, 5.0 * px, CUT_PICK_HOVER_COLOR);
            else
                render_pick_sphere(p, (secondary ? 2.0 : 3.0) * px, CUT_ANCHOR_COLOR);
        }
    }
    if (m_pattern_mode == PatternMode::Line)
        for (const Vec2d &p : m_pattern_line) {
            const Vec3d w = plane2d_to_world(p);
            render_pick_sphere(w, 4.0 * pixel_size_at(w), CUT_PICK_CHOSEN_COLOR);
        }
    if ((m_pattern_mode == PatternMode::Circle || m_pattern_mode == PatternMode::Grid) && m_pattern_center == 0 && m_pattern_center_set) {
        const Vec3d w = plane2d_to_world(m_pattern_center_point);
        render_pick_sphere(w, 4.0 * pixel_size_at(w), CUT_PICK_CHOSEN_COLOR);
    }

    if (depth_test)
        glsafe(::glEnable(GL_DEPTH_TEST));
}

// Connecteurs du motif en cours, transparents, avant leur ajout.
void GLGizmoAdvancedCut::render_pattern_preview()
{
    if (!m_connectors_editing || m_pattern_mode == PatternMode::None || m_pattern_preview.empty())
        return;
    const CutConnectorAttributes attribs{m_connector_type, CutConnectorStyle(m_connector_style), CutConnectorShape(m_connector_shape_id)};
    auto                         shape = m_shapes.find(attribs);
    if (shape == m_shapes.end())
        return;
    const bool   thread = m_connector_type == CutConnectorType::Thread;
    const double radius = thread ? 0.5 * ISO_METRIC_THREADS[m_thread_designation].size : 0.5 * double(m_connector_size);
    float        height = m_connector_depth_ratio > 0.f ? m_connector_depth_ratio : 1.f;
    if (!(radius > 0.))
        return;

    const Camera &camera          = wxGetApp().plater()->get_camera();
    const bool    looking_forward = is_looking_forward();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    for (const Vec2d &p : m_pattern_preview) {
        Vec3d pos = plane2d_to_world(p);
        float h   = height;
        deal_connector_pos_by_type(pos, h, attribs.type, attribs.style, looking_forward, true, m_clp_normal);
        const Transform3d matrix = Geometry::translation_transform(pos) * m_rotate_matrix * Geometry::scale_transform(Vec3d(radius, radius, double(h)));
        render_glmodel(shape->second, CUT_PATTERN_COLOR.get_data(), camera.get_view_matrix() * matrix, camera.get_projection_matrix());
    }
    glsafe(::glDisable(GL_BLEND));
}

// Coordonnées du connecteur sélectionné dans le repère de sa section (depuis le barycentre, le
// long de la longueur et de la largeur), modifiables, et matière restante autour.
void GLGizmoAdvancedCut::render_selected_connector_coordinates()
{
    if (m_selected_count != 1)
        return;
    CutConnectors &connectors = m_c->selection_info()->model_object()->cut_connectors;
    int            idx        = -1;
    for (size_t i = 0; i < m_selected.size() && i < connectors.size(); ++i)
        if (m_selected[i]) {
            idx = int(i);
            break;
        }
    if (idx < 0)
        return;
    update_sections();
    const Vec2d p      = world_to_plane2d(connector_pos_to_world(connectors[idx].pos));
    const int   island = section_at(p);
    if (island < 0)
        return;
    const SectionIsland &isl        = m_sections[island];
    const float          wrap_width = 20.0f * m_imgui->get_font_size();
    const double         u          = (p - isl.centroid).dot(isl.axis_u);
    const double         v          = (p - isl.centroid).dot(isl.axis_v);

    const float letter_width = m_imgui->calc_text_size(wxString("X ")).x;
    const float field_width  = 0.5f * float(m_editing_window_width) - letter_width - ImGui::GetStyle().ItemSpacing.x;
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Connector position"));
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("From the centroid of the section, along its length (X) and its width (Y), in mm."), wrap_width);
    ImGui::SameLine(m_label_width);
    m_imgui->text("X");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field_width);
    ImGui::BBLInputDouble("##connector_u", &m_buffered_connector_u, 0.0f, 0.0f, "%.2f");
    bool edited = ImGui::IsItemDeactivatedAfterEdit();
    if (!ImGui::IsItemActive())
        m_buffered_connector_u = u;
    ImGui::SameLine();
    m_imgui->text("Y");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field_width);
    ImGui::BBLInputDouble("##connector_v", &m_buffered_connector_v, 0.0f, 0.0f, "%.2f");
    edited |= ImGui::IsItemDeactivatedAfterEdit();
    if (!ImGui::IsItemActive())
        m_buffered_connector_v = v;
    if (edited) {
        const Vec2d target = isl.centroid + m_buffered_connector_u * isl.axis_u + m_buffered_connector_v * isl.axis_v;
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Move connector");
        connectors[idx].pos = world_to_connector_pos(plane2d_to_world(target));
        check_and_update_connectors_state();
    }

    // matière restante entre le trou du connecteur (jeu compris) et le bord de la section
    const double wall = section_edge_distance(island, p) - double(connectors[idx].radius) - std::max(0.0, double(connectors[idx].radius_tolerance));
    const wxString text = format_wxstr(_L("Material around: %1% mm"), format(float(wall), 2));
    if (wall < double(connector_wall_margin()))
        m_imgui->warning_text(text);
    else
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, text);
}

// Valeurs de départ des motifs, d'après la section et la taille du connecteur.
void GLGizmoAdvancedCut::reset_pattern_defaults()
{
    update_sections();
    const bool   thread = m_connector_type == CutConnectorType::Thread;
    const double radius = thread ? 0.5 * ISO_METRIC_THREADS[m_thread_designation].size : 0.5 * std::max(double(m_connector_size), 0.1);
    m_pattern_margin    = radius + std::max(0.0, double(m_connector_size_tolerance)) + 2.0;
    m_pattern_radius    = 10.0;
    if (!m_sections.empty()) {
        const SectionIsland &isl         = m_sections.front();
        const bool           centroid_in = section_edge_distance(0, isl.centroid) > 0.;
        // cercle : autour du barycentre ; s'il tombe dans un vide (tube), le cercle passe au milieu
        // de la paroi (par le point le plus éloigné des bords)
        m_pattern_radius = std::round(10. * std::max(1.0, centroid_in ? 0.5 * isl.pole_radius : (isl.pole - isl.centroid).norm())) / 10.;
        // grille : petit espacement de départ, 2,5 diamètres de connecteur (au demi-mm, 3 mm au
        // moins), pour que le motif apparaisse dès qu'on le choisit, sans dépasser ce qui tient
        // dans la section ; centrée sur le point le plus éloigné des bords si le barycentre est vide
        const double small_spacing = std::max(3.0, std::round(2. * 2.5 * 2. * radius) / 2.);
        auto spacing = [this, small_spacing](double extent, int count) {
            const double fill = std::max(1.0, extent - 2. * m_pattern_margin) / double(std::max(count - 1, 1));
            return std::min(small_spacing, std::round(fill * 10.) / 10.);
        };
        m_pattern_spacing_u = spacing(isl.box_max.x() - isl.box_min.x(), m_pattern_cols);
        m_pattern_spacing_v = spacing(isl.box_max.y() - isl.box_min.y(), m_pattern_rows);
        if (m_pattern_mode == PatternMode::Grid && m_pattern_center == 1 && !centroid_in)
            m_pattern_center = 2;
    }
    m_pattern_line.clear();
    m_pattern_key.clear();
}

// Points du motif en cours (repère du plan), recalculés quand ses réglages ou la section changent.
void GLGizmoAdvancedCut::update_pattern_preview()
{
    if (!m_connectors_editing || m_pattern_mode == PatternMode::None) {
        m_pattern_preview.clear();
        return;
    }
    update_sections();
    std::vector<double> key{double(int(m_pattern_mode)), double(m_pattern_island), double(m_pattern_count), double(m_pattern_center), m_pattern_radius,
                            m_pattern_angle, double(m_pattern_cols), double(m_pattern_rows), m_pattern_margin, m_pattern_start,
                            m_pattern_spacing_u, m_pattern_spacing_v, m_pattern_center_point.x(), m_pattern_center_point.y(),
                            double(m_pattern_center_set)};
    for (const Vec2d &p : m_pattern_line) {
        key.push_back(p.x());
        key.push_back(p.y());
    }
    key.insert(key.end(), m_sections_key.begin(), m_sections_key.end());
    if (key == m_pattern_key)
        return;
    m_pattern_key = std::move(key);
    m_pattern_preview.clear();

    const int count = std::clamp(m_pattern_count, 1, 200);
    if (m_pattern_mode == PatternMode::Line) {
        if (m_pattern_line.size() == 2) {
            const int n = std::max(count, 2);
            for (int k = 0; k < n; ++k)
                m_pattern_preview.push_back(m_pattern_line[0] + (m_pattern_line[1] - m_pattern_line[0]) * (double(k) / double(n - 1)));
        }
        return;
    }

    // centre cliqué (cercle, grille) : un seul motif, dans le repère de la section qui contient ce
    // point (trous compris), quelle que soit la section choisie
    const bool clicked_center = (m_pattern_mode == PatternMode::Circle || m_pattern_mode == PatternMode::Grid) && m_pattern_center == 0;
    if (clicked_center && !m_pattern_center_set)
        return;
    int clicked_island = -1;
    if (clicked_center) {
        const Point c = Point::new_scale(m_pattern_center_point);
        for (int i = 0; i < int(m_sections.size()) && clicked_island < 0; ++i)
            if (m_sections[i].expoly.contour.contains(c))
                clicked_island = i;
        if (clicked_island < 0 && !m_sections.empty())
            clicked_island = 0;
    }

    for (int idx = 0; idx < int(m_sections.size()); ++idx) {
        if (clicked_center ? idx != clicked_island : (m_pattern_island >= 0 && idx != m_pattern_island))
            continue;
        const SectionIsland &isl = m_sections[idx];
        auto                 at  = [&isl](double u, double v) { return Vec2d(isl.centroid + u * isl.axis_u + v * isl.axis_v); };

        switch (m_pattern_mode) {
        case PatternMode::Circle: {
            const Vec2d  center = pattern_center(isl);
            const double start  = Geometry::deg2rad(m_pattern_angle);
            for (int k = 0; k < count; ++k) {
                const double a = start + 2. * PI * double(k) / double(count);
                m_pattern_preview.push_back(center + m_pattern_radius * (std::cos(a) * isl.axis_u + std::sin(a) * isl.axis_v));
            }
            break;
        }
        case PatternMode::Grid: {
            // grille centrée, à espacement donné le long de la longueur (X) et de la largeur (Y)
            const int   cols = std::clamp(m_pattern_cols, 1, 50), rows = std::clamp(m_pattern_rows, 1, 50);
            const Vec2d center = pattern_center(isl);
            for (int i = 0; i < cols; ++i)
                for (int j = 0; j < rows; ++j) {
                    const double u = (double(i) - 0.5 * double(cols - 1)) * m_pattern_spacing_u;
                    const double v = (double(j) - 0.5 * double(rows - 1)) * m_pattern_spacing_v;
                    const Vec2d  p = center + u * isl.axis_u + v * isl.axis_v;
                    if (clicked_center ? section_at(p) >= 0 : section_edge_distance(idx, p) > 0.) // hors matière : ignoré
                        m_pattern_preview.push_back(p);
                }
            break;
        }
        case PatternMode::Contour: {
            // répartis à longueur égale sur le contour décalé vers l'intérieur (le plus long morceau),
            // en partant du bout de la longueur
            const ExPolygons inner = offset_ex(isl.expoly, -float(scale_(m_pattern_margin)));
            const Polygon   *loop  = nullptr;
            double           loop_length = 0.;
            for (const ExPolygon &e : inner)
                if (double l = unscale<double>(e.contour.length()); l > loop_length) {
                    loop_length = l;
                    loop        = &e.contour;
                }
            if (loop == nullptr || loop->points.size() < 2 || !(loop_length > 0.))
                break;
            std::vector<Vec2d> pts;
            for (const Point &q : loop->points)
                pts.emplace_back(unscale(q));
            size_t first = 0;
            for (size_t i = 1; i < pts.size(); ++i)
                if ((pts[i] - isl.centroid).dot(isl.axis_u) > (pts[first] - isl.centroid).dot(isl.axis_u))
                    first = i;
            std::rotate(pts.begin(), pts.begin() + first, pts.end());
            pts.push_back(pts.front());
            const double step = loop_length / double(count);
            double       next = std::clamp(m_pattern_start, 0., 100.) * 0.01 * step, walked = 0.;
            for (size_t i = 0; i + 1 < pts.size() && int(m_pattern_preview.size()) < count; ++i) {
                const double seg = (pts[i + 1] - pts[i]).norm();
                while (seg > 0. && next <= walked + seg && int(m_pattern_preview.size()) < count) {
                    m_pattern_preview.push_back(pts[i] + (pts[i + 1] - pts[i]) * ((next - walked) / seg));
                    next += step;
                }
                walked += seg;
            }
            break;
        }
        case PatternMode::Auto: {
            // les « count » points les plus éloignés les uns des autres, à « marge » au moins du bord :
            // choix glouton (le plus loin des points déjà pris) puis quelques passes d'amélioration
            const double w = isl.box_max.x() - isl.box_min.x(), h = isl.box_max.y() - isl.box_min.y();
            const double cell = std::max(std::sqrt(std::max(w * h, 1e-6) / 4000.), 0.05);
            std::vector<Vec2d> candidates;
            for (double u = isl.box_min.x() + 0.5 * cell; u < isl.box_max.x(); u += cell)
                for (double v = isl.box_min.y() + 0.5 * cell; v < isl.box_max.y(); v += cell)
                    if (const Vec2d p = at(u, v); section_edge_distance(idx, p) >= m_pattern_margin)
                        candidates.push_back(p);
            if (candidates.empty())
                break;
            std::vector<Vec2d> chosen;
            size_t             start = 0; // le plus loin du barycentre
            for (size_t i = 1; i < candidates.size(); ++i)
                if ((candidates[i] - isl.centroid).squaredNorm() > (candidates[start] - isl.centroid).squaredNorm())
                    start = i;
            chosen.push_back(candidates[start]);
            auto min_dist2 = [&chosen](const Vec2d &p, int skip) {
                double best = std::numeric_limits<double>::max();
                for (int k = 0; k < int(chosen.size()); ++k)
                    if (k != skip)
                        best = std::min(best, (chosen[k] - p).squaredNorm());
                return best;
            };
            // distance (au carré) de chaque candidat au plus proche point déjà pris, tenue à jour
            std::vector<double> nearest(candidates.size());
            for (size_t i = 0; i < candidates.size(); ++i)
                nearest[i] = (candidates[i] - chosen.front()).squaredNorm();
            while (int(chosen.size()) < count && int(chosen.size()) < int(candidates.size())) {
                const size_t far_idx = size_t(std::max_element(nearest.begin(), nearest.end()) - nearest.begin());
                chosen.push_back(candidates[far_idx]);
                for (size_t i = 0; i < candidates.size(); ++i)
                    nearest[i] = std::min(nearest[i], (candidates[i] - chosen.back()).squaredNorm());
            }
            // passes d'amélioration bornées en calcul (beaucoup de points : moins de passes, l'interface
            // ne doit pas se figer pendant qu'on change le nombre)
            const double work   = double(chosen.size()) * double(chosen.size()) * double(candidates.size());
            const int    passes = std::clamp(int(2e7 / std::max(work, 1.)), 0, 8);
            for (int pass = 0; pass < passes && chosen.size() > 1; ++pass) {
                bool moved = false;
                for (int k = 0; k < int(chosen.size()); ++k) {
                    double best_d = min_dist2(chosen[k], k);
                    Vec2d  best_p = chosen[k];
                    for (const Vec2d &c : candidates)
                        if (const double d = min_dist2(c, k); d > best_d * (1. + 1e-9)) {
                            best_d = d;
                            best_p = c;
                        }
                    if (!(best_p == chosen[k])) {
                        chosen[k] = best_p;
                        moved     = true;
                    }
                }
                if (!moved)
                    break;
            }
            append(m_pattern_preview, std::move(chosen));
            break;
        }
        default: break;
        }
    }
    // pas deux connecteurs au même endroit (rayon ou espacement nul)
    std::vector<Vec2d> unique;
    for (const Vec2d &p : m_pattern_preview)
        if (std::none_of(unique.begin(), unique.end(), [&p](const Vec2d &q) { return (p - q).squaredNorm() < 0.05 * 0.05; }))
            unique.push_back(p);
    m_pattern_preview = std::move(unique);
}

// Symétrie : sous la souris, à 10 pixels près, un ancrage, sinon (avant le premier point) une droite
// guide, sinon le point libre de la face.
void GLGizmoAdvancedCut::update_mirror_hover(const Vec3d &world)
{
    update_sections();
    const Vec2d  p   = world_to_plane2d(world);
    const double tol = std::max(0.3, 10.0 * pixel_size_at(world));
    m_hover_island        = section_at(p);
    m_hover_anchor        = -1;
    m_mirror_hover_point  = p;
    m_mirror_hover_anchor = m_mirror_hover_island = m_mirror_hover_guide = -1;
    double best           = tol;
    for (int i = 0; i < int(m_section_anchors.size()); ++i)
        if (const double d = (m_section_anchors[i].pos - p).norm(); d <= best) {
            best                  = d;
            m_mirror_hover_anchor = i;
            m_mirror_hover_point  = m_section_anchors[i].pos;
        }
    if (m_mirror_hover_anchor >= 0 || m_mirror_has_first)
        return;
    best = tol;
    for (int i = 0; i < int(m_sections.size()); ++i)
        for (int j = 0; j < int(m_sections[i].guides.size()); ++j) {
            const SectionGuide &g  = m_sections[i].guides[j];
            const Vec2d         ab = g.b - g.a;
            const double        l2 = ab.squaredNorm();
            const double        t  = l2 > 0. ? std::clamp((p - g.a).dot(ab) / l2, 0., 1.) : 0.;
            if (const double d = (p - (g.a + t * ab)).norm(); d <= best) {
                best                  = d;
                m_mirror_hover_island = i;
                m_mirror_hover_guide  = j;
            }
        }
}

// Clic pendant le choix de symétrie : sur une droite guide, symétrie par rapport à elle ; sinon le
// clic donne un point (ancrage ou point libre) : le premier est retenu, le second fixe l'axe.
void GLGizmoAdvancedCut::apply_mirror_pick()
{
    Vec2d a, b;
    if (!m_mirror_has_first && m_mirror_hover_anchor < 0 && m_mirror_hover_island >= 0 && m_mirror_hover_island < int(m_sections.size()) &&
        m_mirror_hover_guide >= 0 && m_mirror_hover_guide < int(m_sections[m_mirror_hover_island].guides.size())) {
        a = m_sections[m_mirror_hover_island].guides[m_mirror_hover_guide].a;
        b = m_sections[m_mirror_hover_island].guides[m_mirror_hover_guide].b;
    } else if (!m_mirror_has_first) {
        m_mirror_first     = m_mirror_hover_point;
        m_mirror_has_first = true;
        return;
    } else {
        if ((m_mirror_hover_point - m_mirror_first).squaredNorm() < 0.05 * 0.05) // même point : on attend le second
            return;
        a = m_mirror_first;
        b = m_mirror_hover_point;
    }
    m_mirror_picking      = false;
    m_mirror_has_first    = false;
    m_mirror_hover_anchor = m_mirror_hover_island = m_mirror_hover_guide = -1;
    mirror_selected_connectors(a, b);
}

// Centre des motifs cercle et grille dans une section : point cliqué sur la face, barycentre, point
// le plus éloigné des bords, centre de la boîte, ou centre du premier trou.
Vec2d GLGizmoAdvancedCut::pattern_center(const SectionIsland &island) const
{
    if (m_pattern_center == 0 && m_pattern_center_set)
        return m_pattern_center_point;
    if (m_pattern_center == 2)
        return island.pole;
    if (m_pattern_center == 3)
        return island.centroid + 0.5 * (island.box_min.x() + island.box_max.x()) * island.axis_u +
               0.5 * (island.box_min.y() + island.box_max.y()) * island.axis_v;
    if (m_pattern_center == 4 && !island.hole_centers.empty())
        return island.hole_centers.front();
    return island.centroid;
}

// Le prochain clic sur la face choisit un point du motif (A / B, centre) au lieu de poser un connecteur.
bool GLGizmoAdvancedCut::click_picks_pattern_point() const
{
    return m_mirror_picking || m_pattern_mode == PatternMode::Circle || m_pattern_mode == PatternMode::Grid ||
           (m_pattern_mode == PatternMode::Line && m_pattern_line.size() < 2);
}

// Réglages des motifs et de la symétrie, dans le panneau des connecteurs.
void GLGizmoAdvancedCut::render_patterns_input()
{
    CutConnectors &connectors = m_c->selection_info()->model_object()->cut_connectors;
    const float    wrap_width = 20.0f * m_imgui->get_font_size();
    update_sections();

    ImGui::Separator();
    // un filetage se visse en tournant : un seul par face coupée, donc ni motif ni symétrie
    if (m_connector_type == CutConnectorType::Thread) {
        m_pattern_mode = PatternMode::None;
        m_pattern_preview.clear();
        m_pattern_line.clear();
        m_imgui->text_wrapped(_L("A thread screws in by turning: only one per cut face, so no pattern or mirror. Clicking elsewhere on its face "
                                 "moves it; clicking another face of the cut adds one there."),
                              wrap_width);
        // avertissement si une face porte à la fois un filetage et d'autres connecteurs
        std::set<int> thread_faces;
        for (const CutConnector &c : connectors)
            if (c.attribs.type == CutConnectorType::Thread)
                thread_faces.insert(connector_section(c));
        bool mixed = false;
        for (const CutConnector &c : connectors)
            mixed |= c.attribs.type != CutConnectorType::Thread && thread_faces.count(connector_section(c)) > 0;
        if (mixed)
            m_imgui->warning_text_wrapped(_L("The other connectors of this face will prevent the thread from being screwed in."), wrap_width);
        return;
    }
    std::vector<std::string> modes = {_u8L("None"), _u8L("Circle"), _u8L("Grid"), _u8L("Along the edge"), _u8L("Between two points"), _u8L("Automatic spread")};
    size_t                   mode  = size_t(m_pattern_mode);
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    if (render_combo(_u8L("Pattern"), modes, mode, m_label_width, m_editing_window_width)) {
        m_pattern_mode = PatternMode(mode);
        reset_pattern_defaults();
    }
    const bool clicked_center = (m_pattern_mode == PatternMode::Circle || m_pattern_mode == PatternMode::Grid) && m_pattern_center == 0;
    if (m_pattern_mode != PatternMode::None && m_pattern_mode != PatternMode::Line && !clicked_center && m_sections.size() > 1) {
        std::vector<std::string> islands = {_u8L("All sections")};
        for (size_t i = 0; i < m_sections.size(); ++i)
            islands.push_back(GUI::format(_u8L("Section %1%"), i + 1) + (i == 0 ? " (" + _u8L("largest") + ")" : std::string()));
        size_t island = size_t(std::clamp(m_pattern_island + 1, 0, int(m_sections.size())));
        if (render_combo(_u8L("Section"), islands, island, m_label_width, m_editing_window_width))
            m_pattern_island = int(island) - 1;
    }
    ImGuiWrapper::pop_combo_style();
    if (m_pattern_island >= int(m_sections.size()))
        m_pattern_island = -1;

    auto int_row = [this](const wxString &label, const char *id, int &value, int min_value) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(m_label_width);
        ImGui::SetNextItemWidth(float(m_editing_window_width));
        ImGui::InputInt(id, &value, 1, 1);
        value = std::clamp(value, min_value, 200);
    };
    // réglet + cote saisie au clavier, comme « Position » de la coupe ; une cote hors du réglet reste
    // possible (le réglet s'allonge jusqu'à elle)
    auto slider_row = [this](const wxString &label, const std::string &id, double &value, double lo, double hi, const char *fmt) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(float(m_label_width));
        const float width        = float(m_editing_window_width);
        const float slider_width = 0.58f * width;
        const float gap          = 0.02f * width;
        ImGui::SetNextItemWidth(slider_width);
        float slider_value = float(value);
        if (m_imgui->bbl_slider_float_style("##slider_" + id, &slider_value, float(lo), float(std::max(hi, value)), fmt))
            value = double(slider_value);
        ImGui::SameLine(float(m_label_width) + slider_width + gap);
        ImGui::SetNextItemWidth(width - slider_width - gap);
        ImGui::BBLInputDouble(("##input_" + id).c_str(), &value, 0.0f, 0.0f, fmt);
        value = std::max(value, lo);
    };
    // bornes des réglets d'après les sections concernées
    double max_length = 1., max_width = 1., max_half_diagonal = 1., max_inner = 0.5;
    for (int i = 0; i < int(m_sections.size()); ++i)
        if (m_pattern_island < 0 || i == m_pattern_island) {
            const Vec2d size  = m_sections[i].box_max - m_sections[i].box_min;
            max_length        = std::max(max_length, size.x());
            max_width         = std::max(max_width, size.y());
            max_half_diagonal = std::max(max_half_diagonal, 0.5 * size.norm());
            max_inner         = std::max(max_inner, m_sections[i].pole_radius);
        }
    auto center_row = [this, wrap_width]() {
        std::vector<std::string> centers = {_u8L("Point clicked on the face"), _u8L("Centroid of the section"), _u8L("Farthest point from the edges"),
                                            _u8L("Center of the section box"), _u8L("Center of a hole")};
        size_t                   center  = size_t(std::clamp(m_pattern_center, 0, 4));
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        if (render_combo(_u8L("Center"), centers, center, m_label_width, m_editing_window_width))
            m_pattern_center = int(center);
        ImGuiWrapper::pop_combo_style();
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, m_pattern_center == 0 && !m_pattern_center_set ?
                                                                     _L("Click the center on the cut face.") :
                                                                     _L("Click the cut face to choose another center."));
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("When anchor magnetism is on, anchors attract the click, including the centers of holes."), wrap_width);
    };

    switch (m_pattern_mode) {
    case PatternMode::Circle: {
        center_row();
        int_row(_L("Number"), "##pattern_count", m_pattern_count, 2);
        slider_row(_L("Radius"), "pattern_radius", m_pattern_radius, 0., max_half_diagonal, "%.2f mm");
        slider_row(_L("Start angle"), "pattern_angle", m_pattern_angle, 0., 360., u8"%.1f °");
        break;
    }
    case PatternMode::Grid:
        center_row();
        int_row(_L("Columns"), "##pattern_cols", m_pattern_cols, 1);
        int_row(_L("Rows"), "##pattern_rows", m_pattern_rows, 1);
        m_imgui->disabled_begin(m_pattern_cols < 2);
        slider_row(_L("Spacing X"), "pattern_spacing_u", m_pattern_spacing_u, 0., max_length, "%.2f mm");
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Distance between two columns, along the length of the section."), wrap_width);
        m_imgui->disabled_end();
        m_imgui->disabled_begin(m_pattern_rows < 2);
        slider_row(_L("Spacing Y"), "pattern_spacing_v", m_pattern_spacing_v, 0., max_width, "%.2f mm");
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Distance between two rows, along the width of the section."), wrap_width);
        m_imgui->disabled_end();
        break;
    case PatternMode::Contour:
        int_row(_L("Number"), "##pattern_count", m_pattern_count, 1);
        slider_row(_L("Edge distance"), "pattern_margin", m_pattern_margin, 0., max_inner, "%.2f mm");
        slider_row(_L("Start offset"), "pattern_start", m_pattern_start, 0., 100., "%.0f %%");
        m_pattern_start = std::clamp(m_pattern_start, 0.0, 100.0);
        break;
    case PatternMode::Line:
        m_imgui->text_wrapped(m_pattern_line.size() < 2 ? (m_pattern_line.empty() ? _L("Click point A on the cut face.") : _L("Click point B on the cut face.")) :
                                                          _L("Points A and B chosen."),
                              wrap_width);
        int_row(_L("Number"), "##pattern_count", m_pattern_count, 2);
        if (!m_pattern_line.empty() && m_imgui->button(_L("Choose again")))
            m_pattern_line.clear();
        break;
    case PatternMode::Auto:
        int_row(_L("Number"), "##pattern_count", m_pattern_count, 1);
        slider_row(_L("Edge margin"), "pattern_margin", m_pattern_margin, 0., max_inner, "%.2f mm");
        break;
    default: break;
    }
    m_pattern_margin = std::max(0.0, m_pattern_margin);

    if (m_pattern_mode != PatternMode::None) {
        update_pattern_preview();
        m_imgui->disabled_begin(m_pattern_preview.empty());
        if (m_imgui->button(format_wxstr(_L("Add %1% connectors"), m_pattern_preview.size()))) {
            add_connectors_at(m_pattern_preview);
            m_pattern_mode = PatternMode::None;
            m_pattern_preview.clear();
            m_pattern_line.clear();
        }
        m_imgui->disabled_end();
    }

    // symétrie des connecteurs sélectionnés, par rapport à un axe ou à la droite de deux points
    // choisis sur la face
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Mirror selection") + ":");
    ImGui::SameLine();
    if (m_mirror_picking) {
        if (m_imgui->button(_L("Cancel") + "##mirror_cancel")) {
            m_mirror_picking   = false;
            m_mirror_has_first = false;
        }
        m_imgui->text_wrapped(m_mirror_has_first ? _L("Click the second point of the axis. Right click: undo the first point. Esc: cancel.") :
                                                   _L("Click an axis (blue line), or two points of the cut face that define the axis. Esc: cancel."),
                              wrap_width);
        if (m_mirror_hover_anchor >= 0 && m_mirror_hover_anchor < int(m_section_anchors.size()))
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, anchor_name(m_section_anchors[m_mirror_hover_anchor]));
        else if (m_mirror_hover_island >= 0 && m_mirror_hover_island < int(m_sections.size()) && m_mirror_hover_guide >= 0 &&
                 m_mirror_hover_guide < int(m_sections[m_mirror_hover_island].guides.size()))
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, guide_name(m_sections[m_mirror_hover_island].guides[m_mirror_hover_guide]));
    } else {
        m_imgui->disabled_begin(m_selected_count == 0 || connectors.empty());
        if (m_imgui->button(_L("Choose an axis or two points"))) {
            m_mirror_picking      = true;
            m_mirror_has_first    = false;
            m_mirror_hover_anchor = m_mirror_hover_island = m_mirror_hover_guide = -1;
            m_pattern_mode        = PatternMode::None;
            m_pattern_preview.clear();
            m_pattern_line.clear();
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Copies the selected connectors symmetrically about an axis that you click on the cut face (symmetry axes of "
                                "the section, length and width axes, medians, diagonals, rays), or about the line through two points that you "
                                "click (anchors or free points)."),
                             wrap_width);
        m_imgui->disabled_end();
    }
}

// Symétrie des connecteurs sélectionnés par rapport à la droite (a, b) de la face : une copie par
// connecteur, sauf si la place est prise ou si la copie tombe hors de la face.
void GLGizmoAdvancedCut::mirror_selected_connectors(const Vec2d &a, const Vec2d &b)
{
    CutConnectors &connectors = m_c->selection_info()->model_object()->cut_connectors;
    update_sections();
    Vec2d dir = b - a;
    if (dir.squaredNorm() < 1e-12)
        return;
    dir.normalize();
    std::vector<CutConnector> copies;
    for (size_t i = 0; i < connectors.size() && i < m_selected.size(); ++i) {
        if (!m_selected[i] || connectors[i].attribs.type == CutConnectorType::Thread) // un seul filetage par face
            continue;
        const Vec2d p      = world_to_plane2d(connector_pos_to_world(connectors[i].pos));
        const Vec2d target = 2. * (a + dir.dot(p - a) * dir) - p;
        if ((target - p).squaredNorm() < 1e-4 || section_at(target) < 0)
            continue;
        bool taken = false;
        for (const CutConnector &c : connectors)
            if ((world_to_plane2d(connector_pos_to_world(c.pos)) - target).squaredNorm() < 1e-4)
                taken = true;
        if (taken)
            continue;
        CutConnector copy(connectors[i]);
        copy.pos = world_to_connector_pos(plane2d_to_world(target));
        copies.push_back(copy);
    }
    if (copies.empty())
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Mirror connectors");
    unselect_all_connectors();
    if (m_selected.size() != connectors.size())
        m_selected.assign(connectors.size(), false);
    for (const CutConnector &copy : copies) {
        connectors.push_back(copy);
        m_selected.push_back(true);
    }
    m_selected_count = int(copies.size());
    check_and_update_connectors_state();
    m_parent.set_as_dirty();
}

// Filetage ISO métrique, comme l'outil Filetage de Fusion 360 : type, taille, désignation, classe,
// sens ; puis le jeu sur le rayon et l'alerte d'impression.
void GLGizmoAdvancedCut::render_thread_input()
{
    CutConnectors &connectors = m_c->selection_info()->model_object()->cut_connectors;
    const float    wrap_width = 20.0f * m_imgui->get_font_size();
    if (m_thread_designation >= size_t(ISO_METRIC_THREADS_COUNT))
        m_thread_designation = size_t(iso_metric_thread_closest(10.));

    // première désignation de chaque taille
    static std::vector<int> size_first;
    if (size_first.empty())
        for (int i = 0; i < ISO_METRIC_THREADS_COUNT; ++i)
            if (i == 0 || ISO_METRIC_THREADS[i].size != ISO_METRIC_THREADS[i - 1].size)
                size_first.push_back(i);
    size_t size_idx = 0;
    while (size_idx + 1 < size_first.size() && size_first[size_idx + 1] <= int(m_thread_designation))
        ++size_idx;

    // tailles trop grandes pour la plus grande section (jeu et mur minimal compris)
    update_sections();
    double max_radius = 0.;
    for (const SectionIsland &isl : m_sections)
        max_radius = std::max(max_radius, isl.pole_radius);
    const double room = max_radius - std::max(0.0, double(m_connector_size_tolerance)) - double(connector_wall_margin());

    std::vector<std::string> sizes;
    for (int first : size_first) {
        const double size = ISO_METRIC_THREADS[first].size;
        std::string  text = format(float(size), 1) + " mm"; // « 120.0 mm », comme Fusion 360
        if (!m_sections.empty() && 0.5 * ISO_METRIC_THREADS[first].size > room)
            text += " " + _u8L("(too large)");
        sizes.push_back(text);
    }
    const int                first = size_first[size_idx];
    const int                last  = size_idx + 1 < size_first.size() ? size_first[size_idx + 1] : ISO_METRIC_THREADS_COUNT;
    std::vector<std::string> designations;
    for (int i = first; i < last; ++i)
        designations.push_back(ISO_METRIC_THREADS[i].designation);
    size_t designation_idx = m_thread_designation - size_t(first);

    bool changed = false;
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    size_t type_idx = 0;
    render_combo(_u8L("Thread type"), {"ISO Metric profile"}, type_idx, m_label_width, m_editing_window_width);
    if (render_combo(_u8L("Size"), sizes, size_idx, m_label_width, m_editing_window_width)) {
        m_thread_designation = size_t(size_first[size_idx]);
        changed              = true;
    }
    if (render_combo(_u8L("Designation"), designations, designation_idx, m_label_width, m_editing_window_width)) {
        m_thread_designation = size_t(first) + designation_idx;
        changed              = true;
    }
    changed |= render_combo(_u8L("Class"), {"6g / 6H", "4g6g / 6H"}, m_thread_class, m_label_width, m_editing_window_width);
    changed |= render_combo(_u8L("Direction"), {_u8L("Right hand"), _u8L("Left hand")}, m_thread_hand, m_label_width, m_editing_window_width);
    ImGuiWrapper::pop_combo_style();

    const IsoMetricThread &thread = ISO_METRIC_THREADS[m_thread_designation];
    if (changed) {
        m_connector_size = float(thread.size);
        if (m_connector_depth_ratio > 0.f && m_connector_depth_ratio < float(2. * thread.pitch))
            m_connector_depth_ratio = float(2. * thread.pitch);
        apply_selected_connectors([this, &connectors, &thread](size_t idx) {
            connectors[idx].paras.thread_designation = int(m_thread_designation);
            connectors[idx].paras.thread_class       = int(m_thread_class);
            connectors[idx].paras.thread_left_hand   = m_thread_hand == 1;
            connectors[idx].radius                   = float(0.5 * thread.size);
            connectors[idx].height                   = std::max(connectors[idx].height, float(2. * thread.pitch));
        });
    }

    // jeu sur le rayon (tous les flancs du taraudage)
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Clearance"));
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Gap left all around the thread of the hole, perpendicular to its surfaces (crests, flanks and roots)."), wrap_width);
    ImGui::SameLine(m_label_width);
    float clearance = std::max(0.f, m_connector_size_tolerance);
    ImGui::SetNextItemWidth(float(m_editing_window_width));
    if (ImGui::BBLDragFloat("##thread_clearance", &clearance, 0.01f, 0.f, 1.f, "%.2f mm")) {
        m_connector_size_tolerance = std::clamp(clearance, 0.f, 1.f);
        apply_selected_connectors([this, &connectors](size_t idx) { connectors[idx].radius_tolerance = m_connector_size_tolerance; });
    }

    // alerte : filet trop fin pour la buse ou pour la hauteur de couche
    double nozzle = 0.4, layer = 0.2;
    if (const auto *opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter"); opt && !opt->values.empty())
        nozzle = opt->get_at(0);
    if (const auto *opt = wxGetApp().preset_bundle->prints.get_edited_preset().config.option<ConfigOptionFloat>("layer_height"); opt)
        layer = opt->value;
    const double depth = 0.5 * (thread.ext_6g[0] - thread.ext_6g[2]);
    if (depth < nozzle || thread.pitch < 4. * layer)
        m_imgui->warning_text_wrapped(format_wxstr(_L("Fine pitch for printing (nozzle %1% mm, layers %2% mm): the thread will be rough. Prefer the coarse pitch or a larger size."),
                                                   format(float(nozzle), 2), format(float(layer), 2)),
                                      wrap_width);
}

PartSelection::PartSelection(
    const ModelObject *mo, const Transform3d &cut_matrix, int instance_idx_in, const Vec3d &center, const Vec3d &normal, const CommonGizmosDataObjects::ObjectClipper &oc)
    : m_instance_idx(instance_idx_in)
{
    Cut cut(mo, instance_idx_in, cut_matrix);
    add_object(cut.perform_with_plane().front());

    const ModelVolumePtrs &volumes = model_object()->volumes;

    // split to parts
    for (int id = int(volumes.size()) - 1; id >= 0; id--) {
        auto look = volumes[id]->is_model_part();
        if (volumes[id]->is_splittable() && volumes[id]->is_model_part()) // we have to split just solid volumes
            volumes[id]->split(1);
    }

    const Vec3d inst_offset = model_object()->instances[m_instance_idx]->get_offset();
    int         i           = 0;
    m_cut_parts.resize(volumes.size());
    for (const ModelVolume *volume : volumes) {
        assert(volume != nullptr);
        m_cut_parts[i].is_modifier = !volume->is_model_part();
        m_cut_parts[i].is_up_part = false;
        if (m_cut_parts[i].raycaster) { delete m_cut_parts[i].raycaster; }
        m_cut_parts[i].raycaster = new MeshRaycaster(volume->mesh());
        m_cut_parts[i].glmodel.reset();
        m_cut_parts[i].glmodel.init_from(volume->mesh_ptr()->its);
        m_cut_parts[i].trans = Geometry::translation_transform(inst_offset) * model_object()->volumes[i]->get_matrix();
        // Now check whether this part is below or above the plane.
        Transform3d tr   = (model_object()->instances[m_instance_idx]->get_matrix() * volume->get_matrix()).inverse();
        Vec3f       pos  = (tr * center).cast<float>();
        Vec3f       norm = (tr.linear().inverse().transpose() * normal).cast<float>();

        for (const Vec3f &v : volume->mesh().its.vertices) {
            double p = (v - pos).dot(norm);
            if (std::abs(p) > 0.01) { // 0.01 mm is enough
                m_cut_parts[i].is_up_part = p > 0.;
                break;
            }
        }
        i++;
    }

    m_back_cut_parts_state.resize(m_cut_parts.size());
    for (size_t i = 0; i < m_cut_parts.size(); i++) {
        m_back_cut_parts_state[i] = m_cut_parts[i].is_up_part;
    }
    // Now go through the contours and create a map from contours to parts.
    m_contour_points.clear();
    m_contour_to_parts.clear();
    m_debug_pts = std::vector<std::vector<Vec3d>>(m_cut_parts.size(), std::vector<Vec3d>());
    if (std::vector<Vec3d> pts = oc.point_per_contour(); !pts.empty()) {
        m_contour_to_parts.resize(pts.size());

        for (size_t pt_idx = 0; pt_idx < pts.size(); ++pt_idx) {
            const Vec3d &pt  = pts[pt_idx];
            const Vec3d  dir = (center - pt).dot(normal) * normal;
            m_contour_points.emplace_back(dir + pt); // the result is in world coordinates.

            // Now, cast a ray from every contour point and see which volumes of the ones above
            // the plane are hit from the inside.
            for (size_t part_id = 0; part_id < m_cut_parts.size(); ++part_id) {
                const sla::IndexedMesh &aabb = m_cut_parts[part_id].raycaster->get_aabb_mesh();
                const Transform3d &     tr   = (Geometry::translation_transform(model_object()->instances[m_instance_idx]->get_offset()) *
                                         Geometry::translation_transform(model_object()->volumes[part_id]->get_offset()))
                                            .inverse();
                for (double d : {-1., 1.}) {
                    const Vec3d dir_mesh = d * tr.linear().inverse().transpose() * normal;
                    const Vec3d src      = tr * (m_contour_points[pt_idx] + d * 0.01 * normal);
                    auto        hit      = aabb.query_ray_hit(src, dir_mesh);

                    m_debug_pts[part_id].emplace_back(src);

                    if (hit.is_inside()) {
                        // This part belongs to this point.
                        if (d == 1.)
                            m_contour_to_parts[pt_idx].first.emplace_back(part_id);
                        else
                            m_contour_to_parts[pt_idx].second.emplace_back(part_id);
                    }
                }
            }
        }
    }

    m_valid = true;
}

// In CutMode::cutTongueAndGroove we use PartSelection just for rendering
PartSelection::PartSelection(const ModelObject *object, int instance_idx_in) : m_instance_idx(instance_idx_in)
{
    add_object(object);
    const ModelVolumePtrs &volumes     = model_object()->volumes;
    const Vec3d            inst_offset = model_object()->instances[m_instance_idx]->get_offset();
    int                    i           = 0;
    m_cut_parts.resize(volumes.size());
    for (const ModelVolume *volume : volumes) {
        assert(volume != nullptr);
        m_cut_parts[i].is_modifier = !volume->is_model_part();
        if (m_cut_parts[i].raycaster) { delete m_cut_parts[i].raycaster; }
        m_cut_parts[i].raycaster = new MeshRaycaster(volume->mesh());
        m_cut_parts[i].glmodel.reset();
        m_cut_parts[i].glmodel.init_from(volume->mesh_ptr()->its);
        m_cut_parts[i].trans    = Geometry::translation_transform(inst_offset) * model_object()->volumes[i]->get_matrix();
        // Now check whether this part is below or above the plane.
        m_cut_parts[i].is_up_part = volume->is_from_upper();
        i++;
    }

    m_valid = true;
}

void PartSelection::part_render(const Vec3d *cut_center, const Vec3d *normal)
{
    if (!valid())
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();
    glEnable(GL_DEPTH_TEST);
    for (size_t id = 0; id < m_cut_parts.size(); ++id) { // m_parts.size() test
        bool  is_looking_forward = true;
        auto is_at_normal_dir  = false;
        if (cut_center && normal) {
            auto part_center =m_cut_parts[id].trans * m_cut_parts[id].glmodel.get_bounding_box().center();
            is_at_normal_dir=(*cut_center - part_center).normalized().dot(*normal) > 0 ;
            Vec3d valid_normal = is_at_normal_dir ? *normal : -*normal;
            is_looking_forward = camera.get_dir_forward().dot(valid_normal) < 0.05;
        }
        if (cut_center) {
            if (!is_looking_forward)
                continue;
        }
        if (m_cut_parts[id].is_modifier) {
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        }
        GLGizmoBase::render_glmodel(m_cut_parts[id].glmodel,
                                    m_cut_parts[id].is_modifier ? MODIFIER_COLOR.get_data() :
                                                                  (m_cut_parts[id].is_up_part ? UPPER_PART_COLOR.get_data() : LOWER_PART_COLOR.get_data()),
                                    view_matrix * m_cut_parts[id].trans, projection_matrix);
        if (m_cut_parts[id].is_modifier)
            glsafe(::glDisable(GL_BLEND));
    }
}

void PartSelection::add_object(const ModelObject *object)
{
    m_model = Model();
    m_model.add_object(*object);

    const double sla_shift_z = wxGetApp().plater()->canvas3D()->get_selection().get_first_volume()->get_sla_shift_z();
    if (!is_approx(sla_shift_z, 0.)) {
        Vec3d inst_offset = model_object()->instances[m_instance_idx]->get_offset();
        inst_offset[Z] += sla_shift_z;
        model_object()->instances[m_instance_idx]->set_offset(inst_offset);
    }
}

bool PartSelection::is_one_object() const
{
    // In theory, the implementation could be just this:
    // return m_contour_to_parts.size() == m_ignored_contours.size();
    // However, this would require that the part-contour correspondence works
    // flawlessly. Because it is currently not always so for self-intersecting
    // objects, let's better check the parts itself:
    if (m_cut_parts.size() < 2) return true;
    return std::all_of(m_cut_parts.begin(), m_cut_parts.end(), [this](const PartPara &part) { return part.is_modifier || part.is_up_part == m_cut_parts.front().is_up_part; });
}

std::vector<Cut::Part> PartSelection::get_cut_parts()
{
    std::vector<Cut::Part> parts;
    for (const auto &part : m_cut_parts) parts.push_back({part.is_up_part, part.is_modifier});
    return parts;
}

bool PartSelection::has_modified_cut_parts()
{
    if (m_back_cut_parts_state.size() == 0 || m_back_cut_parts_state.size() != m_cut_parts.size()) {
        return false;
    }
    for (size_t i = 0; i < m_cut_parts.size(); i++) {
        if (m_back_cut_parts_state[i] != m_cut_parts[i].is_up_part) {
            return true;
        }
    }
    return false;
}

void PartSelection::toggle_selection(const Vec2d &mouse_pos)
{
    const Camera &camera     = wxGetApp().plater()->get_camera();
    const Vec3d & camera_pos = camera.get_position();

    Vec3f pos;
    Vec3f normal;

    std::vector<std::pair<size_t, double>> hits_id_and_sqdist;

    for (size_t id = 0; id < m_cut_parts.size(); ++id) {
        //        const Vec3d volume_offset = model_object()->volumes[id]->get_offset();
        Transform3d tr = Geometry::translation_transform(model_object()->instances[m_instance_idx]->get_offset()) *
                         Geometry::translation_transform(model_object()->volumes[id]->get_offset());
        if (m_cut_parts[id].raycaster->unproject_on_mesh(mouse_pos, tr, camera, pos, normal)) {
            hits_id_and_sqdist.emplace_back(id, (camera_pos - tr * (pos.cast<double>())).squaredNorm());
        }
    }
    if (!hits_id_and_sqdist.empty()) {
        size_t id = std::min_element(hits_id_and_sqdist.begin(), hits_id_and_sqdist.end(), [](const std::pair<size_t, double> &a, const std::pair<size_t, double> &b) {
                        return a.second < b.second;
                    })->first;
        toggle_selection(id);
    }
}

void PartSelection::toggle_selection(int id)
{
    if (id >= 0) {
        m_cut_parts[id].is_up_part = !m_cut_parts[id].is_up_part;

        // And now recalculate the contours which should be ignored.
        /* m_ignored_contours.clear();
         size_t cont_id = 0;
         for (const auto &[parts_above, parts_below] : m_contour_to_parts) {
             for (size_t upper : parts_above) {
                 bool upper_sel = m_cut_parts[upper].is_up_part;
                 if (std::find_if(parts_below.begin(), parts_below.end(), [this, &upper_sel](const size_t &i) {
                     return m_cut_parts[i].is_up_part == upper_sel; }) !=
                     parts_below.end()) {
                     m_ignored_contours.emplace_back(cont_id);
                     break;
                 }
             }
             ++cont_id;
         }*/
    }
}

void PartSelection::turn_over_selection()
{
    for (PartPara &part : m_cut_parts) part.is_up_part = !part.is_up_part;
}



}} // namespace Slic3r
