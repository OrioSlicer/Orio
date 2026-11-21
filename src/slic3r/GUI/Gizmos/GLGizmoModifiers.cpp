// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code, which overrides our localization "L" macro.
#include "GLGizmoModifiers.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoMeasure.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoSVG.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Emboss.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Measure.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

#include <Eigen/Dense>
#include <GL/glew.h>
#include <imgui/imgui_internal.h>

#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>

#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

// Repères dessinés sur la pièce : ancrage aimanté (vert, comme l'outil Mesure), ancrages de la face
// survolée (gris), faces choisies pour l'alignement (orange).
static const ColorRGBA MODIFIER_ANCHOR_HOVER_COLOR = {0.0f, 1.0f, 0.0f, 1.0f};
static const ColorRGBA MODIFIER_ANCHOR_COLOR       = {0.62f, 0.62f, 0.62f, 1.0f};
static const ColorRGBA MODIFIER_ALIGN_COLOR        = {1.0f, 0.55f, 0.0f, 0.6f};
// Axes X, Y, Z (poignées, alignements), comme les outils Déplacer et Pivoter.
static const ColorRGBA MODIFIER_AXIS_COLORS[3] = {{0.9f, 0.25f, 0.25f, 1.0f}, {0.3f, 0.8f, 0.3f, 1.0f}, {0.3f, 0.45f, 1.0f, 1.0f}};
// Cotes (« Références ») : bleues, rouges quand elles ne sont plus respectées.
static const ColorRGBA MODIFIER_DIMENSION_COLOR       = {0.15f, 0.55f, 1.0f, 1.0f};
static const ColorRGBA MODIFIER_DIMENSION_ERROR_COLOR = {1.0f, 0.25f, 0.25f, 1.0f};
// Plages de hauteur (de la liste d'objets, ou d'un ancien projet) : bleu-vert.
static const ColorRGBA MODIFIER_RANGE_COLOR = {0.25f, 0.75f, 0.85f, 1.0f};

// Formes proposées, dans l'ordre du menu « Ajouter un modificateur » d'origine, puis l'intervalle (l'ancienne
// nature « Plage de hauteur »), « Depuis une face », Charger et SVG. Pas le Disque : un cylindre de 0,3 mm, que
// le Cylindre fait aussi (un disque déjà posé reste un cylindre pour le panneau).
static const GLGizmoModifiers::Shape PANEL_SHAPES[] = {
    GLGizmoModifiers::Shape::Cube,  GLGizmoModifiers::Shape::Cylinder, GLGizmoModifiers::Shape::Sphere, GLGizmoModifiers::Shape::Cone,
    GLGizmoModifiers::Shape::DoubleTearRomboid, GLGizmoModifiers::Shape::Torus, GLGizmoModifiers::Shape::RoundedRectangle,
    GLGizmoModifiers::Shape::Interval, GLGizmoModifiers::Shape::Face, GLGizmoModifiers::Shape::Loaded, GLGizmoModifiers::Shape::Svg};

// Forme montrée par le panneau pour un volume posé.
static GLGizmoModifiers::Shape panel_shape(GLGizmoModifiers::Shape shape)
{
    return shape == GLGizmoModifiers::Shape::Disc ? GLGizmoModifiers::Shape::Cylinder : shape;
}

static bool is_round_shape(GLGizmoModifiers::Shape shape)
{
    return shape == GLGizmoModifiers::Shape::Cylinder || shape == GLGizmoModifiers::Shape::Cone || shape == GLGizmoModifiers::Shape::Disc ||
           shape == GLGizmoModifiers::Shape::Torus;
}

// (un intervalle n'en est pas une : sa tranche ne devient pas un cube, ni l'inverse ; une face non plus)
static bool is_primitive_shape(GLGizmoModifiers::Shape shape)
{
    return shape != GLGizmoModifiers::Shape::Free && shape != GLGizmoModifiers::Shape::Loaded && shape != GLGizmoModifiers::Shape::Svg &&
           shape != GLGizmoModifiers::Shape::Interval && shape != GLGizmoModifiers::Shape::Face && shape != GLGizmoModifiers::Shape::Count;
}

// cadenas de sa ligne : ni pris au clic sur la pièce, ni sélectionné ou modifié dans l'outil
static bool is_locked(const ModelVolume &volume) { return volume.modifier_placement.has_value() && volume.modifier_placement->locked; }

// « Mesurer » de l'outil : la Mesure de la barre (mêmes éléments à choisir, mêmes cotes en 3D), sans
// rien modifier (ni « mettre à l'échelle » ni champs X / Y / Z qui déplacent) ; ses résultats
// s'affichent dans la fenêtre de l'outil. Elle n'est pas l'outil courant du gestionnaire : l'outil
// « Modificateurs » lui passe la souris, les touches, la sélection et le rendu.
class GLGizmoModifierMeasure : public GLGizmoMeasure
{
public:
    explicit GLGizmoModifierMeasure(GLCanvas3D &parent) : GLGizmoMeasure(parent, (unsigned int) GLGizmosManager::Measure) { m_allow_editing = false; }

    // éléments choisis et cotes, dans la fenêtre en cours
    void render_panel()
    {
        init_render_input_window();
        show_selection_ui();
        show_distance_xyz_ui();
    }

protected:
    std::string on_get_name_str() override { return ""; } // pas de suivi de durée d'usage (c'est la Mesure de la barre qui compte)
};

GLGizmoModifiers::GLGizmoModifiers(GLCanvas3D &parent, unsigned int sprite_id) : GLGizmoBase(parent, sprite_id) {}

GLGizmoModifiers::~GLGizmoModifiers() = default;

bool GLGizmoModifiers::on_init()
{
    // pas de raccourci clavier pour le moment (demande de l'utilisateur)
    m_shortcut_key = 0;
    return true;
}

std::string GLGizmoModifiers::get_icon_filename(bool b_dark_mode) const
{
    return b_dark_mode ? "toolbar_modifiers_dark.svg" : "toolbar_modifiers.svg";
}

std::string GLGizmoModifiers::on_get_name() const
{
    return _u8L("Modifiers");
}

// Un seul objet, en Préparation, pour une imprimante à filament.
bool GLGizmoModifiers::on_is_activable() const
{
    if (wxGetApp().plater()->printer_technology() != ptFFF)
        return false;
    const Selection &selection = m_parent.get_selection();
    if (selection.is_empty() || selection.is_wipe_tower() || !selection.is_from_single_object())
        return false;
    const int object_idx = selection.get_object_idx();
    return object_idx >= 0 && object_idx < int(wxGetApp().plater()->model().objects.size());
}

void GLGizmoModifiers::on_set_state()
{
    if (m_state == On) {
        m_align       = AlignStep::None;
        m_placing     = false;
        m_measuring   = false;
        m_ref_step    = RefStep::None;
        m_warning.clear();
        // plus de plans d'une plage choisie avant l'outil : il dessine lui-même les plages
        m_parent.handle_sidebar_focus_event("", false);
        data_changed(false);
    } else {
        stop_measure();
        stop_references();
        m_drag.reset();
        m_align = AlignStep::None;
        m_hover = SurfacePoint();
        m_hover_tool = -1;
        m_parts.clear();
        m_tools.clear();
        m_pick_cache.clear();
        m_face = FaceData();
        m_face_selection = FaceSelection();
        m_mesh_models.clear();
        m_plane_models.clear();
        m_interval_outlines.clear();
        m_ghost_outlines = IntervalOutlines();
        m_range_outlines = IntervalOutlines();
        m_interval_centers.clear();
        m_current_range.reset();
        m_range_buffer.reset();
        m_synced_volume_id = size_t(-1);
        m_synced_range.reset();
        m_edit_snapshot  = false;
        m_pending_finish = false;
        m_placing        = false;
        m_circle_model.reset();
        m_handle_mode  = HandleMode::None;
        m_handle_hover = -1;
        m_handle_drag.reset();
        for (std::shared_ptr<GLModel> &ring : m_ring_models)
            ring.reset();
    }
    GLGizmoBase::on_set_state();
}

// La sélection a changé (clic, liste d'objets, annuler / rétablir).
void GLGizmoModifiers::data_changed(bool is_serializing)
{
    m_drag.reset();
    m_left_down_unused = false;
    m_hover            = SurfacePoint();
    m_hover_tool       = -1;
    m_synced_volume_id = size_t(-1);
    sync_with_selection();
    if (m_measuring && m_measure)
        m_measure->data_changed(is_serializing);
    m_parent.set_as_dirty();
}

// Le panneau suit la sélection : il reprend la nature, la forme et les cotes du volume sélectionné
// (comme les connecteurs de la coupe). Appelé à chaque événement : la liste peut changer la sélection
// sans prévenir.
void GLGizmoModifiers::sync_with_selection()
{
    // « Références » : pour le volume où elles ont commencé seulement
    if (m_ref_step != RefStep::None) {
        const ModelVolume *volume = current_volume();
        if (volume == nullptr || volume->id() != m_ref_volume_id)
            stop_references();
    }
    std::optional<t_layer_height_range> range;
    if (ObjectList *list = wxGetApp().obj_list()) {
        const wxDataViewItem item = list->GetSelection();
        if (item.IsOk() && (list->GetModel()->GetItemType(item) & itLayer) != 0)
            range = list->GetModel()->GetLayerRangeByItem(item);
    }
    if (range != m_current_range) {
        m_current_range = range;
        m_range_buffer.reset();
    }
    const ModelVolume *volume = current_volume();
    const size_t       id     = volume != nullptr ? volume->id().id : 0;
    if (id == m_synced_volume_id && m_current_range == m_synced_range)
        return;
    m_synced_volume_id = id;
    m_synced_range     = m_current_range;
    if (m_current_range.has_value())
        m_align = AlignStep::None;
    else if (volume != nullptr && volume->is_modifier_tool_volume()) {
        m_nature                          = nature_of(*volume);
        const ModifierPlacement placement = placement_of(*volume);
        if (is_primitive_shape(placement.shape) || placement.shape == Shape::Interval)
            m_shape = panel_shape(placement.shape);
    } else
        m_align = AlignStep::None;
}

// =============================================================================================
// Objet édité
// =============================================================================================

ModelObject *GLGizmoModifiers::model_object() const
{
    const int idx = m_parent.get_selection().get_object_idx();
    Model    &model = wxGetApp().plater()->model();
    return (idx >= 0 && idx < int(model.objects.size())) ? model.objects[idx] : nullptr;
}

int GLGizmoModifiers::object_index() const
{
    return m_parent.get_selection().get_object_idx();
}

int GLGizmoModifiers::instance_index() const
{
    const ModelObject *mo  = model_object();
    const int          idx = m_parent.get_selection().get_instance_idx();
    if (mo == nullptr || mo->instances.empty())
        return -1;
    return (idx >= 0 && idx < int(mo->instances.size())) ? idx : 0;
}

Transform3d GLGizmoModifiers::instance_matrix() const
{
    const ModelObject *mo  = model_object();
    const int          idx = instance_index();
    return (mo != nullptr && idx >= 0) ? Transform3d(mo->instances[idx]->get_matrix()) : Transform3d::Identity();
}

// Volume de l'objet sélectionné seul (liste d'objets, clic dans l'outil), s'il figure au tableau et n'est pas
// verrouillé (choisi dans la liste d'objets, l'outil ne le modifie pas).
ModelVolume *GLGizmoModifiers::current_volume() const
{
    const Selection &selection = m_parent.get_selection();
    if (selection.get_mode() != Selection::Volume || selection.get_volume_idxs().size() != 1)
        return nullptr;
    const GLVolume *gl_volume = selection.get_first_volume();
    ModelObject    *mo        = model_object();
    if (gl_volume == nullptr || mo == nullptr)
        return nullptr;
    const int volume_idx = gl_volume->volume_idx();
    if (volume_idx < 0 || volume_idx >= int(mo->volumes.size()))
        return nullptr;
    ModelVolume *volume = mo->volumes[volume_idx];
    return is_tool_listed(*volume) && !is_locked(*volume) ? volume : nullptr;
}

ModelVolume *GLGizmoModifiers::volume_by_id(const ObjectID &id) const
{
    if (ModelObject *mo = model_object())
        for (ModelVolume *volume : mo->volumes)
            if (volume->id() == id)
                return volume;
    return nullptr;
}

// Ce que montre le tableau : tout ce qui s'ajoute aux pièces de l'objet (modificateurs, volumes
// négatifs, supports, pièces posées par l'outil, textes et SVG), pas les connecteurs de coupe.
bool GLGizmoModifiers::is_tool_listed(const ModelVolume &volume) const
{
    return !volume.is_cut_connector() && (!volume.is_model_part() || volume.modifier_placement.has_value() || volume.is_text() || volume.is_svg());
}

int GLGizmoModifiers::solid_parts_count() const
{
    int count = 0;
    if (const ModelObject *mo = model_object())
        for (const ModelVolume *volume : mo->volumes)
            if (volume->is_model_part())
                ++count;
    return count;
}

// =============================================================================================
// Formes
// =============================================================================================

GLGizmoModifiers::Nature GLGizmoModifiers::nature_of(const ModelVolume &volume)
{
    switch (volume.type()) {
    case ModelVolumeType::MODEL_PART: return Nature::Join;
    case ModelVolumeType::NEGATIVE_VOLUME: return Nature::Cut;
    case ModelVolumeType::SUPPORT_BLOCKER: return Nature::SupportBlocker;
    case ModelVolumeType::SUPPORT_ENFORCER: return Nature::SupportEnforcer;
    default: return Nature::Modifier;
    }
}

ModelVolumeType GLGizmoModifiers::volume_type(Nature nature)
{
    switch (nature) {
    case Nature::Join: return ModelVolumeType::MODEL_PART;
    case Nature::Cut: return ModelVolumeType::NEGATIVE_VOLUME;
    case Nature::SupportBlocker: return ModelVolumeType::SUPPORT_BLOCKER;
    case Nature::SupportEnforcer: return ModelVolumeType::SUPPORT_ENFORCER;
    case Nature::Modifier: return ModelVolumeType::PARAMETER_MODIFIER;
    default: return ModelVolumeType::INVALID;
    }
}

// De la matière ajoutée sort de la pièce ; le reste (trou, modificateur, supports) y entre.
bool GLGizmoModifiers::default_outside(Nature nature)
{
    return nature == Nature::Join;
}

const TriangleMesh *GLGizmoModifiers::shape_mesh(Shape shape)
{
    if (shape == Shape::Loaded)
        return m_loaded_mesh.get();
    if (auto it = m_shape_meshes.find(shape); it != m_shape_meshes.end())
        return it->second.get();
    TriangleMesh mesh;
    auto         read_stl = [&mesh](const char *name) { mesh.ReadSTLFile((Slic3r::resources_dir() + "/model/" + name).c_str(), true, nullptr); };
    switch (shape) {
    case Shape::Cube:
    case Shape::Interval: mesh = TriangleMesh(its_make_cube(1., 1., 1.)); break; // (une tranche : un cube étiré)
    case Shape::Cylinder: mesh = TriangleMesh(its_make_cylinder(0.5, 1.)); break;
    case Shape::Sphere: mesh = TriangleMesh(its_make_sphere(0.5, PI / 36.)); break;
    case Shape::Cone: mesh = TriangleMesh(its_make_cone(0.5, 1.)); break;
    case Shape::DoubleTearRomboid: read_stl("double_tear_romboid_cylinder.stl"); break;
    case Shape::Disc: read_stl("Disc.stl"); break;
    case Shape::Torus: read_stl("torus.stl"); break;
    case Shape::RoundedRectangle: read_stl("rounded_rectangle.stl"); break;
    default: return nullptr;
    }
    if (mesh.its.indices.empty())
        return nullptr;
    auto ptr             = std::make_shared<TriangleMesh>(std::move(mesh));
    m_shape_meshes[shape] = ptr;
    return ptr.get();
}

std::shared_ptr<const TriangleMesh> GLGizmoModifiers::panel_mesh()
{
    if (m_shape == Shape::Loaded)
        return m_loaded_mesh;
    if (shape_mesh(m_shape) == nullptr)
        return nullptr;
    return m_shape_meshes[m_shape];
}

// Taille de départ d'une forme : ses proportions, la plus grande cote au tiers de la pièce (entre
// 2 mm et 10 % du plateau) ; un fichier chargé garde sa taille.
Vec3d GLGizmoModifiers::default_size(Shape shape)
{
    if (shape == Shape::Loaded && m_loaded_mesh)
        return m_loaded_mesh->bounding_box().size();
    double side = m_parent.get_size_proportional_to_max_bed_size(0.1);
    if (const ModelObject *mo = model_object(); mo != nullptr && instance_index() >= 0) {
        const Vec3d object_size = mo->instance_bounding_box(size_t(instance_index())).size();
        side                    = std::clamp(0.3 * object_size.maxCoeff(), 2.0, std::max(side, 2.0));
    }
    side = std::round(side * 10.) / 10.;
    const TriangleMesh *mesh = shape_mesh(shape);
    if (mesh == nullptr)
        return Vec3d::Constant(side);
    const Vec3d  size    = mesh->bounding_box().size();
    const double largest = size.maxCoeff();
    return largest > 1e-9 ? Vec3d(size * (side / largest)) : Vec3d::Constant(side);
}

wxString GLGizmoModifiers::shape_label(Shape shape) const
{
    switch (shape) {
    case Shape::Cube: return _L("Cube");
    case Shape::Cylinder: return _L("Cylinder");
    case Shape::Sphere: return _L("Sphere");
    case Shape::Cone: return _L("Cone");
    case Shape::DoubleTearRomboid: return _L("Double Tear Romboid Cylinder");
    case Shape::Disc: return _L("Disc");
    case Shape::Torus: return _L("Torus");
    case Shape::RoundedRectangle: return _L("Rounded Rectangle");
    case Shape::Loaded: return _L("Load a file");
    case Shape::Svg: return _L("SVG");
    case Shape::Interval: return _L("Interval");
    case Shape::Face: return _L("From a face");
    default: return _L("Free shape");
    }
}

// Image d'une forme, celle de l'ancien menu « Ajouter un modificateur » (resources/model/<nom>.png),
// chargée au premier besoin ; aucune pour un fichier chargé ou un SVG.
ImTextureID GLGizmoModifiers::shape_image(Shape shape)
{
    static const std::map<Shape, std::string> FILES = {{Shape::Cube, "Cube"},
                                                       {Shape::Cylinder, "Cylinder"},
                                                       {Shape::Sphere, "Sphere"},
                                                       {Shape::Cone, "Cone"},
                                                       {Shape::DoubleTearRomboid, "double_tear_romboid_cylinder"},
                                                       {Shape::Torus, "Torus"},
                                                       {Shape::RoundedRectangle, "rounded_rectangle"}};
    const auto file = FILES.find(shape);
    if (file == FILES.end())
        return nullptr;
    const auto [texture, created] = m_shape_images.try_emplace(shape);
    if (created)
        texture->second.load_from_file(Slic3r::resources_dir() + "/model/" + file->second + ".png", true, GLTexture::None, false);
    return texture->second.get_id() != 0 ? ImTextureID(intptr_t(texture->second.get_id())) : nullptr;
}

// Ancien nom donné au volume, comme l'ancien menu « Ajouter un modificateur » (« Générique-Cube ») : reconnu comme un nom
// par défaut (is_default_name).
wxString GLGizmoModifiers::generic_name(Shape shape) const
{
    return _L("Generic") + "-" + shape_label(shape);
}

// Nature dans le nom d'un volume (un nom, pas le verbe du menu Nature).
wxString GLGizmoModifiers::nature_name(Nature nature)
{
    switch (nature) {
    case Nature::Join: return _L("Addition");
    case Nature::Cut: return _L("Cutout");
    case Nature::SupportBlocker: return _L("Blocker");
    case Nature::SupportEnforcer: return _L("Enforcer");
    default: return _L("Modifier");
    }
}

// Forme dans le nom d'un volume : son nom dans le menu Forme ; un fichier chargé, son nom sans extension ; « Depuis
// une face », « Face ».
std::string GLGizmoModifiers::shape_name(const ModelVolume &volume) const
{
    const Shape shape = placement_of(volume).shape;
    if (shape == Shape::Loaded && !volume.source.input_file.empty())
        return boost::filesystem::path(volume.source.input_file).stem().string();
    if (shape == Shape::Face)
        return into_u8(_L("Face"));
    return into_u8(shape_label(shape));
}

// Nom par défaut d'un volume de l'outil : [FORME-NATURE-NUMÉRO] (« Cube-Modificateur-2 »), le premier numéro libre dans
// l'objet.
std::string GLGizmoModifiers::default_name(const std::string &shape_text, Nature nature) const
{
    const std::string  prefix = shape_text + "-" + into_u8(nature_name(nature)) + "-";
    const ModelObject *mo     = model_object();
    for (int number = 1;; ++number) {
        const std::string name = prefix + std::to_string(number);
        if (mo == nullptr || std::none_of(mo->volumes.begin(), mo->volumes.end(), [&name](const ModelVolume *v) { return v->name == name; }))
            return name;
    }
}

// Nom que l'outil a donné (l'utilisateur ne l'a pas changé) : [FORME-NATURE-NUMÉRO], ou l'ancien « Générique-Forme » ; il
// suit alors un changement de forme ou de nature.
bool GLGizmoModifiers::is_default_name(const std::string &name, const std::string &shape_text, Shape shape, Nature nature) const
{
    if (name == into_u8(generic_name(shape)))
        return true;
    const std::string prefix = shape_text + "-" + into_u8(nature_name(nature)) + "-";
    return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
           std::all_of(name.begin() + prefix.size(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// =============================================================================================
// Repères : un volume est posé par son point d'ancrage (centre du dessous de la boîte de son
// maillage), sa rotation et son échelle le long de ses axes, le tout dans le monde.
// =============================================================================================

Vec3d GLGizmoModifiers::base_point(const BoundingBoxf3 &mesh_box)
{
    const Vec3d center = mesh_box.center();
    return {center.x(), center.y(), mesh_box.min.z()};
}

GLGizmoModifiers::Pose GLGizmoModifiers::decompose(const Transform3d &world, const BoundingBoxf3 &mesh_box)
{
    Pose           pose;
    const Matrix3d linear = world.linear();
    for (int i = 0; i < 3; ++i) {
        const double s = linear.col(i).norm();
        pose.scale(i)  = s > 1e-12 ? s : 1.;
        pose.rotation.col(i) = s > 1e-12 ? Vec3d(linear.col(i) / s) : Vec3d(Vec3d::Unit(i));
    }
    if (pose.rotation.determinant() < 0.) { // volume en miroir : le miroir passe dans l'échelle
        pose.rotation.col(0) = -pose.rotation.col(0);
        pose.scale(0)        = -pose.scale(0);
    }
    // rotation propre, sans les petites dérives des calculs successifs
    const Vec3d x = pose.rotation.col(0).normalized();
    const Vec3d y = (pose.rotation.col(1) - pose.rotation.col(1).dot(x) * x).normalized();
    pose.rotation.col(0) = x;
    pose.rotation.col(1) = y;
    pose.rotation.col(2) = x.cross(y);
    pose.anchor          = world * base_point(mesh_box);
    return pose;
}

Transform3d GLGizmoModifiers::compose(const Pose &pose, const BoundingBoxf3 &mesh_box)
{
    Transform3d world   = Transform3d::Identity();
    world.linear()      = pose.rotation * pose.scale.asDiagonal();
    world.translation() = pose.anchor - world.linear() * base_point(mesh_box);
    return world;
}

// Repère posé sur une surface : Z vers l'intérieur de la pièce (ou vers l'extérieur pour Joindre),
// X le long de la référence de la face.
Matrix3d GLGizmoModifiers::surface_frame(const Vec3d &normal, const Vec3d &xref, bool outside)
{
    const Vec3d z = (outside ? normal : Vec3d(-normal)).normalized();
    Vec3d       x = xref - xref.dot(z) * z;
    if (x.squaredNorm() < 1e-10)
        x = z.cross(std::abs(z.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY());
    x.normalize();
    Matrix3d frame;
    frame.col(0) = x;
    frame.col(1) = z.cross(x);
    frame.col(2) = z;
    return frame;
}

Matrix3d GLGizmoModifiers::rotation_z(double degrees)
{
    return Eigen::AngleAxisd(degrees * PI / 180., Vec3d::UnitZ()).toRotationMatrix();
}

Transform3d GLGizmoModifiers::volume_world_matrix(const ModelVolume &volume) const
{
    return instance_matrix() * volume.get_matrix();
}

Vec3d GLGizmoModifiers::volume_size(const ModelVolume &volume) const
{
    const BoundingBoxf3 box  = volume.mesh().bounding_box();
    const Pose          pose = decompose(volume_world_matrix(volume), box);
    return box.size().cwiseProduct(pose.scale.cwiseAbs());
}

// Nouvelle matrice (monde) d'un volume : écrite dans le modèle et dans les volumes affichés, sans
// recharger la scène (fait à la fin du geste).
void GLGizmoModifiers::set_volume_world_matrix(ModelVolume &volume, const Transform3d &world)
{
    // jamais une pose absurde (non finie, ou à plus de 10 m) : la scène grandirait d'autant et la caméra
    // reculerait sans retour (vue grise, barre d'outils perdue)
    if (!world.matrix().allFinite() || world.translation().cwiseAbs().maxCoeff() > 1e4)
        return;
    volume.set_transformation(Transform3d(instance_matrix().inverse() * world));
    sync_gl_volumes(volume);
}

void GLGizmoModifiers::sync_gl_volumes(const ModelVolume &volume)
{
    const ModelObject *mo = model_object();
    if (mo == nullptr)
        return;
    const int object_idx = object_index();
    int       volume_idx = -1;
    for (size_t i = 0; i < mo->volumes.size(); ++i)
        if (mo->volumes[i] == &volume)
            volume_idx = int(i);
    if (volume_idx < 0)
        return;
    for (GLVolume *gl_volume : m_parent.get_volumes().volumes)
        if (gl_volume->object_idx() == object_idx && gl_volume->volume_idx() == volume_idx)
            gl_volume->set_volume_transformation(volume.get_transformation());
    m_parent.set_as_dirty();
}

// Les poses se calculent dans le monde : une instance tournée ET étirée inégalement déformerait
// les volumes. Comme l'ancien ajout de modificateur, on reporte alors sa transformation dans les
// volumes (à appeler après l'instantané d'annulation).
void GLGizmoModifiers::ensure_rigid_instance()
{
    ModelObject *mo = model_object();
    if (mo == nullptr)
        return;
    const Matrix3d linear = instance_matrix().linear();
    const Matrix3d gram   = linear.transpose() * linear;
    const double   k      = gram.trace() / 3.;
    if ((gram - k * Matrix3d::Identity()).norm() <= 1e-6 * std::max(k, 1.))
        return;
    // (la fonction de la liste d'objets reporte la transformation de la 1re instance)
    const Transform3d old_matrix = mo->instances.front()->get_matrix();
    wxGetApp().obj_list()->apply_object_instance_transfrom_to_all_volumes(mo);
    // les références à la pièce sont dans le repère de l'objet, qui vient de changer
    rebase_dimensions(*mo, Transform3d(mo->instances.front()->get_matrix().inverse() * old_matrix));
}

bool GLGizmoModifiers::instance_is_rigid() const
{
    const Matrix3d linear = instance_matrix().linear();
    const Matrix3d gram   = linear.transpose() * linear;
    const double   k      = gram.trace() / 3.;
    return (gram - k * Matrix3d::Identity()).norm() <= 1e-6 * std::max(k, 1.);
}

ModifierPlacement GLGizmoModifiers::placement_of(const ModelVolume &volume) const
{
    return volume.modifier_placement.has_value() ? *volume.modifier_placement : ModifierPlacement();
}

// Repère posé (sans l'angle ni l'inclinaison) d'un volume : rotation = posé × Rz(angle) × inclinaison.
Matrix3d GLGizmoModifiers::posed_frame(const Pose &pose, const ModifierPlacement &placement) const
{
    return pose.rotation * placement.tilt.transpose() * rotation_z(-placement.angle);
}

// =============================================================================================
// Survol, ancrages
// =============================================================================================

// Maillages de l'objet pour les rayons : les pièces pleines (où l'on pose) et les volumes de
// l'outil (que l'on prend pour les déplacer). Les lanceurs de rayons ne sont refaits que pour un
// nouveau maillage.
void GLGizmoModifiers::update_picks()
{
    m_parts.clear();
    m_tools.clear();
    ModelObject *mo = model_object();
    if (mo == nullptr || instance_index() < 0) {
        m_pick_cache.clear();
        return;
    }
    const Transform3d              instance = instance_matrix();
    std::set<const TriangleMesh *> used;
    for (const ModelVolume *volume : mo->volumes) {
        const bool tool = volume->is_modifier_tool_volume();
        if ((!tool && !volume->is_model_part()) || volume->is_interval()) // (la tranche d'un intervalle, invisible, couvre la pièce)
            continue;
        PickMesh pick;
        pick.volume_id = volume->id();
        pick.mesh      = volume->mesh_ptr();
        pick.trafo     = instance * volume->get_matrix();
        if (pick.mesh == nullptr || pick.mesh->its.indices.empty())
            continue;
        PickCache &cached = m_pick_cache[pick.mesh];
        if (!cached.raycaster) {
            cached.mesh      = volume->get_mesh_shared_ptr();
            cached.raycaster = std::make_shared<MeshRaycaster>(*pick.mesh);
        }
        pick.raycaster = cached.raycaster;
        pick.measuring = cached.measuring;
        used.insert(pick.mesh);
        (tool ? m_tools : m_parts).push_back(std::move(pick));
    }
    for (auto it = m_pick_cache.begin(); it != m_pick_cache.end();) {
        if (used.count(it->first) > 0) {
            ++it;
            continue;
        }
        it = m_pick_cache.erase(it);
        m_plane_models.clear(); // (rangées par adresse de maillage)
    }
}

// Faces planes d'un maillage (moteur de l'outil Mesure), calculées au premier survol.
std::shared_ptr<Measure::Measuring> GLGizmoModifiers::measuring_of(PickMesh &pick)
{
    if (!pick.measuring) {
        PickCache &cached = m_pick_cache[pick.mesh];
        if (!cached.measuring) {
            wxBusyCursor wait;
            cached.measuring = std::make_shared<Measure::Measuring>(pick.mesh->its);
        }
        pick.measuring = cached.measuring;
    }
    return pick.measuring;
}

// Centre d'une face plane (barycentre de ses triangles), dans le monde.
std::optional<Vec3d> GLGizmoModifiers::face_centroid(PickMesh &pick, int plane)
{
    const indexed_triangle_set &its    = pick.mesh->its;
    Vec3d                       sum    = Vec3d::Zero();
    double                      weight = 0.;
    for (int t : measuring_of(pick)->get_plane_triangle_indices(plane)) {
        if (t < 0 || t >= int(its.indices.size()))
            continue;
        const Vec3d  a    = pick.trafo * its.vertices[its.indices[t][0]].cast<double>();
        const Vec3d  b    = pick.trafo * its.vertices[its.indices[t][1]].cast<double>();
        const Vec3d  c    = pick.trafo * its.vertices[its.indices[t][2]].cast<double>();
        const double area = 0.5 * (b - a).cross(c - a).norm();
        sum += area * (a + b + c) / 3.;
        weight += area;
    }
    if (weight <= 1e-12)
        return std::nullopt;
    return Vec3d(sum / weight);
}

// Sommets (extrémités des arêtes de ses faces planes) et centre de la matière d'un maillage, dans son
// repère, calculés une fois (Maj pendant « Joindre »).
const GLGizmoModifiers::PickCache &GLGizmoModifiers::corners_of(PickMesh &pick)
{
    const std::shared_ptr<Measure::Measuring> measuring = measuring_of(pick);
    PickCache                                &cached    = m_pick_cache[pick.mesh];
    if (cached.corners_ready)
        return cached;
    cached.corners_ready = true;
    // centre de la matière : somme des tétraèdres (origine, triangle) ; maillage ouvert : centre de sa boîte
    const indexed_triangle_set &its    = pick.mesh->its;
    double                      volume = 0.;
    Vec3d                       moment = Vec3d::Zero();
    for (const auto &triangle : its.indices) {
        const Vec3d  a = its.vertices[triangle[0]].cast<double>();
        const Vec3d  b = its.vertices[triangle[1]].cast<double>();
        const Vec3d  c = its.vertices[triangle[2]].cast<double>();
        const double v = a.dot(b.cross(c)) / 6.;
        volume += v;
        moment += v * (a + b + c) / 4.;
    }
    cached.barycentre = std::abs(volume) > 1e-12 ? Vec3d(moment / volume) : pick.mesh->bounding_box().center();
    // ponytail: sommets des faces planes jusqu'à 2000 faces ; au-delà (maillage courbe très fin), le centre seul
    if (measuring->get_num_of_planes() <= 2000) {
        std::set<std::array<long long, 3>> seen;
        for (int i = 0; i < measuring->get_num_of_planes(); ++i)
            for (const Measure::SurfaceFeature &f : measuring->get_plane_features(unsigned(i))) {
                if (f.get_type() != Measure::SurfaceFeatureType::Edge)
                    continue;
                const auto [a, b] = f.get_edge();
                for (const Vec3d &p : {a, b}) {
                    const std::array<long long, 3> key{std::llround(p.x() * 1e4), std::llround(p.y() * 1e4), std::llround(p.z() * 1e4)};
                    if (seen.insert(key).second)
                        cached.corners.push_back(p);
                }
            }
    }
    return cached;
}

// Points d'une forme ronde sur ses plans YZ, ZX et XY passant par le centre de sa boîte (repère du
// maillage) : ses sommets sur ces plans et les arêtes qui les traversent, avec la normale de la surface
// là (moyenne des faces voisines), calculés une fois.
const GLGizmoModifiers::PickCache &GLGizmoModifiers::principal_of(PickMesh &pick)
{
    PickCache &cached = m_pick_cache[pick.mesh];
    if (cached.principal_ready)
        return cached;
    cached.principal_ready            = true;
    const indexed_triangle_set &its    = pick.mesh->its;
    const BoundingBoxf3         box    = pick.mesh->bounding_box();
    const Vec3d                 center = box.center();
    const double                eps    = 1e-4 * std::max(box.max_size(), 1e-9);
    std::vector<Vec3d>          normals(its.vertices.size(), Vec3d::Zero()); // aux sommets, pondérées par l'aire
    for (const auto &triangle : its.indices) {
        const Vec3d a = its.vertices[triangle[0]].cast<double>();
        const Vec3d n = (its.vertices[triangle[1]].cast<double>() - a).cross(its.vertices[triangle[2]].cast<double>() - a);
        for (int k = 0; k < 3; ++k)
            normals[triangle[k]] += n;
    }
    std::set<std::array<long long, 4>> seen;
    auto add = [&](const Vec3d &p, const Vec3d &n, int axis) {
        const std::array<long long, 4> key{std::llround(p.x() / eps), std::llround(p.y() / eps), std::llround(p.z() / eps), axis};
        if (n.norm() > 1e-12 && seen.insert(key).second)
            cached.principal.push_back({p, n.normalized(), axis});
    };
    for (int axis = 0; axis < 3; ++axis) {
        for (size_t v = 0; v < its.vertices.size(); ++v)
            if (std::abs(double(its.vertices[v](axis)) - center(axis)) < eps)
                add(its.vertices[v].cast<double>(), normals[v], axis);
        for (const auto &triangle : its.indices)
            for (int k = 0; k < 3; ++k) {
                const int    i0 = triangle[k], i1 = triangle[(k + 1) % 3];
                const Vec3d  a  = its.vertices[i0].cast<double>(), b = its.vertices[i1].cast<double>();
                const double da = a(axis) - center(axis), db = b(axis) - center(axis);
                if (da * db >= 0. || std::abs(da) < eps || std::abs(db) < eps)
                    continue;
                const double s = da / (da - db);
                add(a + s * (b - a), (1. - s) * normals[i0] + s * normals[i1], axis);
            }
    }
    return cached;
}

// Anneaux d'une forme ronde de l'outil (sphère, cylindre, cône, disque, tore) : ses points sur ses plans YZ, ZX et XY
// (monde, avec la normale de la surface là), montrés par « Joindre » et par les références avec Maj ; aucun pour une
// autre forme (cône : sans l'anneau à mi-hauteur, qui ne sert à rien).
std::vector<GLGizmoModifiers::PrincipalPoint> GLGizmoModifiers::ring_points(PickMesh &pick)
{
    std::vector<PrincipalPoint> points;
    const ModelVolume          *owner = volume_by_id(pick.volume_id);
    const Shape shape = owner != nullptr && owner->is_modifier_tool_volume() && owner->modifier_placement.has_value() ? owner->modifier_placement->shape : Shape::Free;
    if (shape != Shape::Sphere && !is_round_shape(shape))
        return points;
    const Matrix3d normal_to_world = pick.trafo.linear().inverse().transpose();
    for (const PrincipalPoint &q : principal_of(pick).principal)
        if (shape != Shape::Cone || q.axis != 2)
            points.push_back({Vec3d(pick.trafo * q.point), Vec3d((normal_to_world * q.normal).normalized()), q.axis});
    return points;
}

// Taille d'un pixel de l'écran, en mm, à la profondeur de « point » (comme l'outil Coupe).
double GLGizmoModifiers::pixel_size_at(const Vec3d &point) const
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

// La souris est sur une fenêtre (celle de l'outil, une notification…) : ses clics ne sont pas pour
// la scène. Le canevas en laisse parfois passer (infobulle de la barre encore affichée) : sans ce
// garde-fou, un clic dans le tableau désélectionnait (« clic dans le vide »).
bool GLGizmoModifiers::mouse_on_imgui()
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
}

// Rayon de la souris : le maillage touché le plus près de la caméra (point et normale dans le monde).
bool GLGizmoModifiers::raycast(const std::vector<PickMesh> &meshes, const Vec2d &mouse, int &index, Vec3d &hit, Vec3d &normal, size_t &facet) const
{
    const Camera &camera = wxGetApp().plater()->get_camera();
    double        best   = std::numeric_limits<double>::max();
    index                = -1;
    for (size_t i = 0; i < meshes.size(); ++i) {
        Vec3f  position, triangle_normal;
        size_t triangle = 0;
        if (!meshes[i].raycaster->unproject_on_mesh(mouse, meshes[i].trafo, camera, position, triangle_normal, nullptr, &triangle, false))
            continue;
        const Vec3d  world = meshes[i].trafo * position.cast<double>();
        const double dist  = (camera.get_position() - world).squaredNorm();
        if (dist >= best)
            continue;
        best   = dist;
        index  = int(i);
        hit    = world;
        normal = (meshes[i].trafo.linear().inverse().transpose() * triangle_normal.cast<double>()).normalized();
        facet  = triangle;
    }
    return index >= 0;
}

// Point de la pièce sous la souris : sa face plane, l'axe de référence de cette face (longueur de
// l'îlot touché), et, avec le magnétisme, l'ancrage le plus proche à 10 pixels près : centres de
// cercles, de polygones et de trous, barycentre, point le plus éloigné des bords, centre de la
// face, foyers ; puis sommets et milieux d'arêtes ; puis subdivisions des médianes et diagonales.
GLGizmoModifiers::SurfacePoint GLGizmoModifiers::surface_under_mouse(const Vec2d &mouse, bool snap)
{
    SurfacePoint surface;
    int          part = -1;
    Vec3d        hit, normal;
    size_t       facet = 0;
    if (!raycast(m_parts, mouse, part, hit, normal, facet))
        return surface;
    surface.valid  = true;
    surface.part   = part;
    surface.facet  = int(facet);
    surface.point  = hit;
    surface.normal = normal;
    surface.xref   = std::abs(normal.dot(Vec3d::UnitX())) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();

    PickMesh                                  &pick          = m_parts[part];
    const std::shared_ptr<Measure::Measuring>  measuring     = measuring_of(pick);
    const Transform3d                         &trafo         = pick.trafo;
    const Matrix3d                             normal_matrix = trafo.linear().inverse().transpose();
    const Vec3d                                local         = trafo.inverse() * hit;
    const std::optional<Measure::SurfaceFeature> feature     = measuring->get_feature(facet, local, Transform3d::Identity(), true);
    if (feature.has_value() && feature->get_type() == Measure::SurfaceFeatureType::Plane) {
        surface.plane  = std::get<0>(feature->get_plane());
        surface.normal = (normal_matrix * std::get<1>(feature->get_plane())).normalized();
        update_face(part, surface.plane, surface.normal);
        const Vec2d p2((hit - m_face.origin).dot(m_face.axis_x), (hit - m_face.origin).dot(m_face.axis_y));
        const int   island = FaceAnchors::island_at(m_face.face, p2);
        if (island >= 0) {
            const Vec2d &u = m_face.face.islands[island].axis_u;
            surface.xref   = u.x() * m_face.axis_x + u.y() * m_face.axis_y;
        }
    }
    // Sans magnétisme (demande de l'utilisateur), la forme se pose droite dans l'espace, exactement
    // sous la souris : la normale devient l'axe du plateau le plus proche (il dit seulement de quel
    // côté de la pièce elle entre ou sort), quelle que soit la pente de la facette touchée.
    if (snap && !m_magnetism) {
        int axis = 0;
        surface.normal.cwiseAbs().maxCoeff(&axis);
        surface.normal = (surface.normal(axis) < 0. ? -1. : 1.) * Vec3d::Unit(axis);
        surface.xref   = axis == 0 ? Vec3d::UnitY() : Vec3d::UnitX();
        return surface;
    }
    if (!snap)
        return surface;

    // cercle dont le centre est retenu : son contour est surligné
    struct Circle
    {
        Vec3d  center;
        Vec3d  normal;
        double radius;
    };
    const double          tolerance = std::max(0.3, 10.0 * pixel_size_at(hit));
    int                   best_rank = std::numeric_limits<int>::max();
    double                best_dist = std::numeric_limits<double>::max();
    Vec3d                 best_point;
    wxString              best_name;
    std::optional<Circle> best_circle;
    auto consider = [&](const Vec3d &point, int rank, double dist, const wxString &name, const std::optional<Circle> &circle) {
        if (dist > tolerance || rank > best_rank || (rank == best_rank && dist >= best_dist))
            return;
        best_rank   = rank;
        best_dist   = dist;
        best_point  = point;
        best_name   = name;
        best_circle = circle;
    };
    auto to_world = [&trafo](const Vec3d &p) { return Vec3d(trafo * p); };

    if (surface.plane >= 0) {
        for (const Measure::SurfaceFeature &f : measuring->get_plane_features(surface.plane)) {
            if (f.get_type() == Measure::SurfaceFeatureType::Edge) {
                const auto [a_local, b_local] = f.get_edge();
                const Vec3d a = to_world(a_local), b = to_world(b_local);
                consider(a, 1, (hit - a).norm(), _L("Vertex of the part"), std::nullopt);
                consider(b, 1, (hit - b).norm(), _L("Vertex of the part"), std::nullopt);
                const Vec3d middle = 0.5 * (a + b);
                consider(middle, 1, (hit - middle).norm(), _L("Middle of an edge"), std::nullopt);
                if (const std::optional<Vec3d> &center = f.get_extra_point(); center.has_value()) {
                    const Vec3d c = to_world(*center);
                    consider(c, 0, (hit - c).norm(), _L("Center of a polygon"), std::nullopt);
                }
            } else if (f.get_type() == Measure::SurfaceFeatureType::Circle) {
                const auto [center_local, radius_local, normal_local] = f.get_circle();
                const Vec3d  center   = to_world(center_local);
                const Vec3d  axis     = (normal_matrix * normal_local).normalized();
                const Vec3d  side     = normal_local.cross(std::abs(normal_local.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
                const double radius   = (to_world(center_local + radius_local * side) - center).norm();
                const Vec3d  v        = hit - center;
                const double height   = v.dot(axis);
                const double rim_dist = std::hypot((v - height * axis).norm() - radius, height);
                // près du bord comme du centre : on vise le centre (renfort autour d'un trou)
                consider(center, 0, std::min(rim_dist, v.norm()), _L("Center of a circle"), Circle{center, axis, radius});
            }
        }
        if (!m_face.face.empty()) {
            const Vec2d p2((hit - m_face.origin).dot(m_face.axis_x), (hit - m_face.origin).dot(m_face.axis_y));
            for (const FaceAnchors::Anchor &anchor : m_face.face.anchors) {
                const int    rank = anchor.kind >= FaceAnchors::AnchorKind::Fraction ? 2 : 0;
                const double dist = (anchor.pos - p2).norm();
                if (dist <= tolerance)
                    consider(m_face.origin + anchor.pos.x() * m_face.axis_x + anchor.pos.y() * m_face.axis_y, rank, dist, FaceAnchors::anchor_name(anchor),
                             std::nullopt);
            }
        }
    }
    if (best_rank != std::numeric_limits<int>::max()) {
        surface.point       = best_point;
        surface.snapped     = true;
        surface.anchor_name = best_name;
        if (best_circle.has_value()) {
            surface.on_circle     = true;
            surface.circle_center = best_circle->center;
            surface.circle_normal = best_circle->normal;
            surface.circle_radius = best_circle->radius;
        }
    }
    return surface;
}

// Îlots et ancrages de la face plane survolée, dans un repère 2D posé sur elle (gardés tant que la
// face, la pièce et sa position ne changent pas).
void GLGizmoModifiers::update_face(int part, int plane, const Vec3d &normal)
{
    if (part < 0 || part >= int(m_parts.size()))
        return;
    PickMesh           &pick = m_parts[part];
    std::vector<double> key  = {double(part), double(plane), double(reinterpret_cast<std::uintptr_t>(pick.mesh))};
    const double       *data = pick.trafo.matrix().data();
    key.insert(key.end(), data, data + 16);
    if (key == m_face.key)
        return;
    m_face        = FaceData();
    m_face.key    = std::move(key);
    m_face.part   = part;
    m_face.plane  = plane;
    m_face.normal = normal;

    const std::shared_ptr<Measure::Measuring> measuring = measuring_of(pick);
    const std::vector<int>                   &triangles = measuring->get_plane_triangle_indices(plane);
    const indexed_triangle_set               &its       = pick.mesh->its;
    if (triangles.empty() || triangles.size() > 50000)
        return;
    const Vec3d reference = std::abs(normal.dot(Vec3d::UnitX())) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
    m_face.axis_x         = (reference - reference.dot(normal) * normal).normalized();
    m_face.axis_y         = normal.cross(m_face.axis_x);
    m_face.origin         = pick.trafo * its.vertices[its.indices[triangles.front()][0]].cast<double>();

    Polygons polygons;
    polygons.reserve(triangles.size());
    for (int t : triangles) {
        if (t < 0 || t >= int(its.indices.size()))
            continue;
        Polygon polygon;
        for (int k = 0; k < 3; ++k) {
            const Vec3d v = pick.trafo * its.vertices[its.indices[t][k]].cast<double>() - m_face.origin;
            polygon.points.emplace_back(Point::new_scale(Vec2d(v.dot(m_face.axis_x), v.dot(m_face.axis_y))));
        }
        const double area = polygon.area();
        if (std::abs(area) < 1.)
            continue;
        if (area < 0.)
            polygon.reverse();
        polygons.emplace_back(std::move(polygon));
    }
    if (!polygons.empty())
        m_face.face = FaceAnchors::compute(union_ex(polygons));
}

// « Depuis une face » : épaisseur du volume, élargissement d'un modificateur (les appuis d'un pont) et écart
// permis avec le triangle visé (la tolérance de l'outil Remplir des peintures).
static constexpr double FACE_DEPTH  = 2.;  // mm
static constexpr double FACE_MARGIN = 3.;  // mm
static constexpr double FACE_ANGLE  = 30.; // degrés

// Face survolée (« Depuis une face ») : les triangles reliés au triangle visé et tournés comme lui à 30° près
// (comparés à lui, pas de proche en proche : une forme ronde n'est pas prise en entier).
void GLGizmoModifiers::update_face_selection()
{
    FaceSelection &face = m_face_selection;
    if (!m_hover.valid || m_hover.part < 0 || m_hover.part >= int(m_parts.size()) || m_hover.facet < 0) {
        face = FaceSelection();
        return;
    }
    const PickMesh             &pick   = m_parts[m_hover.part];
    PickCache                  &cached = m_pick_cache[pick.mesh];
    const indexed_triangle_set &its    = pick.mesh->its;
    const bool                  same   = face.mesh == cached.mesh && face.trafo.isApprox(pick.trafo);
    if (same && face.seed == m_hover.facet)
        return;
    if (cached.neighbors.size() != its.indices.size())
        cached.neighbors = its_face_neighbors(its);
    const Matrix3d normal_matrix = pick.trafo.linear().inverse().transpose();
    auto           world_normal  = [&](int t) {
        const Vec3d a = its.vertices[its.indices[t][0]].cast<double>();
        const Vec3d n = normal_matrix * (its.vertices[its.indices[t][1]].cast<double>() - a).cross(its.vertices[its.indices[t][2]].cast<double>() - a);
        return n.norm() > 1e-12 ? Vec3d(n.normalized()) : Vec3d(Vec3d::Zero());
    };
    const Vec3d       seed  = world_normal(m_hover.facet);
    const double      limit = std::cos(FACE_ANGLE * PI / 180.);
    std::vector<char> seen(its.indices.size(), 0);
    std::vector<int>  facets{m_hover.facet};
    seen[m_hover.facet] = 1;
    for (size_t i = 0; i < facets.size(); ++i)
        for (int k = 0; k < 3; ++k) {
            const int t = cached.neighbors[facets[i]][k];
            if (t < 0 || seen[t])
                continue;
            seen[t] = 1;
            const Vec3d n = world_normal(t);
            if (n.isZero() || n.dot(seed) > limit) // (un triangle sans aire suit ses voisins)
                facets.push_back(t);
        }
    std::sort(facets.begin(), facets.end());
    if (!same || facets != face.facets)
        face.highlight.reset();
    face.mesh   = cached.mesh;
    face.trafo  = pick.trafo;
    face.seed   = m_hover.facet;
    face.facets = std::move(facets);
}

// Volume « Depuis une face » : la face survolée projetée sur son plan moyen (un modificateur élargi de 3 mm,
// pour que le pont garde ses appuis), épaissie de 2 mm vers l'intérieur de la pièce (une addition : vers
// l'extérieur) depuis son point le plus en dehors. Le maillage est fait dans le repère de la face (axe X :
// xref), la pose est ce repère.
bool GLGizmoModifiers::face_volume(const Vec3d &xref, TriangleMesh &mesh, Pose &pose) const
{
    const FaceSelection &face = m_face_selection;
    if (!face.mesh || face.facets.empty())
        return false;
    const indexed_triangle_set &its    = face.mesh->its;
    auto                        corner = [&](int t, int k) { return Vec3d(face.trafo * its.vertices[its.indices[t][k]].cast<double>()); };
    Vec3d                       normal = Vec3d::Zero(); // plan moyen : normales des triangles pondérées par leurs aires
    for (int t : face.facets)
        normal += (corner(t, 1) - corner(t, 0)).cross(corner(t, 2) - corner(t, 0));
    if (face.trafo.linear().determinant() < 0.) // (pièce en miroir : ses triangles sont retournés)
        normal = -normal;
    if (normal.norm() < 1e-12)
        return false;
    const Matrix3d frame  = surface_frame(normal.normalized(), xref, default_outside(m_nature));
    const Vec3d    origin = corner(face.facets.front(), 0);
    double         base   = std::numeric_limits<double>::max(); // départ de l'épaisseur, le long de son axe
    Polygons       triangles;
    triangles.reserve(face.facets.size());
    for (int t : face.facets) {
        Polygon triangle;
        for (int k = 0; k < 3; ++k) {
            const Vec3d v = corner(t, k) - origin;
            base          = std::min(base, v.dot(frame.col(2)));
            triangle.points.emplace_back(Point::new_scale(Vec2d(v.dot(frame.col(0)), v.dot(frame.col(1)))));
        }
        const double area = triangle.area();
        if (std::abs(area) < 1.)
            continue;
        if (area < 0.)
            triangle.reverse();
        triangles.emplace_back(std::move(triangle));
    }
    ExPolygons shape = union_ex(triangles);
    if (m_nature == Nature::Modifier)
        shape = offset_ex(shape, float(scale_(FACE_MARGIN)));
    if (shape.empty())
        return false;
    // (Slic3r:: : dans Slic3r::GUI, « Emboss » seul désigne Slic3r::GUI::Emboss, celui des polices)
    mesh = TriangleMesh(Slic3r::Emboss::polygons2model(
        shape, Slic3r::Emboss::ProjectScale(std::make_unique<Slic3r::Emboss::ProjectZ>(FACE_DEPTH / SCALING_FACTOR), SCALING_FACTOR)));
    if (mesh.empty())
        return false;
    const BoundingBoxf3 box = mesh.bounding_box(); // (son dessous : z = 0)
    pose.rotation           = frame;
    pose.scale              = Vec3d::Ones();
    pose.anchor             = origin + frame * Vec3d(box.center().x(), box.center().y(), base);
    return true;
}

// Décalage, dans le repère d'un maillage, qui fait 0,05 mm dans le monde le long de la normale d'un de ses
// triangles (le maillage d'une forme posée est une forme unité étirée à ses cotes : un décalage dans ses
// unités se voyait à des mm).
static float highlight_offset(const indexed_triangle_set &its, const Transform3d &trafo, int facet)
{
    if (facet < 0 || facet >= int(its.indices.size()))
        return 0.05f;
    const Vec3f &v0      = its.vertices[its.indices[facet][0]];
    const Vec3f  normal  = (its.vertices[its.indices[facet][1]] - v0).cross(its.vertices[its.indices[facet][2]] - v0).normalized();
    const double stretch = (trafo.linear() * normal.cast<double>()).norm();
    return stretch > 1e-9 ? float(0.05 / stretch) : 0.05f;
}

// (un volume verrouillé n'est pas pris : le clic passe à la pièce derrière, ou à la vue)
int GLGizmoModifiers::tool_volume_under_mouse(const Vec2d &mouse) const
{
    std::vector<PickMesh> unlocked;
    std::vector<int>      indices; // leur place dans m_tools
    for (size_t i = 0; i < m_tools.size(); ++i)
        if (const ModelVolume *v = volume_by_id(m_tools[i].volume_id); v == nullptr || !is_locked(*v)) {
            unlocked.push_back(m_tools[i]);
            indices.push_back(int(i));
        }
    int    index = -1;
    Vec3d  hit, normal;
    size_t facet = 0;
    return raycast(unlocked, mouse, index, hit, normal, facet) ? indices[size_t(index)] : -1;
}

// Survol : un volume de l'outil passe avant la pièce (on le prend pour le déplacer), sauf avec Maj
// pendant « Placer » (poser par-dessus) ; la pièce n'est suivie que pendant « Placer » (Maj : comme pour
// « Joindre », aussi tous les sommets et le centre de chaque volume) ; pendant « Joindre », les points et
// les faces de la forme, puis de la cible.
void GLGizmoModifiers::update_hover(const Vec2d &mouse, bool shift)
{
    m_mouse      = mouse;
    m_shift_down = shift;
    update_picks();
    m_hover_tool = -1;
    if (m_align != AlignStep::None) {
        m_hover      = SurfacePoint();
        m_join_hover = joint_under_mouse(mouse, shift);
        return;
    }
    m_join_points.clear();
    if (!(shift && m_placing))
        m_hover_tool = tool_volume_under_mouse(mouse);
    if (!m_placing) {
        m_hover = SurfacePoint();
        return;
    }
    if (m_shape == Shape::Face) { // la face entière, sans ancrage
        m_hover = surface_under_mouse(mouse, false);
        update_face_selection();
        return;
    }
    m_hover = surface_under_mouse(mouse, true); // (pas le plateau : à côté de la pièce, un clic fait tourner la vue)
    if (shift)
        if (const JointPick point = joint_under_mouse(mouse, true, true); point.valid) {
            if (!m_hover.valid) { // à côté de la pièce : posée droite
                m_hover.valid  = true;
                m_hover.normal = Vec3d::UnitZ();
            }
            if (point.normal.norm() > 0.5) // point d'une paroi courbe : sa normale
                m_hover.normal = point.normal;
            m_hover.point       = point.point;
            m_hover.snapped     = true;
            m_hover.on_circle   = false;
            m_hover.anchor_name = point.name;
        }
}

// « Joindre » (comme la Liaison de Fusion 360) : ce qui est sous la souris, sur la forme (1er clic) ou
// sur une pièce / une autre forme (2e clic). Ancrages de la face survolée, ou de la face choisie d'un
// clic hors ancrage, avec alors SA normale : sommets, milieux d'arêtes, centres des polygones et des
// cercles, centre de la face ; Maj : en plus, tous les sommets et le centre de chaque volume. Le plus
// proche à 12 pixels près l'emporte ; sinon la face elle-même (sur une surface courbe : le point).
// « placing » (« Placer » avec Maj) : seulement ces sommets et ces centres, des pièces et de toutes les
// formes.
GLGizmoModifiers::JointPick GLGizmoModifiers::joint_under_mouse(const Vec2d &mouse, bool shift, bool placing)
{
    JointPick pick;
    m_join_points.clear();
    const ModelVolume *volume = current_volume();
    if (volume == nullptr && !placing)
        return pick;
    const JointPick       lock = placing ? JointPick() : m_join_lock;
    std::vector<PickMesh> meshes;
    if (m_align == AlignStep::ShapePoint) {
        if (const PickMesh *own = pick_mesh_of(volume->id(), true))
            meshes.push_back(*own);
    } else {
        meshes = m_parts;
        for (const PickMesh &tool : m_tools)
            if (placing || tool.volume_id != volume->id())
                meshes.push_back(tool);
    }
    auto on_tool = [this](size_t i) { return m_align == AlignStep::ShapePoint || i >= m_parts.size(); };

    int    index = -1;
    Vec3d  hit, normal;
    size_t facet = 0;
    if (!placing && !meshes.empty() && raycast(meshes, mouse, index, hit, normal, facet)) {
        PickMesh  &mesh    = meshes[index];
        const auto feature = measuring_of(mesh)->get_feature(facet, mesh.trafo.inverse() * hit, Transform3d::Identity(), true);
        pick.valid         = true;
        pick.volume_id     = mesh.volume_id;
        pick.on_tool       = on_tool(size_t(index));
        pick.point         = hit;
        pick.normal        = normal;
        pick.name          = kind_label(RefKind::Point);
        if (feature.has_value() && feature->get_type() == Measure::SurfaceFeatureType::Plane) {
            pick.face   = true;
            pick.plane  = std::get<0>(feature->get_plane());
            pick.normal = (mesh.trafo.linear().inverse().transpose() * std::get<1>(feature->get_plane())).normalized();
            pick.name   = kind_label(RefKind::Plane);
        }
    }

    double   best          = 12.0; // pixels
    Vec3d    anchor;
    Vec3d    anchor_normal = Vec3d::Zero();
    wxString anchor_name;
    // un point montré (gris, ou de la couleur de l'axe normal à son plan) ; weight < 1 : prioritaire
    auto consider = [&](const Vec3d &p, const wxString &name, int axis, const Vec3d &normal, double weight) {
        m_join_points.emplace_back(p, axis);
        const double dist = weight * (to_screen(p) - mouse).norm();
        if (dist < best) {
            best          = dist;
            anchor        = p;
            anchor_name   = name;
            anchor_normal = normal;
        }
    };
    const JointPick face           = lock.valid ? lock : pick;
    const char     *plane_names[3] = {"YZ", "ZX", "XY"};
    for (size_t i = 0; i < meshes.size(); ++i) {
        PickMesh      &mesh            = meshes[i];
        auto           to_world        = [&mesh](const Vec3d &p) { return Vec3d(mesh.trafo * p); };
        const wxString vertex          = on_tool(i) ? _L("Vertex of the shape") : _L("Vertex of the part");
        // forme ronde de l'outil (sphère, cylindre, cône, tore, disque) : sur sa paroi courbe, seulement les
        // points de ses plans YZ, ZX et XY, chacun de la couleur de l'axe qui lui est normal
        const ModelVolume *owner = volume_by_id(mesh.volume_id);
        const Shape        shape = owner != nullptr && owner->is_modifier_tool_volume() && owner->modifier_placement.has_value() ?
                                       owner->modifier_placement->shape : Shape::Free;
        const bool         round          = shape == Shape::Sphere || is_round_shape(shape);
        bool               principal_done = false;
        auto               consider_principal = [&]() {
            if (principal_done)
                return;
            principal_done = true;
            for (const PrincipalPoint &q : ring_points(mesh))
                consider(q.point, format_wxstr(_L("Point of the %1% plane of the shape"), plane_names[q.axis]), q.axis, q.normal, 1.);
        };
        if (face.valid && face.plane >= 0 && mesh.volume_id == face.volume_id) {
            const Vec3d local_normal = (mesh.trafo.linear().transpose() * face.normal).normalized();
            if (round && (shape == Shape::Sphere || shape == Shape::Torus || std::abs(local_normal.z()) < 0.99))
                consider_principal(); // paroi courbe
            else
                for (const Measure::SurfaceFeature &f : measuring_of(mesh)->get_plane_features(unsigned(face.plane))) {
                    if (f.get_type() == Measure::SurfaceFeatureType::Edge) {
                        const auto [a, b] = f.get_edge();
                        consider(to_world(a), vertex, -1, Vec3d::Zero(), 1.);
                        consider(to_world(b), vertex, -1, Vec3d::Zero(), 1.);
                        consider(to_world(0.5 * (a + b)), _L("Middle of an edge"), -1, Vec3d::Zero(), 1.);
                        if (const std::optional<Vec3d> &center = f.get_extra_point(); center.has_value())
                            consider(to_world(*center), _L("Center of a polygon"), -1, Vec3d::Zero(), 1.);
                    } else if (f.get_type() == Measure::SurfaceFeatureType::Circle)
                        consider(to_world(std::get<0>(f.get_circle())), _L("Center of a circle"), -1, Vec3d::Zero(), 1.);
                }
            if (!principal_done)
                if (const std::optional<Vec3d> center = face_centroid(mesh, face.plane))
                    consider(*center, _L("Centroid of the face"), -1, Vec3d::Zero(), 1.);
        }
        if (shift) {
            const PickCache &cached = corners_of(mesh);
            if (round)
                consider_principal();
            else
                for (const Vec3d &p : cached.corners)
                    consider(to_world(p), vertex, -1, Vec3d::Zero(), 1.);
            // le centre passe devant les points qui se projettent sur lui (pôle d'une sphère vue de dessus…)
            consider(to_world(cached.barycentre), _L("Center of the volume"), -1, Vec3d::Zero(), 0.5);
        }
    }
    if (best < 12.0) {
        // un ancrage : joint selon la face choisie, sinon celle survolée (dans le vide : aucune)
        pick.valid     = true;
        pick.face      = false;
        pick.point     = anchor;
        pick.name      = anchor_name;
        pick.volume_id = face.volume_id;
        pick.on_tool   = face.on_tool;
        pick.plane     = face.plane;
        // la face choisie impose sa normale ; sinon celle de la surface au point (paroi courbe), puis la
        // face survolée
        pick.normal    = lock.valid ? lock.normal : anchor_normal.norm() > 0.5 ? anchor_normal : face.valid ? face.normal : Vec3d::Zero();
    }
    return pick;
}

// =============================================================================================
// Souris et clavier
// =============================================================================================

bool GLGizmoModifiers::on_mouse(const wxMouseEvent &mouse_event)
{
    if (m_state != On || model_object() == nullptr)
        return false;
    sync_with_selection();
    const Vec2d mouse(double(mouse_event.GetX()), double(mouse_event.GetY()));

    // souris sur une fenêtre : rien pour la scène (pas d'aperçu dessous, pas de clic « dans le vide »)
    if (mouse_on_imgui() && !m_drag.has_value()) {
        if (mouse_event.Moving() && (m_hover.valid || m_hover_tool >= 0)) {
            m_hover      = SurfacePoint();
            m_hover_tool = -1;
            m_parent.set_as_dirty();
        }
        if (mouse_event.LeftDown() || mouse_event.LeftUp())
            m_left_down_unused = false;
        if (mouse_event.LeftDown())
            return true;
        if (mouse_event.Moving() || mouse_event.LeftUp())
            return false;
    }

    m_mouse = mouse; // (Maj enfoncée sans bouger : le survol se refait là)

    // « Mesurer » : la souris est à la Mesure
    if (m_measuring && m_measure)
        return m_measure->on_mouse(mouse_event);

    // « Références » : une référence de la forme, puis une de la pièce, puis la distance au clavier
    if (m_ref_step != RefStep::None) {
        const bool picking = m_ref_step == RefStep::VolumeRef || m_ref_step == RefStep::PartRef;
        if (mouse_event.Moving()) {
            update_picks();
            m_ref_hover = picking ? pick_reference(mouse, m_ref_step == RefStep::VolumeRef, mouse_event.ShiftDown()) : PickedRef();
            m_parent.set_as_dirty();
            return false;
        }
        if (mouse_event.LeftDown()) {
            m_left_down_unused = false;
            if (mouse_event.CmdDown())
                return false;
            if (!picking) { // un clic dans la scène ferme le champ de la distance
                close_value();
                return true;
            }
            update_picks();
            const PickedRef ref = pick_reference(mouse, m_ref_step == RefStep::VolumeRef, mouse_event.ShiftDown());
            if (!ref.valid)
                return false; // dans le vide : la caméra
            m_ref_hover = PickedRef();
            if (m_ref_step == RefStep::VolumeRef) {
                m_ref_first = ref;
                m_ref_step  = RefStep::PartRef;
                m_ref_error.clear();
            } else if (ModelVolume *volume = current_volume(); volume != nullptr) {
                ModifierDimension probe;
                wxString          error;
                double            current = 0.;
                m_ref_along               = 3;
                if (make_dimension(*volume, m_ref_first, ref, m_ref_along, 0., probe, error, &current)) {
                    m_ref_second = ref;
                    m_ref_value  = current; // le champ s'ouvre sur la distance actuelle
                    m_ref_step   = RefStep::Value;
                    m_ref_edit   = -1;
                    m_ref_focus  = true;
                    m_ref_error.clear();
                } else
                    m_ref_error = error;
            }
            m_parent.set_as_dirty();
            return true;
        }
        return false; // glisser : la caméra
    }

    if (mouse_event.Moving()) {
        m_handle_hover = handle_under_mouse(mouse);
        update_hover(mouse, mouse_event.ShiftDown());
        m_parent.set_as_dirty();
        return false;
    }

    if (mouse_event.LeftDown()) {
        m_left_down_unused = false;
        // Ctrl : la caméra
        if (mouse_event.CmdDown())
            return false;
        update_hover(mouse, mouse_event.ShiftDown());
        m_warning.clear();

        // « Joindre » : un point (ou une face) de la forme, puis un de la pièce ou d'une autre forme ;
        // un clic hors ancrage sur une face la choisit (ses ancrages, joints selon elle), un 2e clic
        // hors ancrage sur elle prend la face elle-même
        if (m_align != AlignStep::None) {
            // (copies : reporter l'étirement de l'instance recharge la scène, qui remet le survol à zéro)
            const JointPick pick = m_join_hover;
            if (!pick.valid)
                return false; // dans le vide : la caméra
            const bool locked = m_join_lock.valid && pick.face && pick.volume_id == m_join_lock.volume_id && pick.plane == m_join_lock.plane;
            if (pick.face && !locked) {
                m_join_lock  = pick;
                m_join_hover = joint_under_mouse(mouse, mouse_event.ShiftDown());
                m_parent.set_as_dirty();
                return true;
            }
            m_join_lock = JointPick();
            if (m_align == AlignStep::ShapePoint) {
                m_join_first = pick;
                m_align      = AlignStep::TargetPoint;
            } else {
                const JointPick first  = m_join_first;
                ModelVolume    *volume = current_volume();
                if (volume != nullptr && volume->is_modifier_tool_volume()) {
                    const ObjectID       id = volume->id();
                    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Join modifier"));
                    ensure_rigid_instance();
                    if (ModelVolume *joined = volume_by_id(id)) {
                        // deux faces : elle tourne au plus court (autour du point cliqué sur sa face) pour que sa face soit
                        // parallèle à la cible, dans le même sens ou face à face, puis glisse ; des points : elle ne tourne pas
                        Vec3d from_normal = first.normal;
                        if (first.face && pick.face && first.normal.norm() > 1e-9 && pick.normal.norm() > 1e-9) {
                            const Vec3d from = first.normal.normalized();
                            const Vec3d to   = (from.dot(pick.normal) >= 0. ? 1. : -1.) * pick.normal.normalized();
                            const Vec3d axis = from.cross(to);
                            if (axis.norm() > 1e-9)
                                rotate_volume(*joined, volume_world_matrix(*joined), placement_of(*joined), first.point, axis.normalized(),
                                              std::atan2(axis.norm(), from.dot(to)) * 180. / PI);
                            from_normal = to;
                        }
                        align(*joined, from_normal, first.point, pick.normal, pick.point, !first.face && !pick.face);
                        rebase_on_joint(*joined, pick.normal, pick.point);
                        finish_change();
                    }
                }
                m_align = AlignStep::None;
            }
            m_join_hover = JointPick();
            m_join_points.clear();
            m_parent.set_as_dirty();
            return true;
        }

        // un anneau (tourner), une flèche (déplacer) ou un cube (échelle) de la forme sélectionnée
        if (const int axis = handle_under_mouse(mouse); axis >= 0) {
            HandleDrag drag;
            drag.volume_id   = current_volume()->id();
            drag.axis        = axis;
            drag.ring        = m_handle_mode == HandleMode::Rotate;
            drag.scale       = m_handle_mode == HandleMode::Scale;
            drag.start_mouse = mouse;
            m_handle_drag    = drag;
            return true;
        }

        // un volume de l'outil sous la souris : il est sélectionné et se glisse sur la pièce
        if (m_hover_tool >= 0 && m_hover_tool < int(m_tools.size())) {
            const ObjectID id     = m_tools[m_hover_tool].volume_id;
            ModelVolume   *volume = volume_by_id(id);
            if (volume != nullptr) {
                if (volume != current_volume())
                    select_volume(volume);
                Drag drag;
                drag.volume_id = id;
                drag.start     = mouse;
                m_drag         = drag;
            }
            return true;
        }

        // « Placer », la pièce sous la souris : on pose (et l'on peut glisser aussitôt pour ajuster)
        if (m_placing && m_hover.valid) {
            if (m_shape == Shape::Svg)
                start_svg(mouse);
            else
                create_at(m_hover);
            return true;
        }

        // dans le vide, sur le plateau (ou sur la pièce sans « Placer ») : la caméra tourne ; un clic sans glisser
        // désélectionne
        m_left_down_unused = true;
        m_left_down_pos    = mouse;
        return false;
    }

    if (mouse_event.Dragging() && mouse_event.LeftIsDown()) {
        if (m_handle_drag.has_value()) {
            drag_handle(mouse);
            m_parent.set_as_dirty();
            return true;
        }
        if (m_drag.has_value()) {
            if (!m_drag->moved && (mouse - m_drag->start).norm() < 3.0)
                return true;
            ModelVolume *volume = volume_by_id(m_drag->volume_id);
            if (volume == nullptr || !volume->is_modifier_tool_volume()) {
                m_drag.reset();
                return true;
            }
            if (!m_drag->moved) {
                m_drag->moved = true;
                if (!m_drag->created)
                    wxGetApp().plater()->take_snapshot(_u8L("Move modifier"));
                const ObjectID id = m_drag->volume_id;
                ensure_rigid_instance();
                // (la scène rechargée a pu remettre le glisser à zéro)
                if (!m_drag.has_value()) {
                    Drag drag;
                    drag.volume_id = id;
                    drag.moved     = true;
                    drag.created   = true; // instantané déjà pris
                    m_drag         = drag;
                }
                volume = volume_by_id(m_drag->volume_id);
                if (volume == nullptr) {
                    m_drag.reset();
                    return true;
                }
            }
            m_mouse = mouse;
            if (has_dimensions(*volume)) {
                constrained_drag(*volume, mouse);
                m_parent.set_as_dirty();
                return true;
            }
            update_picks();
            m_hover = surface_under_mouse(mouse, true);
            if (m_hover.valid)
                move_to(*volume, m_hover);
            else
                slide_off(*volume, mouse); // hors de la pièce
            m_parent.set_as_dirty();
            return true;
        }
        if (m_left_down_unused && (mouse - m_left_down_pos).norm() > 3.0)
            m_left_down_unused = false;
        return false;
    }

    if (mouse_event.LeftUp()) {
        if (m_handle_drag.has_value()) {
            const bool moved = m_handle_drag->moved;
            m_handle_drag.reset();
            if (moved)
                finish_change();
            return true;
        }
        if (m_drag.has_value()) {
            const bool moved = m_drag->moved;
            m_drag.reset();
            if (moved)
                finish_change();
            return true;
        }
        if (m_left_down_unused) {
            m_left_down_unused = false;
            // (sans « Placer », la scène a pu sélectionner la pièce cliquée seule : l'objet entier)
            if (current_volume() != nullptr || m_current_range.has_value() || m_parent.get_selection().get_mode() == Selection::Volume)
                select_object();
        }
        return false;
    }
    return false;
}

// Suppr : le volume ou la plage sélectionnés (jamais l'objet) ; Échap : défait « Joindre » pas à pas,
// puis arrête « Placer », puis désélectionne, puis ferme l'outil.
bool GLGizmoModifiers::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    sync_with_selection();
    // « Mesurer » : Suppr recommence le choix, Échap défait le dernier élément choisi, puis quitte la
    // Mesure ; Maj (un point) et Ctrl comme dans la Mesure de la barre
    if (m_measuring && m_measure) {
        if (action == SLAGizmoEventType::Escape) {
            if (!m_measure->gizmo_event(action, mouse_position, shift_down, alt_down, control_down))
                stop_measure();
            return true;
        }
        if (action == SLAGizmoEventType::Delete || action == SLAGizmoEventType::ShiftDown || action == SLAGizmoEventType::ShiftUp ||
            action == SLAGizmoEventType::CtrlDown || action == SLAGizmoEventType::CtrlUp)
            return m_measure->gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
        return false;
    }
    if (action == SLAGizmoEventType::ShiftDown || action == SLAGizmoEventType::ShiftUp) {
        // Maj enfoncée ou relâchée sans bouger la souris : les points de « Joindre », de « Placer » ou des
        // références
        const bool shift = action == SLAGizmoEventType::ShiftDown;
        if (m_align != AlignStep::None || (m_placing && !m_drag.has_value()))
            update_hover(m_mouse, shift);
        else if (m_ref_step == RefStep::VolumeRef || m_ref_step == RefStep::PartRef) {
            update_picks();
            m_ref_hover = pick_reference(m_mouse, m_ref_step == RefStep::VolumeRef, shift);
        } else
            return false;
        m_parent.set_as_dirty();
        return true;
    }
    if (action == SLAGizmoEventType::Delete) {
        // références : Suppr efface la cote dont le champ est ouvert, jamais la forme
        if (m_ref_step == RefStep::Value && m_ref_edit >= 0) {
            const size_t index = size_t(m_ref_edit);
            close_value();
            delete_dimension(index);
            return true;
        }
        if (m_ref_step != RefStep::None)
            return true;
        delete_current();
        return true;
    }
    if (action == SLAGizmoEventType::Escape) {
        // références : ferme le champ, puis oublie la référence de la forme, puis les arrête
        if (m_ref_step != RefStep::None) {
            if (m_ref_step == RefStep::Value)
                close_value();
            else if (m_ref_step == RefStep::PartRef) {
                m_ref_step  = RefStep::VolumeRef;
                m_ref_first = PickedRef();
                m_ref_error.clear();
            } else
                stop_references();
            m_parent.set_as_dirty();
            return true;
        }
        if (m_align != AlignStep::None) {
            // « Joindre » : oublie la face choisie, puis le point de la forme, puis s'arrête
            if (m_join_lock.valid)
                m_join_lock = JointPick();
            else
                m_align = m_align == AlignStep::TargetPoint ? AlignStep::ShapePoint : AlignStep::None;
            m_join_hover = JointPick();
            m_join_points.clear();
            m_parent.set_as_dirty();
            return true;
        }
        if (m_placing) {
            m_placing = false;
            m_hover   = SurfacePoint();
            m_parent.set_as_dirty();
            return true;
        }
        if (m_handle_mode != HandleMode::None) { // fin de Pivoter / Déplacer
            m_handle_mode = HandleMode::None;
            m_parent.set_as_dirty();
            return true;
        }
        if (current_volume() != nullptr || m_current_range.has_value()) {
            select_object();
            return true;
        }
        return false;
    }
    return false;
}

// =============================================================================================
// Actions
// =============================================================================================

void GLGizmoModifiers::select_volume(ModelVolume *volume)
{
    ModelObject *mo = model_object();
    if (mo == nullptr || volume == nullptr)
        return;
    wxGetApp().obj_list()->select_item(ObjectVolumeID{mo, volume});
}

void GLGizmoModifiers::select_object()
{
    ModelObject *mo = model_object();
    if (mo == nullptr)
        return;
    wxGetApp().obj_list()->select_item(ObjectVolumeID{mo, nullptr});
}

void GLGizmoModifiers::select_range(const t_layer_height_range &range)
{
    ObjectList          *list = wxGetApp().obj_list();
    const wxDataViewItem item = list->GetModel()->GetItemByLayerRange(object_index(), range);
    if (!item.IsOk())
        return;
    list->select_item(item);
    list->selection_changed();
}

// Pose d'un nouveau volume (réglages du panneau) au point de la pièce ; il est sélectionné et suit
// la souris tant que le bouton reste enfoncé.
bool GLGizmoModifiers::create_at(const SurfacePoint &surface_in)
{
    // Copie : souvent m_hover lui-même, que la scène rechargée (étirement de l'instance reporté dans
    // les volumes, au 1er ajout sur une pièce étirée) remet à zéro : la forme partait à l'origine.
    const SurfacePoint surface = surface_in;
    ModelObject       *mo      = model_object();
    if (mo == nullptr || !surface.valid)
        return false;
    // « Depuis une face » : son volume, fait avant l'ajout (la scène rechargée remet le survol à zéro)
    TriangleMesh face_mesh;
    Pose         face_pose;
    if (m_shape == Shape::Face && !face_volume(surface.xref, face_mesh, face_pose))
        return false;
    const TriangleMesh *source = m_shape == Shape::Face ? &face_mesh : shape_mesh(m_shape);
    if (source == nullptr) {
        if (m_shape == Shape::Loaded)
            m_warning = _L("Choose the file to load first.");
        return false;
    }
    const int   object_idx = object_index();
    const Vec3d size       = default_size(m_shape); // une nouvelle forme : les cotes par défaut de sa forme

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add modifier"));
    ensure_rigid_instance();
    mo = model_object();
    if (mo == nullptr)
        return false;

    // un intervalle ne se joint pas (sa tranche, plus large que la pièce, s'y ajouterait)
    const bool            interval = m_shape == Shape::Interval;
    const Nature          nature   = interval && m_nature == Nature::Join ? Nature::Modifier : m_nature;
    const ModelVolumeType type     = volume_type(nature);
    // un modificateur : sa couleur, différente de celles des modificateurs déjà posés (choisie avant de l'ajouter)
    const std::string     color    = type == ModelVolumeType::PARAMETER_MODIFIER ? encode_color(m_parent.new_modifier_color(*mo)) : std::string();
    ModelVolume          *volume   = mo->add_volume(TriangleMesh(*source), type);
    if (m_shape == Shape::Loaded)
        volume->source.input_file = m_loaded_path;
    else
        volume->source.is_from_builtin_objects = true;
    // filament : comme l'ancien ajout de modificateur
    int extruder_id = 0;
    if (type == ModelVolumeType::MODEL_PART && mo->config.has("extruder"))
        extruder_id = mo->config.opt_int("extruder");
    volume->config.set_key_value("extruder", new ConfigOptionInt(extruder_id));

    ModifierPlacement placement;
    placement.shape   = m_shape;
    placement.angle   = interval || m_shape == Shape::Face ? 0. : m_new_angle;
    placement.outside = default_outside(nature);
    placement.color   = color;
    volume->modifier_placement = placement;
    volume->name               = default_name(shape_name(*volume), nature); // [FORME-NATURE-NUMÉRO]

    const BoundingBoxf3 box = volume->mesh().bounding_box();
    Pose                pose;
    pose.anchor   = surface.point;
    pose.rotation = surface_frame(surface.normal, surface.xref, placement.outside) * rotation_z(placement.angle);
    for (int i = 0; i < 3; ++i)
        pose.scale(i) = box.size()(i) > 1e-9 ? std::max(size(i), 0.01) / box.size()(i) : 1.;
    if (interval) { // horizontal, son bas au point visé ; son plan s'étend ensuite à toute la pièce
        pose.rotation = Matrix3d::Identity();
        pose.scale    = Vec3d(1., 1., std::max(m_range_height, 0.01)).cwiseQuotient(box.size());
    }
    if (m_shape == Shape::Face) // sur la face, à sa taille
        pose = face_pose;
    volume->set_transformation(Transform3d(instance_matrix().inverse() * compose(pose, box)));
    if (interval)
        fit_interval(*volume);

    if (type == ModelVolumeType::MODEL_PART)
        m_parent.update_instance_printable_state_for_object(size_t(object_idx));

    // liste d'objets triée, nouveau volume sélectionné, réglages de l'objet affichés
    ObjectList               *list  = wxGetApp().obj_list();
    const wxDataViewItemArray items = list->reorder_volumes_and_get_selection(object_idx, [volume](const ModelVolume *v) { return v == volume; });
    if (!items.IsEmpty())
        list->select_item(items.front());
    list->selection_changed();
    if (type == ModelVolumeType::PARAMETER_MODIFIER)
        wxGetApp().params_panel()->switch_to_object();

    if (m_shape == Shape::Face) // à sa place : il ne suit pas la souris
        return true;
    Drag drag;
    drag.volume_id = volume->id();
    drag.start     = m_mouse;
    drag.created   = true;
    m_drag         = drag;
    return true;
}

// SVG : l'outil SVG choisit le fichier et le pose au point cliqué, puis s'ouvre pour ses réglages.
void GLGizmoModifiers::start_svg(const Vec2d &mouse)
{
    if (m_nature != Nature::Join && m_nature != Nature::Cut && m_nature != Nature::Modifier) {
        m_warning = _L("An SVG can be joined, cut or used as a modifier only.");
        return;
    }
    auto *svg = dynamic_cast<GLGizmoSVG *>(m_parent.get_gizmos_manager().get_gizmo(GLGizmosManager::Svg));
    if (svg != nullptr)
        svg->create_volume(volume_type(m_nature), mouse);
}

void GLGizmoModifiers::move_to(ModelVolume &volume, const SurfacePoint &surface)
{
    // un intervalle garde son inclinaison : son plan du bas passe par le point visé
    if (volume.is_interval()) {
        const IntervalPlanes planes = interval_planes(volume);
        const Vec3d          n      = planes.frame.col(2);
        set_volume_world_matrix(volume, Geometry::translation_transform(n * n.dot(surface.point - planes.origin)) * volume_world_matrix(volume));
        return;
    }
    const BoundingBoxf3 box       = volume.mesh().bounding_box();
    Pose                pose      = decompose(volume_world_matrix(volume), box);
    ModifierPlacement   placement = placement_of(volume);
    placement.base                = 0.; // posée par son dessous
    pose.rotation = surface_frame(surface.normal, surface.xref, placement.outside) * rotation_z(placement.angle) * placement.tilt;
    pose.anchor   = surface.point;
    volume.modifier_placement = placement; // (un ancien modificateur, posé librement, l'est désormais par l'outil)
    set_volume_world_matrix(volume, compose(pose, box));
}

// Nouvelles cotes : le long de chaque axe de la forme, « Symétrie » allumée, son centre ne bouge
// pas (la cote change des deux côtés) ; éteinte, son côté bas ne bouge pas (elle ne change que d'un
// côté : en hauteur, le dessous reste sur la pièce ; après « Joindre », ce qui est dans le plan de
// jonction y reste, la pointe d'un cône par exemple).
void GLGizmoModifiers::apply_size(ModelVolume &volume, const Vec3d &size)
{
    const BoundingBoxf3 box   = volume.mesh().bounding_box();
    const Transform3d   start = volume_world_matrix(volume);
    Pose                pose  = decompose(start, box);
    for (int i = 0; i < 3; ++i)
        if (box.size()(i) > 1e-9 && size(i) > 1e-6)
            pose.scale(i) = (pose.scale(i) < 0. ? -1. : 1.) * size(i) / box.size()(i);
    Vec3d fixed; // point du maillage qui reste en place
    for (int i = 0; i < 3; ++i)
        fixed(i) = m_symmetric[i] ? box.center()(i) : box.min(i);
    if (!m_symmetric[2])
        fixed.z() += placement_of(volume).base * box.size().z();
    Transform3d world   = Transform3d::Identity();
    world.linear()      = pose.rotation * pose.scale.asDiagonal();
    world.translation() = start * fixed - world.linear() * fixed;
    set_volume_world_matrix(volume, world);
    enforce_dimensions(volume); // ses cotes restent justes
}

void GLGizmoModifiers::apply_angle(ModelVolume &volume, double angle)
{
    const BoundingBoxf3 box       = volume.mesh().bounding_box();
    Pose                pose      = decompose(volume_world_matrix(volume), box);
    ModifierPlacement   placement = placement_of(volume);
    const Matrix3d      posed     = posed_frame(pose, placement);
    placement.angle               = angle;
    pose.rotation                 = posed * rotation_z(angle) * placement.tilt;
    volume.modifier_placement     = placement;
    set_volume_world_matrix(volume, compose(pose, box));
    enforce_dimensions(volume);
}

// De l'autre côté du plan d'ancrage (ou de jonction) : demi-tour autour de l'axe X du repère posé, passant
// par le point de la forme qui est dans ce plan (son dessous, ou la pointe d'un cône joint par elle…), qui
// reste en place, comme « Flip » dans Fusion 360.
void GLGizmoModifiers::flip(ModelVolume &volume)
{
    const BoundingBoxf3 box       = volume.mesh().bounding_box();
    const Transform3d   world     = volume_world_matrix(volume);
    Pose                pose      = decompose(world, box);
    ModifierPlacement   placement = placement_of(volume);
    const Vec3d         local     = base_point(box) + placement.base * box.size().z() * Vec3d::UnitZ(); // dans le plan
    const Vec3d         pivot     = world * local;
    const Matrix3d      posed     = posed_frame(pose, placement) * Eigen::AngleAxisd(PI, Vec3d::UnitX()).toRotationMatrix();
    placement.outside             = !placement.outside;
    pose.rotation                 = posed * rotation_z(placement.angle) * placement.tilt;
    pose.anchor += pivot - compose(pose, box) * local;
    volume.modifier_placement     = placement;
    set_volume_world_matrix(volume, compose(pose, box));
    enforce_dimensions(volume);
}

// « Joindre » (comme la Liaison de Fusion 360, sans rotation) : la forme se déplace sans tourner (ses
// plans restent parallèles à ceux du plateau, ou gardent la rotation qu'on lui a donnée) : point sur point
// (« coincide »), sinon le long de la normale de la cible jusqu'à ce que son point (ou sa face) soit dans
// le plan de la cible.
void GLGizmoModifiers::align(ModelVolume &volume, const Vec3d &from_normal, const Vec3d &from_point, const Vec3d &to_normal, const Vec3d &to_point, bool coincide)
{
    const BoundingBoxf3 box   = volume.mesh().bounding_box();
    Pose                pose  = decompose(volume_world_matrix(volume), box);
    const Vec3d         slide = to_normal.norm() > 1e-9 ? Vec3d(to_normal.normalized()) : from_normal.norm() > 1e-9 ? Vec3d(from_normal.normalized()) : Vec3d::Zero();
    pose.anchor += coincide ? Vec3d(to_point - from_point) : Vec3d(slide.dot(to_point - from_point) * slide);
    set_volume_world_matrix(volume, compose(pose, box));
    enforce_dimensions(volume);
}

// Après « Joindre », si un axe de la forme est perpendiculaire au plan de la cible : son repère posé devient
// celui de ce plan, et « base » dit à quelle hauteur de la forme il passe. Une forme symétrique se décrit
// autrement, sans bouger, pour que ce soit son dessous (cube tourné d'un quart de tour, cylindre / sphère /
// tore / disque retourné ; ses plans restent donc parallèles aux mêmes plans) ; sinon il passe ailleurs (la
// pointe d'un cône : son dessus ; le centre du volume : son milieu). « Inverser » se fait alors par rapport
// au plan de jonction, et la hauteur lui reste perpendiculaire (ce qui est dans le plan y reste). Rien si
// aucun axe ne l'est (forme pivotée, cylindre couché…) ou si le plan ne touche pas la forme.
void GLGizmoModifiers::rebase_on_joint(ModelVolume &volume, const Vec3d &plane_normal, const Vec3d &plane_point)
{
    if (plane_normal.norm() < 1e-9)
        return;
    ModifierPlacement   placement = placement_of(volume);
    const BoundingBoxf3 box       = volume.mesh().bounding_box();
    const Transform3d   world     = volume_world_matrix(volume);
    const Vec3d         n         = plane_normal.normalized();
    const Vec3d         local     = decompose(world, box).rotation.transpose() * n; // normale du plan, repère du maillage
    int                 axis      = 2;
    local.cwiseAbs().maxCoeff(&axis);
    if (std::abs(local(axis)) < 0.999 || box.size()(axis) < 1e-9)
        return; // aucun axe de la forme perpendiculaire au plan
    const double f = ((world.inverse() * plane_point)(axis) - box.min(axis)) / box.size()(axis); // où le plan coupe cet axe
    if (f < -1e-3 || f > 1. + 1e-3)
        return; // le plan ne touche pas la forme
    const bool symmetric_z = placement.shape == Shape::Cube || placement.shape == Shape::Cylinder || placement.shape == Shape::Sphere ||
                             placement.shape == Shape::Torus || placement.shape == Shape::Disc;
    Matrix3d   q           = Matrix3d::Identity(); // quart ou demi-tour (repère du maillage) qui amène ce côté dessous
    if (axis == 2) {
        if (f > 0.5 + 1e-9 && symmetric_z)
            q = Eigen::AngleAxisd(PI, Vec3d::UnitX()).toRotationMatrix();
    } else if (placement.shape == Shape::Cube) {
        const Vec3d from = f > 0.5 ? Vec3d(Vec3d::Unit(axis)) : Vec3d(-Vec3d::Unit(axis));
        q                = Eigen::Quaterniond::FromTwoVectors(from, -Vec3d::UnitZ()).toRotationMatrix();
    } else
        return;
    q = q.array().round().matrix(); // permutation signée exacte
    // même forme dans le monde : x -> c + q^T (x - c), la forme étant symétrique autour du centre c de sa boîte
    const Vec3d c    = box.center();
    Transform3d turn = Transform3d::Identity();
    turn.linear()      = q.transpose();
    turn.translation() = c - q.transpose() * c;
    const Transform3d rebased = world * turn;
    for (ModifierDimension &dimension : placement.dimensions) { // références à la forme : même point du monde
        dimension.volume_point = c + q * (dimension.volume_point - c);
        dimension.volume_dir   = q * dimension.volume_dir;
    }
    const Pose     pose      = decompose(rebased, box);
    placement.outside        = pose.rotation.col(2).dot(n) > 0.;
    placement.base           = std::clamp(((rebased.inverse() * plane_point).z() - box.min.z()) / box.size().z(), 0., 1.);
    const Matrix3d posed     = surface_frame(n, pose.rotation.col(0), placement.outside);
    placement.tilt           = (posed * rotation_z(placement.angle)).transpose() * pose.rotation;
    volume.modifier_placement = placement;
    set_volume_world_matrix(volume, rebased);
}

void GLGizmoModifiers::change_nature(ModelVolume &volume, Nature nature)
{
    if (nature == Nature::Count || nature == nature_of(volume) || (nature == Nature::Join && volume.is_interval()))
        return;
    if (volume.is_model_part() && solid_parts_count() <= 1)
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change modifier type"));
    const bool was_part = volume.is_model_part();
    // un nom donné par l'outil suit sa nature
    const bool default_named = is_default_name(volume.name, shape_name(volume), placement_of(volume).shape, nature_of(volume));
    if (!volume.modifier_placement.has_value())
        volume.modifier_placement = placement_of(volume);
    volume.set_type(volume_type(nature));
    if (default_named)
        volume.name = default_name(shape_name(volume), nature);
    if (volume.is_model_part()) {
        ModelObject *mo          = model_object();
        int          extruder_id = (mo != nullptr && mo->config.has("extruder")) ? mo->config.opt_int("extruder") : 0;
        volume.config.set_key_value("extruder", new ConfigOptionInt(extruder_id));
    }
    m_nature = nature;
    if (was_part || volume.is_model_part())
        m_parent.update_instance_printable_state_for_object(size_t(object_index()));
    finish_change(&volume);
}

// Autre forme pour un volume posé : même point d'ancrage, même repère, mêmes cotes.
void GLGizmoModifiers::change_shape(ModelVolume &volume, Shape shape)
{
    if (!is_primitive_shape(shape))
        return;
    const TriangleMesh *source = shape_mesh(shape);
    if (source == nullptr)
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change modifier shape"));
    ensure_rigid_instance();
    const BoundingBoxf3 old_box = volume.mesh().bounding_box();
    Pose                pose    = decompose(volume_world_matrix(volume), old_box);
    Vec3d               size    = old_box.size().cwiseProduct(pose.scale.cwiseAbs());
    if (shape == Shape::Sphere)
        size = Vec3d::Constant(size.x());
    else if (is_round_shape(shape))
        size.y() = size.x();
    ModifierPlacement placement     = placement_of(volume);
    const bool        default_named = is_default_name(volume.name, shape_name(volume), placement.shape, nature_of(volume)); // (il suit sa forme)

    TriangleMesh mesh(*source);
    mesh.translate(-mesh.bounding_box().center().cast<float>());
    volume.set_mesh(std::move(mesh));
    volume.calculate_convex_hull();
    volume.set_new_unique_id();

    const BoundingBoxf3 box = volume.mesh().bounding_box();
    for (int i = 0; i < 3; ++i) // signe gardé (miroir)
        pose.scale(i) = (pose.scale(i) < 0. ? -1. : 1.) * (box.size()(i) > 1e-9 ? size(i) / box.size()(i) : 1.);
    placement.shape           = shape;
    volume.modifier_placement = placement;
    if (default_named)
        volume.name = default_name(shape_name(volume), nature_of(volume));
    volume.set_transformation(Transform3d(instance_matrix().inverse() * compose(pose, box)));
    // (les références de la forme sont dans le repère de son maillage, une forme unité centrée comme
    // l'ancienne : elles gardent leur sens)
    enforce_dimensions(volume);
    m_shape = shape;
    finish_change(&volume);
}

// Suppr : la plage ou le volume sélectionnés.
void GLGizmoModifiers::delete_current()
{
    if (m_current_range.has_value())
        delete_range(*m_current_range);
    else if (const ModelVolume *volume = current_volume(); volume != nullptr)
        delete_volume(volume->id());
}

// Un volume du tableau (croix ou Suppr). L'objet entier est d'abord sélectionné : une sélection
// vide (après « Supprimer » de la scène) fermait l'outil.
void GLGizmoModifiers::delete_volume(const ObjectID &id)
{
    ModelObject *mo = model_object();
    if (mo == nullptr)
        return;
    int volume_idx = -1;
    for (size_t i = 0; i < mo->volumes.size(); ++i)
        if (mo->volumes[i]->id() == id)
            volume_idx = int(i);
    if (volume_idx < 0)
        return;
    const ModelVolume *volume = mo->volumes[volume_idx];
    if (!is_tool_listed(*volume) || (volume->is_model_part() && solid_parts_count() <= 1))
        return;
    const int object_idx = object_index();
    // le volume sélectionné reste sélectionné s'il n'est pas celui que l'on supprime
    const ModelVolume   *current    = current_volume();
    const ObjectID       current_id = current != nullptr ? current->id() : ObjectID();
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete modifier"));
    select_object();
    ObjectList *list = wxGetApp().obj_list();
    if (list->del_subobject_from_object(object_idx, volume_idx, itVolume))
        list->delete_volume_from_list(size_t(object_idx), size_t(volume_idx));
    m_drag.reset();
    m_hover_tool = -1;
    if (current != nullptr && current_id != id) {
        if (ModelVolume *kept = volume_by_id(current_id))
            select_volume(kept);
    } else
        select_object();
    m_parent.set_as_dirty();
}

// « Mesurer » : l'objet entier est sélectionné (la pièce et ses formes se mesurent), « Placer » et
// l'alignement s'arrêtent ; la Mesure prend la souris jusqu'à ce qu'on la quitte.
void GLGizmoModifiers::start_measure()
{
    if (m_measuring || model_object() == nullptr)
        return;
    if (!m_measure) {
        m_measure = std::make_unique<GLGizmoModifierMeasure>(m_parent);
        m_measure->init();
    }
    stop_references();
    m_placing     = false;
    m_align       = AlignStep::None;
    m_handle_mode = HandleMode::None;
    m_drag.reset();
    m_hover      = SurfacePoint();
    m_hover_tool = -1;
    select_object();
    m_measure->on_change_color_mode(m_is_dark_mode);
    m_measure->set_state(On);
    m_measure->data_changed(false);
    m_measuring = true;
    m_parent.set_as_dirty();
}

void GLGizmoModifiers::stop_measure()
{
    if (!m_measuring)
        return;
    m_measuring = false;
    if (m_measure)
        m_measure->set_state(Off);
    // la Mesure, en s'éteignant, a éteint le retour de la barre d'outils : l'outil, lui, reste ouvert
    if (m_state == On && m_parent.get_canvas_type() == GLCanvas3D::ECanvasType::CanvasView3D)
        m_parent.enable_return_toolbar(true);
    m_parent.set_as_dirty();
}

// =============================================================================================
// Références : cotes-contraintes. Une cote relie une référence de la forme (point, axe ou face) à
// une de la pièce, le long d'une direction : une équation linéaire sur sa position (repère de
// l'objet) et sur l'allongement de ses côtés. Plusieurs cotes = un petit système, résolu au moindre
// changement : la forme se déplace si cela suffit, sinon elle se redimensionne aussi, le moins
// possible ; un système sans solution = cotes contradictoires.
// =============================================================================================

wxString GLGizmoModifiers::kind_label(RefKind kind)
{
    switch (kind) {
    case RefKind::Axis: return _L("Axis");
    case RefKind::Plane: return _L("Face");
    default: return _L("Point");
    }
}

// Nom d'une cote : celui que l'utilisateur lui a donné, sinon ses références (« Face - Arête »).
wxString GLGizmoModifiers::dimension_name(const ModifierDimension &dimension)
{
    return dimension.name.empty() ? kind_label(dimension.volume_kind) + " - " + kind_label(dimension.part_kind) : wxString::FromUTF8(dimension.name);
}

wxString GLGizmoModifiers::dimension_label(const ModifierDimension &dimension) const
{
    char value[64];
    std::snprintf(value, sizeof(value), "%.2f mm", dimension_target_mm(dimension));
    return dimension_name(dimension) + " : " + wxString::FromUTF8(value);
}

// Échelle (uniforme) de l'instance : les cotes sont gardées dans le repère de l'objet, affichées et
// tapées en mm du plateau.
double GLGizmoModifiers::instance_scale() const
{
    const double det = std::abs(instance_matrix().linear().determinant());
    return det > 1e-18 ? std::cbrt(det) : 1.;
}

bool GLGizmoModifiers::has_dimensions(const ModelVolume &volume) const
{
    return volume.modifier_placement.has_value() && !volume.modifier_placement->dimensions.empty();
}

double GLGizmoModifiers::dimension_target_mm(const ModifierDimension &dimension) const
{
    return dimension.coincident ? 0. : std::abs(dimension.value) * instance_scale();
}

const GLGizmoModifiers::PickMesh *GLGizmoModifiers::pick_mesh_of(const ObjectID &volume_id, bool on_tool) const
{
    for (const PickMesh &pick : on_tool ? m_tools : m_parts)
        if (pick.volume_id == volume_id)
            return &pick;
    return nullptr;
}

// Référence sous la souris, sur la forme sélectionnée (on_tool) ou sur la pièce : la face plane,
// ou l'arête / l'axe du cercle (bord d'un trou, d'un cylindre) les plus proches à 8 pixels près ;
// Maj : un point, aimanté (sommet, milieu d'arête, centre d'un cercle ou d'un polygone, ancrages de
// la face de la pièce, centre de la face de la forme, points des anneaux d'une forme ronde), sinon
// l'arête proche.
GLGizmoModifiers::PickedRef GLGizmoModifiers::pick_reference(const Vec2d &mouse, bool on_tool, bool point_only)
{
    PickedRef             ref;
    std::vector<PickMesh> meshes;
    if (on_tool) {
        const ModelVolume *volume = current_volume();
        if (volume == nullptr)
            return ref;
        if (volume->is_interval()) // un de ses deux plans, par son contour
            return pick_interval_reference(*volume, mouse);
        if (const PickMesh *pick = pick_mesh_of(volume->id(), true))
            meshes.push_back(*pick);
    } else {
        // le liseré d'un intervalle (ses sommets, ses arêtes) l'emporte sur la pièce sous lui
        if (const PickedRef outline = pick_outline_reference(mouse, point_only); outline.valid)
            return outline;
        meshes = m_parts;
    }
    int    index = -1;
    Vec3d  hit, normal;
    size_t facet = 0;
    if (meshes.empty() || !raycast(meshes, mouse, index, hit, normal, facet))
        return ref;

    PickMesh                                 &pick          = meshes[index];
    const std::shared_ptr<Measure::Measuring> measuring     = measuring_of(pick);
    const Transform3d                        &trafo         = pick.trafo;
    const Matrix3d                            normal_matrix = trafo.linear().inverse().transpose();
    const Vec3d                               local         = trafo.inverse() * hit;
    const std::optional<Measure::SurfaceFeature> feature    = measuring->get_feature(facet, local, Transform3d::Identity(), true);
    ref.valid     = true;
    ref.on_tool   = on_tool;
    ref.volume_id = pick.volume_id;
    ref.kind      = RefKind::Plane;
    ref.point     = hit;
    ref.dir       = normal.normalized();
    ref.name      = kind_label(RefKind::Plane);
    if (feature.has_value() && feature->get_type() == Measure::SurfaceFeatureType::Plane) {
        ref.plane = std::get<0>(feature->get_plane());
        ref.dir   = (normal_matrix * std::get<1>(feature->get_plane())).normalized();
    }
    const double tolerance = std::max(0.2, 8.0 * pixel_size_at(hit));
    auto         to_world  = [&trafo](const Vec3d &p) { return Vec3d(trafo * p); };
    // cercle (repère du maillage) → centre, axe et rayon dans le monde
    auto circle_of = [&](const Measure::SurfaceFeature &f, Vec3d &center, Vec3d &axis, double &radius) {
        const auto [center_local, radius_local, normal_local] = f.get_circle();
        const Vec3d side = normal_local.cross(std::abs(normal_local.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
        center           = to_world(center_local);
        axis             = (normal_matrix * normal_local).normalized();
        radius           = (to_world(center_local + radius_local * side) - center).norm();
    };
    auto rim_distance = [&hit](const Vec3d &center, const Vec3d &axis, double radius) {
        const Vec3d  v      = hit - center;
        const double height = v.dot(axis);
        return std::hypot((v - height * axis).norm() - radius, height);
    };

    if (point_only) {
        double   best         = tolerance;
        Vec3d    best_point   = hit;
        wxString best_name    = kind_label(RefKind::Point);
        int      best_aligned = -1;
        std::optional<std::tuple<Vec3d, Vec3d, double>> best_circle; // centre, axe, rayon
        auto consider = [&](const Vec3d &p, double dist, const wxString &name) {
            if (dist >= best)
                return false;
            best         = dist;
            best_point   = p;
            best_name    = name;
            best_aligned = -1;
            best_circle.reset();
            return true;
        };
        if (ref.plane >= 0) {
            for (const Measure::SurfaceFeature &f : measuring->get_plane_features(ref.plane)) {
                if (f.get_type() == Measure::SurfaceFeatureType::Edge) {
                    const auto [a_local, b_local] = f.get_edge();
                    const Vec3d a = to_world(a_local), b = to_world(b_local), middle = 0.5 * (a + b);
                    const wxString vertex = on_tool ? _L("Vertex of the shape") : _L("Vertex of the part");
                    consider(a, (hit - a).norm(), vertex);
                    consider(b, (hit - b).norm(), vertex);
                    consider(middle, (hit - middle).norm(), _L("Middle of an edge"));
                    if (const std::optional<Vec3d> &center = f.get_extra_point(); center.has_value()) {
                        const Vec3d c = to_world(*center);
                        consider(c, (hit - c).norm(), _L("Center of a polygon"));
                    }
                } else if (f.get_type() == Measure::SurfaceFeatureType::Circle) {
                    Vec3d  center, axis;
                    double radius = 0.;
                    circle_of(f, center, axis, radius);
                    if (consider(center, std::min(rim_distance(center, axis, radius), (hit - center).norm()), _L("Center of a circle")))
                        best_circle = std::make_tuple(center, axis, radius);
                }
            }
            if (!on_tool) {
                // ancrages de la face de la pièce (barycentre, centre de sa boîte, point le plus éloigné des bords…)
                update_face(index, ref.plane, ref.dir);
                if (m_face.part == index && m_face.plane == ref.plane)
                    for (const FaceAnchors::Anchor &anchor : m_face.face.anchors) {
                        if (anchor.kind >= FaceAnchors::AnchorKind::Fraction)
                            continue;
                        const Vec3d p = m_face.origin + anchor.pos.x() * m_face.axis_x + anchor.pos.y() * m_face.axis_y;
                        consider(p, (hit - p).norm(), FaceAnchors::anchor_name(anchor));
                    }
            } else if (const std::optional<Vec3d> center = face_centroid(pick, ref.plane)) // centre de la face de la forme
                consider(*center, (hit - *center).norm(), _L("Centroid of the face"));
        }
        // forme ronde : les points de ses anneaux, comme « Joindre » (le plus proche du point touché : pas un point
        // de l'autre côté)
        if (on_tool) {
            const char *plane_names[3] = {"YZ", "ZX", "XY"};
            for (const PrincipalPoint &q : ring_points(pick))
                consider(q.point, (hit - q.point).norm(), format_wxstr(_L("Point of the %1% plane of the shape"), plane_names[q.axis]));
        }
        // sur la pièce, après un premier point : un point aligné sur lui le long de X, Y ou Z l'emporte
        // sur tout autre ancrage, à 18 pixels près : sur la droite qui passe par lui le long d'un axe
        // (deux coordonnées égales), sur une arête proche, puis dans la face
        if (!on_tool && m_ref_step == RefStep::PartRef && m_ref_first.valid && m_ref_first.kind == RefKind::Point) {
            const Vec3d &first      = m_ref_first.point;
            const Vec3d &n          = ref.dir;
            const char  *names[3]   = {"X", "Y", "Z"};
            double       align_best = std::max(0.3, 18.0 * pixel_size_at(hit));
            Vec3d        align_point;
            wxString     align_name;
            int          align_axis = -1;
            auto         aligned    = [&](const Vec3d &q, double dist, const wxString &name, int axis) {
                if (m_ref_hover.valid && m_ref_hover.aligned == axis)
                    dist *= 0.5;
                if (dist < align_best) {
                    align_best  = dist;
                    align_point = q;
                    align_name  = name;
                    align_axis  = axis;
                }
            };
            for (int i = 0; i < 3; ++i) {
                if (std::abs(n(i)) > 1e-3) {
                    const Vec3d q = first + Vec3d::Unit(i) * (n.dot(hit - first) / n(i));
                    aligned(q, 0.7 * (q - hit).norm(), format_wxstr(_L("In line with the first point along %1%"), names[i]), 3 + i);
                }
                const Vec3d along = Vec3d::Unit(i) - n * n(i); // l'axe, ramené dans la face
                if (along(i) > 1e-3) {
                    const Vec3d q = hit + along * ((first(i) - hit(i)) / along(i));
                    aligned(q, (q - hit).norm(), format_wxstr(_L("Same %1% as the first point"), names[i]), i);
                }
            }
            if (ref.plane >= 0)
                for (const Measure::SurfaceFeature &f : measuring->get_plane_features(ref.plane)) {
                    if (f.get_type() != Measure::SurfaceFeatureType::Edge)
                        continue;
                    const auto [a_local, b_local] = f.get_edge();
                    const Vec3d a = to_world(a_local), b = to_world(b_local);
                    for (int i = 0; i < 3; ++i) {
                        if (std::abs(b(i) - a(i)) < 1e-6)
                            continue;
                        const double s = (first(i) - a(i)) / (b(i) - a(i));
                        if (s < 0. || s > 1.)
                            continue;
                        const Vec3d q = a + s * (b - a);
                        aligned(q, 0.85 * (q - hit).norm(), format_wxstr(_L("Same %1% as the first point"), names[i]), i);
                    }
                }
            if (align_axis >= 0) {
                best_point   = align_point;
                best_name    = align_name;
                best_aligned = align_axis;
                best_circle.reset();
            }
        }
        // aucun point aimanté : une arête proche reste une arête, Maj ou non (un point puis une arête : la
        // distance perpendiculaire à elle)
        if (best >= tolerance && best_aligned < 0)
            if (const PickedRef edge = pick_reference(mouse, on_tool, false); edge.valid && edge.kind == RefKind::Axis)
                return edge;
        ref.kind    = RefKind::Point;
        ref.point   = best_point;
        ref.name    = best_name;
        ref.aligned = best_aligned;
        if (best_circle.has_value()) {
            ref.circle        = true;
            ref.circle_center = std::get<0>(*best_circle);
            ref.circle_normal = std::get<1>(*best_circle);
            ref.circle_radius = std::get<2>(*best_circle);
        }
        return ref;
    }

    if (ref.plane < 0) {
        // surface courbe (sphère…) : le point cliqué
        ref.kind = RefKind::Point;
        ref.name = kind_label(RefKind::Point);
        return ref;
    }
    double best = tolerance;
    for (const Measure::SurfaceFeature &f : measuring->get_plane_features(ref.plane)) {
        if (f.get_type() == Measure::SurfaceFeatureType::Edge) {
            const auto [a_local, b_local] = f.get_edge();
            const Vec3d  a  = to_world(a_local), b = to_world(b_local);
            const Vec3d  ab = b - a;
            const double l  = ab.squaredNorm();
            if (l < 1e-18)
                continue;
            const double t    = std::clamp((hit - a).dot(ab) / l, 0., 1.);
            const double dist = (a + t * ab - hit).norm();
            if (dist < best) {
                best       = dist;
                ref.kind   = RefKind::Axis;
                ref.point  = a;
                ref.dir    = ab.normalized();
                ref.edge_a = a;
                ref.edge_b = b;
                ref.circle = false;
                ref.name   = _L("Edge");
            }
        } else if (f.get_type() == Measure::SurfaceFeatureType::Circle) {
            Vec3d  center, axis;
            double radius = 0.;
            circle_of(f, center, axis, radius);
            const double dist = rim_distance(center, axis, radius);
            if (dist < best) {
                // un cercle (bord d'un trou, d'un cylindre) : son centre
                best              = dist;
                ref.kind          = RefKind::Point;
                ref.point         = center;
                ref.dir           = axis;
                ref.circle        = true;
                ref.circle_center = center;
                ref.circle_normal = axis;
                ref.circle_radius = radius;
                ref.name          = _L("Center of a circle");
            }
        }
    }
    return ref;
}

// Cote entre deux références (monde) : références gardées dans leurs repères, direction et valeur
// (distance tapée, en mm du plateau, du côté où la forme est déjà). Refusée si elle n'a pas de sens
// (deux faces ou deux axes non parallèles, axe qui traverse la face).
bool GLGizmoModifiers::make_dimension(const ModelVolume &volume, const PickedRef &volume_ref, const PickedRef &part_ref, int along, double value_mm,
                                      ModifierDimension &out, wxString &error, double *current_mm) const
{
    const Transform3d instance        = instance_matrix();
    const Transform3d instance_inv    = instance.inverse();
    const Matrix3d    direction_inv   = instance_inv.linear();          // directions monde → objet
    const Matrix3d    normal_to_obj   = instance.linear().transpose();  // normales monde → objet
    auto              to_object_dir   = [&](const PickedRef &ref) {
        return Vec3d(((ref.kind == RefKind::Plane ? normal_to_obj : direction_inv) * ref.dir).normalized());
    };
    const Vec3d       pm              = instance_inv * volume_ref.point;
    const Vec3d       dm              = to_object_dir(volume_ref);
    const Vec3d       pp              = instance_inv * part_ref.point;
    const Vec3d       dp              = to_object_dir(part_ref);
    const Transform3d volume_matrix   = volume.get_matrix();
    const Transform3d volume_inv      = volume_matrix.inverse();

    out              = ModifierDimension();
    out.volume_kind  = volume_ref.kind;
    out.part_kind    = part_ref.kind;
    out.volume_point = volume_inv * pm;
    out.volume_dir   = ((volume_ref.kind == RefKind::Plane ? Matrix3d(volume_matrix.linear().transpose()) : Matrix3d(volume_inv.linear())) * dm).normalized();
    out.part_point   = pp;
    out.part_dir     = dp;

    const double parallel = std::cos(PI / 180.);
    const double crossing = std::sin(PI / 180.);
    const Vec3d  offset   = pm - pp;
    auto any_perpendicular = [](const Vec3d &axis) { return Vec3d(axis.cross(std::abs(axis.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized()); };
    const RefKind vk = volume_ref.kind, pk = part_ref.kind;
    bool  can_coincide = false;
    Vec3d direction    = Vec3d::UnitX();
    if (vk == RefKind::Plane || pk == RefKind::Plane) {
        const Vec3d normal = pk == RefKind::Plane ? dp : dm;
        if (vk == RefKind::Plane && pk == RefKind::Plane && std::abs(dm.dot(dp)) < parallel) {
            error = _L("These two faces are not parallel: their distance has no meaning.");
            return false;
        }
        const RefKind other     = pk == RefKind::Plane ? vk : pk;
        const Vec3d   other_dir = pk == RefKind::Plane ? dm : dp;
        if (other == RefKind::Axis && std::abs(other_dir.dot(normal)) > crossing) {
            error = _L("This axis crosses the face: their distance has no meaning.");
            return false;
        }
        direction = normal;
    } else if (vk == RefKind::Axis || pk == RefKind::Axis) {
        const Vec3d axis = pk == RefKind::Axis ? dp : dm;
        if (vk == RefKind::Axis && pk == RefKind::Axis && std::abs(dm.dot(dp)) < parallel) {
            error = _L("These two axes are not parallel: their distance has no meaning.");
            return false;
        }
        // la distance d'un point ou d'un axe à un axe : perpendiculairement à lui
        const Vec3d v = offset - offset.dot(axis) * axis;
        direction     = v.norm() > 1e-6 ? Vec3d(v.normalized()) : any_perpendicular(axis);
        can_coincide  = true;
    } else if (along >= 0 && along < 3)
        direction = Vec3d::Unit(along); // deux points, le long de X, Y ou Z de la pièce
    else {
        direction    = offset.norm() > 1e-6 ? Vec3d(offset.normalized()) : Vec3d::UnitX(); // en ligne droite
        can_coincide = true;
    }
    out.direction       = direction;
    const double scale  = instance_scale();
    const double current = direction.dot(offset);
    if (current_mm != nullptr)
        *current_mm = std::abs(current) * scale;
    const double target = std::abs(value_mm) / scale;
    if (target < 1e-6 && can_coincide) {
        out.coincident = true;
        out.value      = 0.;
    } else
        out.value = (current < 0. ? -1. : 1.) * target;
    return true;
}

// Équations d'une cote sur la translation t de la forme (repère de l'objet) : n · t = c.
void GLGizmoModifiers::dimension_equations(const ModelVolume &volume, const ModifierDimension &dimension, std::vector<std::pair<Vec3d, double>> &equations) const
{
    const Transform3d matrix = volume.get_matrix();
    const Vec3d       offset = matrix * dimension.volume_point - dimension.part_point;
    if (!dimension.coincident) {
        equations.emplace_back(dimension.direction, dimension.value - dimension.direction.dot(offset));
        return;
    }
    if (dimension.volume_kind == RefKind::Point && dimension.part_kind == RefKind::Point) {
        for (int i = 0; i < 3; ++i)
            equations.emplace_back(Vec3d::Unit(i), -offset(i));
        return;
    }
    // sur un axe : les deux directions perpendiculaires à lui
    const Vec3d axis = dimension.part_kind == RefKind::Axis ? Vec3d(dimension.part_dir.normalized()) : Vec3d((matrix.linear() * dimension.volume_dir).normalized());
    const Vec3d e1   = axis.cross(std::abs(axis.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
    const Vec3d e2   = axis.cross(e1);
    equations.emplace_back(e1, -e1.dot(offset));
    equations.emplace_back(e2, -e2.dot(offset));
}

// Moindre changement par priorités : a x = b, les colonnes de « a » rangées par groupes (« groups » : leurs tailles), du
// plus coûteux au moins coûteux. Un groupe ne bouge que de ce que les suivants ne peuvent pas faire, et le moins
// possible : pas du tout s'ils suffisent. Sans solution : la moins mauvaise. (Réplique en Python et ses cas :
// voir la fiche des modificateurs.)
static Eigen::VectorXd least_change(const Eigen::MatrixXd &a, Eigen::VectorXd b, const std::vector<int> &groups)
{
    Eigen::VectorXd x     = Eigen::VectorXd::Zero(a.cols());
    int             start = 0;
    for (const int size : groups) {
        Eigen::MatrixXd block  = a.middleCols(start, size);
        Eigen::VectorXd target = b;
        if (const int rest = int(a.cols()) - start - size; rest > 0) { // ce que les groupes suivants ne peuvent pas faire
            const Eigen::MatrixXd                                   cheaper = a.rightCols(rest);
            Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> cod(cheaper);
            cod.setThreshold(1e-9);
            const Eigen::MatrixXd away = Eigen::MatrixXd::Identity(a.rows(), a.rows()) - cheaper * cod.pseudoInverse();
            block                      = away * block;
            target                     = away * b;
        }
        if (target.cwiseAbs().maxCoeff() > 1e-9) {
            Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> cod(block);
            cod.setThreshold(1e-9);
            x.segment(start, size) = cod.solve(target);
            b -= a.middleCols(start, size) * x.segment(start, size);
        }
        start += size;
    }
    return x;
}

// Place, et au besoin taille, de la forme qui respecte toutes ses cotes : elle se déplace si cela suffit ; sinon elle
// s'allonge aussi le long de ses côtés, le moins possible (une sphère en entier, une forme ronde en diamètre et en
// hauteur, une forme aux proportions gardées en entier), autour du point que gardent ses champs de taille
// (« Symétrie »), puis se déplace le moins possible. « world » : sa matrice (monde) ; « free_dirs » : projecteur sur
// les directions où elle peut glisser (sans changer de taille) ; « set_sizes » : ses côtés (repère du maillage) que
// ses cotes fixent (un déplacement ne compenserait pas leur changement). Faux : cotes contradictoires, ou un côté qui
// passerait sous 0,1 mm (elle est alors seulement déplacée, au moins mal).
bool GLGizmoModifiers::solve_dimensions(const ModelVolume &volume, const std::vector<ModifierDimension> &dimensions, Transform3d &world, Matrix3d *free_dirs,
                                        std::array<bool, 3> *set_sizes) const
{
    const Transform3d   matrix = volume.get_matrix();
    const BoundingBoxf3 box    = volume.mesh().bounding_box();
    world                      = instance_matrix() * matrix;
    if (free_dirs != nullptr)
        *free_dirs = Matrix3d::Identity();
    if (set_sizes != nullptr)
        set_sizes->fill(false);
    // point du maillage qui reste en place quand elle s'allonge (comme apply_size)
    Vec3d fixed;
    for (int i = 0; i < 3; ++i)
        fixed(i) = m_symmetric[i] ? box.center()(i) : box.min(i);
    if (!m_symmetric[2])
        fixed.z() += placement_of(volume).base * box.size().z();
    std::vector<std::pair<Vec3d, double>> equations;
    std::vector<Vec3d>                    stretch; // dérivée de chaque équation par l'allongement (relatif) de chaque côté
    for (const ModifierDimension &dimension : dimensions) {
        dimension_equations(volume, dimension, equations);
        while (stretch.size() < equations.size())
            stretch.push_back((matrix.linear().transpose() * equations[stretch.size()].first).cwiseProduct(dimension.volume_point - fixed));
    }
    if (equations.empty())
        return true;
    // côtés liés
    const Shape     shape = placement_of(volume).shape;
    Eigen::MatrixXd ties  = Eigen::MatrixXd::Identity(3, 3);
    if (shape == Shape::Sphere || (m_keep_ratio && shape != Shape::Cube && shape != Shape::Face && !is_round_shape(shape)))
        ties = Eigen::MatrixXd::Ones(3, 1);
    else if (is_round_shape(shape)) {
        ties       = Eigen::MatrixXd::Zero(3, 2);
        ties(0, 0) = ties(1, 0) = ties(2, 1) = 1.;
    }
    const Eigen::Index rows = Eigen::Index(equations.size());
    const Eigen::Index sides = ties.cols();
    Eigen::MatrixXd    move(rows, 3), sizes(rows, 3);
    Eigen::VectorXd    c(rows);
    for (Eigen::Index i = 0; i < rows; ++i) {
        move.row(i)  = equations[size_t(i)].first.transpose();
        sizes.row(i) = stretch[size_t(i)].transpose();
        c(i)         = equations[size_t(i)].second;
    }
    Eigen::MatrixXd a(rows, sides + 3);
    a << sizes * ties, move;
    Eigen::VectorXd x = least_change(a, c, {int(sides), 3});
    Vec3d           k = ties * x.head(sides);
    // déplacement seul : directions libres, côtés fixés
    Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> cod(move);
    cod.setThreshold(1e-6);
    const Eigen::MatrixXd pinv = cod.pseudoInverse();
    if (free_dirs != nullptr)
        *free_dirs = Matrix3d::Identity() - Matrix3d(pinv * move);
    if (set_sizes != nullptr) {
        const Eigen::MatrixXd away = Eigen::MatrixXd::Identity(rows, rows) - move * pinv;
        for (int i = 0; i < 3; ++i)
            (*set_sizes)[i] = (away * sizes.col(i)).cwiseAbs().maxCoeff() > 1e-6;
    }
    bool ok = true;
    for (int i = 0; i < 3; ++i)
        if (box.size()(i) > 1e-9 && matrix.linear().col(i).norm() * (1. + k(i)) * box.size()(i) < 0.1)
            ok = false;
    if (!ok) { // (jamais une forme retournée ou plate : seulement déplacée)
        x.setZero();
        x.tail<3>() = pinv * c;
        k.setZero();
    }
    Transform3d stretched = matrix * Geometry::translation_transform(fixed) * Geometry::scale_transform(Vec3d(Vec3d::Ones() + k)) *
                            Geometry::translation_transform(-fixed);
    stretched.translation() += Vec3d(x.tail<3>());
    world = instance_matrix() * stretched;
    return ok && (a * x - c).cwiseAbs().maxCoeff() < 1e-4;
}

// Cible d'une cote d'intervalle : à sa valeur de la référence de la pièce, le long de sa direction (là où elle a
// été posée), fixe dans l'espace ; son plan du bas y passe, ou son plan du haut si c'est lui qui a été cliqué.
GLGizmoModifiers::IntervalTarget GLGizmoModifiers::interval_target(const ModelVolume &volume, const ModifierDimension &dimension) const
{
    const Transform3d   instance = instance_matrix();
    const BoundingBoxf3 box      = volume.mesh().bounding_box();
    IntervalTarget      target;
    target.kind   = dimension.part_kind;
    target.point  = instance * Vec3d(dimension.part_point + (dimension.coincident ? 0. : dimension.value) * dimension.direction);
    target.dir    = ((dimension.part_kind == RefKind::Plane ? Matrix3d(instance.linear().inverse().transpose()) : Matrix3d(instance.linear())) *
                  dimension.part_dir).normalized();
    target.offset = dimension.volume_point.z() > box.center().z() ? interval_planes(volume).thickness : 0.;
    return target;
}

// Intervalle coté : ses cotes fixent son plan et au besoin son épaisseur. Le plan le plus proche de l'actuel qui passe
// par toutes leurs cibles : il glisse le long de sa normale si cela suffit ; sinon il s'épaissit aussi (une cote sur
// son bas, une sur son haut) ; sinon encore, il bascule (autour de son origine) : deux cibles de part et d'autre du
// même plan, à des hauteurs différentes, donnent son inclinaison (Gauss-Newton, moindre changement par priorités).
// Faux si elles se contredisent, ou s'il passerait sous 0,01 mm d'épaisseur (le plan rendu est alors le moins
// mauvais).
bool GLGizmoModifiers::solve_interval(const ModelVolume &volume, const std::vector<ModifierDimension> &dimensions, Transform3d &world) const
{
    world = volume_world_matrix(volume);
    if (dimensions.empty())
        return true;
    const BoundingBoxf3         box  = volume.mesh().bounding_box();
    Pose                        pose = decompose(world, box);
    std::vector<IntervalTarget> targets;
    for (const ModifierDimension &dimension : dimensions)
        targets.push_back(interval_target(volume, dimension));
    // « length » : la taille de la pièce (l'écart à une droite ou à un plan visés s'y compte en mm)
    double length = 10.;
    if (const ModelObject *mo = model_object(); mo != nullptr && instance_index() >= 0)
        length = std::max(length, mo->instance_convex_hull_bounding_box(size_t(instance_index())).size().norm());
    const Vec3d pivot     = pose.anchor;
    Vec3d       n         = pose.rotation.col(2);
    double      d         = n.dot(pose.anchor); // plan du bas : n · x = d
    double      thickness = std::abs(pose.scale.z()) * box.size().z();
    double      worst     = 0.;
    for (int iteration = 0; iteration < 30; ++iteration) {
        const Vec3d e1 = n.cross(std::abs(n.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
        const Vec3d e2 = n.cross(e1);
        // dérivées : basculer vers e1, vers e2, épaissir, glisser le long de n (du plus coûteux au moins coûteux)
        std::vector<std::array<double, 4>> rows;
        std::vector<double>                residuals;
        auto through = [&](const Vec3d &q, bool top) { // son plan du bas (ou du haut) passe par q
            rows.push_back({e1.dot(q - pivot), e2.dot(q - pivot), top ? -1. : 0., -1.});
            residuals.push_back(n.dot(q) - d - (top ? thickness : 0.));
        };
        auto along = [&](const Vec3d &a) { // la direction a est dans le plan
            rows.push_back({length * e1.dot(a), length * e2.dot(a), 0., 0.});
            residuals.push_back(length * n.dot(a));
        };
        for (const IntervalTarget &target : targets) {
            through(target.point, target.offset > 0.); // (« offset » : l'épaisseur pour une cote sur le haut, 0 sinon)
            if (target.kind == RefKind::Axis)
                along(target.dir);
            else if (target.kind == RefKind::Plane) { // parallèle à lui
                const Vec3d m1 = target.dir.cross(std::abs(target.dir.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
                along(m1);
                along(target.dir.cross(m1));
            }
        }
        worst = 0.;
        for (const double residual : residuals)
            worst = std::max(worst, std::abs(residual));
        if (worst < 1e-7)
            break;
        Eigen::MatrixXd jacobian(rows.size(), 4);
        Eigen::VectorXd r(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            for (int j = 0; j < 4; ++j)
                jacobian(Eigen::Index(i), j) = rows[i][size_t(j)];
            r(Eigen::Index(i)) = residuals[i];
        }
        const Eigen::VectorXd step   = least_change(jacobian, -r, {2, 1, 1});
        const Vec3d           tilted = (n + step(0) * e1 + step(1) * e2).normalized();
        d += step(3) + (tilted - n).dot(pivot); // (il bascule autour de son origine, puis glisse)
        thickness += step(2);
        n = tilted;
    }
    // la tranche tourne du moins possible pour prendre cette normale ; son origine passe sur le nouveau plan du bas
    pose.rotation = Matrix3d(Eigen::Quaterniond::FromTwoVectors(Vec3d(pose.rotation.col(2)), n).toRotationMatrix()) * pose.rotation;
    pose.anchor -= (n.dot(pose.anchor) - d) * n;
    if (box.size().z() > 1e-9)
        pose.scale.z() = (pose.scale.z() < 0. ? -1. : 1.) * std::max(thickness, 0.01) / box.size().z();
    world = compose(pose, box);
    return worst < 1e-3 && thickness >= 0.01;
}

// Pose (monde) qui tient les cotes : d'un intervalle, son plan et son épaisseur ; d'une autre forme, sa place et au
// besoin sa taille. Faux : elles se contredisent.
bool GLGizmoModifiers::solve_pose(const ModelVolume &volume, const std::vector<ModifierDimension> &dimensions, Transform3d &world) const
{
    return volume.is_interval() ? solve_interval(volume, dimensions, world) : solve_dimensions(volume, dimensions, world, nullptr);
}

// Bout de la cote sur la forme : sa référence, qui bouge avec elle ; pour un intervalle, sa cible, fixe.
Vec3d GLGizmoModifiers::dimension_end(const ModelVolume &volume, const ModifierDimension &dimension) const
{
    return volume.is_interval() ? interval_target(volume, dimension).point : Vec3d(volume_world_matrix(volume) * dimension.volume_point);
}

// Cote avec laquelle « tested » se contredit (sans elle, tout redevient possible), sinon -1.
int GLGizmoModifiers::conflicting_dimension(const ModelVolume &volume, const std::vector<ModifierDimension> &dimensions, size_t tested) const
{
    for (size_t j = 0; j < dimensions.size(); ++j) {
        if (j == tested)
            continue;
        std::vector<ModifierDimension> others = dimensions;
        others.erase(others.begin() + j);
        Transform3d world;
        if (solve_pose(volume, others, world))
            return int(j);
    }
    return -1;
}

// Écart (mm) entre une cote et la position actuelle de la forme ; un intervalle : entre sa cible et son plan (une
// droite ou un plan : à 10 mm du point de la cible aussi, son inclinaison compte).
double GLGizmoModifiers::dimension_error_mm(const ModelVolume &volume, const ModifierDimension &dimension) const
{
    if (volume.is_interval()) {
        const IntervalTarget target = interval_target(volume, dimension);
        const IntervalPlanes planes = interval_planes(volume);
        auto gap = [&](const Vec3d &q) { return std::abs(planes.frame.col(2).dot(q - planes.origin) - target.offset); };
        double error = gap(target.point);
        if (target.kind == RefKind::Axis)
            error = std::max({error, gap(target.point + 10. * target.dir), gap(target.point - 10. * target.dir)});
        else if (target.kind == RefKind::Plane) {
            const Vec3d m1 = target.dir.cross(std::abs(target.dir.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
            error          = std::max({error, gap(target.point + 10. * m1), gap(target.point + 10. * target.dir.cross(m1))});
        }
        return error;
    }
    std::vector<std::pair<Vec3d, double>> equations;
    dimension_equations(volume, dimension, equations);
    double error = 0.;
    for (const auto &equation : equations)
        error = std::max(error, std::abs(equation.second));
    return error * instance_scale();
}

// Après une modification de la forme (cotes, angle, inverser, aligner, autre forme) : elle est
// déplacée, et au besoin redimensionnée, pour que ses cotes restent justes (un intervalle : son plan
// passe par leurs cibles).
bool GLGizmoModifiers::enforce_dimensions(ModelVolume &volume)
{
    if (!has_dimensions(volume))
        return true;
    Transform3d world;
    const bool  ok = solve_pose(volume, volume.modifier_placement->dimensions, world);
    set_volume_world_matrix(volume, world);
    if (!ok)
        m_warning = _L("The dimensions of this shape can no longer all be kept: change or delete one of them.");
    return ok;
}

// Le repère de l'objet a changé (transformation de l'instance reportée dans les volumes) : les
// références à la pièce suivent, les valeurs sont relues sur la position (inchangée dans le monde).
void GLGizmoModifiers::rebase_dimensions(ModelObject &object, const Transform3d &old_to_new)
{
    const Matrix3d linear = old_to_new.linear();
    const Matrix3d normal = linear.inverse().transpose();
    for (ModelVolume *volume : object.volumes) {
        if (!has_dimensions(*volume))
            continue;
        ModifierPlacement placement = *volume->modifier_placement;
        const Transform3d matrix    = volume->get_matrix();
        for (ModifierDimension &d : placement.dimensions) {
            const bool  plane  = d.volume_kind == RefKind::Plane || d.part_kind == RefKind::Plane;
            const Vec3d target = old_to_new * Vec3d(d.part_point + (d.coincident ? 0. : d.value) * d.direction); // (d'un intervalle : fixe)
            d.part_point       = old_to_new * d.part_point;
            d.part_dir         = ((d.part_kind == RefKind::Plane ? normal : linear) * d.part_dir).normalized();
            d.direction        = ((plane ? normal : linear) * d.direction).normalized();
            if (!d.coincident)
                d.value = d.direction.dot((volume->is_interval() ? target : Vec3d(matrix * d.volume_point)) - d.part_point);
        }
        volume->modifier_placement = placement;
    }
}

// Glisser une forme cotée : elle suit la souris dans un plan face à la caméra, seulement le long des
// directions que ses cotes laissent libres.
void GLGizmoModifiers::constrained_drag(ModelVolume &volume, const Vec2d &mouse)
{
    if (!m_drag.has_value())
        return;
    Drag         &drag   = *m_drag;
    const Camera &camera = wxGetApp().plater()->get_camera();
    Vec3d         origin, direction;
    MeshRaycaster::line_from_mouse_pos_static(mouse, Transform3d::Identity(), camera, origin, direction);
    if (!drag.constrained) {
        drag.constrained  = true;
        drag.start_offset = volume.get_offset();
        drag.plane_origin = volume_world_matrix(volume) * volume.mesh().bounding_box().center();
        drag.plane_normal = -camera.get_dir_forward();
        const double den  = direction.dot(drag.plane_normal);
        drag.start_point  = std::abs(den) < 1e-12 ? drag.plane_origin :
                                                    Vec3d(origin + direction * ((drag.plane_origin - origin).dot(drag.plane_normal) / den));
        Transform3d unused;
        solve_dimensions(volume, volume.modifier_placement->dimensions, unused, &drag.free_dirs);
        if (drag.free_dirs.norm() < 1e-6)
            m_warning = _L("Its dimensions fix the shape in every direction: change them in the window.");
    }
    const double den = direction.dot(drag.plane_normal);
    if (std::abs(den) < 1e-12)
        return;
    const Vec3d point        = origin + direction * ((drag.plane_origin - origin).dot(drag.plane_normal) / den);
    const Vec3d delta_object = instance_matrix().linear().inverse() * (point - drag.start_point);
    volume.set_offset(drag.start_offset + drag.free_dirs * delta_object);
    sync_gl_volumes(volume);
}

void GLGizmoModifiers::start_references()
{
    const ModelVolume *volume = current_volume();
    if (volume == nullptr || !volume->is_modifier_tool_volume())
        return;
    stop_measure();
    m_placing = false;
    m_align   = AlignStep::None;
    m_drag.reset();
    m_handle_mode   = HandleMode::None; // (Pivoter / Déplacer éteints pendant les références)
    m_ref_step      = RefStep::VolumeRef;
    m_ref_volume_id = volume->id();
    m_ref_hover     = PickedRef();
    m_ref_first     = PickedRef();
    m_ref_second    = PickedRef();
    m_ref_edit      = -1;
    m_ref_error.clear();
    m_parent.set_as_dirty();
}

void GLGizmoModifiers::stop_references()
{
    m_ref_step   = RefStep::None;
    m_ref_hover  = PickedRef();
    m_ref_first  = PickedRef();
    m_ref_second = PickedRef();
    m_ref_edit   = -1;
    m_ref_error.clear();
    m_parent.set_as_dirty();
}

// Champ de la distance fermé (appliquée ou annulée) : on choisit deux autres références (ou l'on
// revient où l'on était avant de cliquer l'étiquette d'une cote).
void GLGizmoModifiers::close_value()
{
    const bool edit = m_ref_edit >= 0;
    m_ref_step      = edit ? m_ref_before_edit : RefStep::VolumeRef;
    m_ref_edit      = -1;
    m_ref_first     = PickedRef();
    m_ref_second    = PickedRef();
    m_ref_hover     = PickedRef();
    m_ref_error.clear();
    m_parent.set_as_dirty();
}

// Clic sur l'étiquette d'une cote : son champ s'ouvre pour la changer (ou la supprimer).
void GLGizmoModifiers::open_dimension(size_t index)
{
    const ModelVolume *volume = current_volume();
    if (volume == nullptr || !has_dimensions(*volume) || index >= volume->modifier_placement->dimensions.size())
        return;
    stop_measure();
    m_placing         = false;
    m_align           = AlignStep::None;
    m_ref_before_edit = m_ref_step == RefStep::Value ? RefStep::VolumeRef : m_ref_step;
    m_ref_step        = RefStep::Value;
    m_ref_volume_id   = volume->id();
    m_ref_edit        = int(index);
    m_ref_value       = dimension_target_mm(volume->modifier_placement->dimensions[index]);
    m_ref_focus       = true;
    m_ref_error.clear();
    m_parent.set_as_dirty();
}

// « Appliquer » (ou Entrée) : la nouvelle cote, si elle ne contredit pas les autres ; la forme est
// déplacée, et au besoin redimensionnée, pour la respecter. On reste dans les références pour en poser
// d'autres.
void GLGizmoModifiers::apply_reference_value()
{
    if (m_ref_edit >= 0) {
        set_dimension_value(size_t(m_ref_edit), m_ref_value);
        if (m_ref_error.IsEmpty())
            close_value();
        return;
    }
    ModelVolume *volume = current_volume();
    if (volume == nullptr || !volume->is_modifier_tool_volume() || !m_ref_first.valid || !m_ref_second.valid) {
        stop_references();
        return;
    }
    const ObjectID id = volume->id();
    if (!instance_is_rigid()) {
        // une instance tournée et étirée inégalement : sa transformation passe d'abord dans les volumes
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add dimension"));
        ensure_rigid_instance();
        volume = volume_by_id(id);
        if (volume == nullptr)
            return;
    }
    ModifierDimension dimension;
    wxString          error;
    if (!make_dimension(*volume, m_ref_first, m_ref_second, m_ref_along, m_ref_value, dimension, error)) {
        m_ref_error = error;
        return;
    }
    std::vector<ModifierDimension> dimensions = placement_of(*volume).dimensions;
    dimensions.push_back(dimension);
    Transform3d world;
    if (!solve_pose(*volume, dimensions, world)) {
        const int other = conflicting_dimension(*volume, dimensions, dimensions.size() - 1);
        m_ref_error     = other >= 0 ? format_wxstr(_L("Not possible: this dimension contradicts \"%1%\"."), into_u8(dimension_label(dimensions[other]))) :
                                       _L("Not possible: this dimension contradicts the others.");
        return;
    }
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Add dimension"));
    ModifierPlacement    placement = placement_of(*volume);
    placement.dimensions           = dimensions;
    volume->modifier_placement     = placement;
    set_volume_world_matrix(*volume, world);
    finish_change();
    close_value();
}

// Nouvelle valeur d'une cote (champ de la fenêtre ou de son étiquette) ; refusée si elle contredit
// les autres.
void GLGizmoModifiers::set_dimension_value(size_t index, double value_mm)
{
    ModelVolume *volume = current_volume();
    if (volume == nullptr || !has_dimensions(*volume) || index >= volume->modifier_placement->dimensions.size())
        return;
    std::vector<ModifierDimension> dimensions = volume->modifier_placement->dimensions;
    ModifierDimension             &d          = dimensions[index];
    const double                   target     = std::abs(value_mm) / instance_scale();
    const bool                     plane      = d.volume_kind == RefKind::Plane || d.part_kind == RefKind::Plane;
    const bool                     two_points = d.volume_kind == RefKind::Point && d.part_kind == RefKind::Point;
    const bool                     along_axis = d.direction.cwiseAbs().maxCoeff() > 1. - 1e-9; // deux points le long de X, Y ou Z
    if (target < 1e-6 && !plane && !(two_points && along_axis)) {
        d.coincident = true;
        d.value      = 0.;
    } else {
        const double sign = d.value < 0. ? -1. : 1.;
        d.coincident      = false;
        d.value           = sign * target;
    }
    Transform3d world;
    if (!solve_pose(*volume, dimensions, world)) {
        const int other = conflicting_dimension(*volume, dimensions, index);
        m_ref_error     = other >= 0 ? format_wxstr(_L("Not possible: this dimension contradicts \"%1%\"."), into_u8(dimension_label(dimensions[other]))) :
                                       _L("Not possible: this dimension contradicts the others.");
        return;
    }
    m_ref_error.clear();
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Edit dimension"));
    ModifierPlacement    placement = *volume->modifier_placement;
    placement.dimensions           = dimensions;
    volume->modifier_placement     = placement;
    set_volume_world_matrix(*volume, world);
    finish_change();
}

// Nouveau nom d'une cote (vide : ses références).
void GLGizmoModifiers::rename_dimension(size_t index, const std::string &name)
{
    ModelVolume *volume = current_volume();
    if (volume == nullptr || !has_dimensions(*volume) || index >= volume->modifier_placement->dimensions.size() ||
        volume->modifier_placement->dimensions[index].name == name)
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Rename dimension"));
    ModifierPlacement    placement = *volume->modifier_placement;
    placement.dimensions[index].name = name;
    volume->modifier_placement       = placement;
    m_parent.set_as_dirty();
}

void GLGizmoModifiers::delete_dimension(size_t index)
{
    ModelVolume *volume = current_volume();
    if (volume == nullptr || !has_dimensions(*volume) || index >= volume->modifier_placement->dimensions.size())
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Delete dimension"));
    ModifierPlacement    placement = *volume->modifier_placement;
    placement.dimensions.erase(placement.dimensions.begin() + index);
    volume->modifier_placement = placement;
    m_warning.clear();
    finish_change();
}

// « Réappliquer » : la forme reprend la place que ses cotes lui donnent (après une coupe, une pièce
// changée, une mise à l'échelle inégale…).
void GLGizmoModifiers::reapply_dimensions()
{
    ModelVolume *volume = current_volume();
    if (volume == nullptr || !has_dimensions(*volume))
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reapply dimensions"));
    m_warning.clear();
    enforce_dimensions(*volume);
    finish_change();
}

// Une plage du tableau (croix ou Suppr).
void GLGizmoModifiers::delete_range(const t_layer_height_range &range)
{
    ModelObject *mo = model_object();
    if (mo == nullptr || mo->layer_config_ranges.count(range) == 0)
        return;
    const ModelVolume                  *current    = current_volume();
    const ObjectID                      current_id = current != nullptr ? current->id() : ObjectID();
    const std::optional<t_layer_height_range> kept_range = m_current_range != range ? m_current_range : std::nullopt;
    Plater::TakeSnapshot                snapshot(wxGetApp().plater(), _u8L("Delete height range"));
    if (m_current_range.has_value())
        select_object();
    mo->layer_config_ranges.erase(range);
    m_current_range.reset();
    m_range_buffer.reset();
    refresh_layer_ranges();
    // la sélection d'avant, si elle existe encore
    if (kept_range.has_value() && mo->layer_config_ranges.count(*kept_range) > 0)
        select_range(*kept_range);
    else if (ModelVolume *kept = current != nullptr ? volume_by_id(current_id) : nullptr)
        select_volume(kept);
    else
        select_object();
    m_parent.set_as_dirty();
}

// Fin d'un geste : la scène et le tranchage suivent ; après un changement de nature ou de forme, la
// liste d'objets est triée à nouveau et le volume resélectionné.
void GLGizmoModifiers::finish_change(ModelVolume *reordered)
{
    ModelObject *mo = model_object();
    if (mo == nullptr)
        return;
    // un intervalle : son plan passe de nouveau par les cibles de ses cotes, sa tranche couvre toute la pièce
    ModelVolume *interval = reordered != nullptr ? reordered : current_volume();
    if (interval != nullptr && !interval->is_interval())
        interval = nullptr;
    if (interval != nullptr) {
        enforce_dimensions(*interval);
        fit_interval(*interval);
    }
    if (reordered != nullptr) {
        ObjectList               *list  = wxGetApp().obj_list();
        const wxDataViewItemArray items = list->reorder_volumes_and_get_selection(object_index(), [reordered](const ModelVolume *v) { return v == reordered; });
        if (!items.IsEmpty())
            list->select_item(items.front());
        list->selection_changed();
        GLGizmosManager &manager = m_parent.get_gizmos_manager();
        if (manager.get_current_type() != GLGizmosManager::Modifiers)
            manager.open_gizmo(GLGizmosManager::Modifiers);
    } else
        wxGetApp().plater()->changed_object(*mo);
    // ses réglages suivent : sa hauteur de couche, grisée s'il est incliné
    if (interval != nullptr)
        if (auto *tab = dynamic_cast<TabPrintModel *>(wxGetApp().get_model_tab(true)); tab != nullptr && tab->has_model_config())
            tab->update_model_config();
    m_parent.set_as_dirty();
}

// Réglet ou clavier : un seul instantané d'annulation par modification, la scène est rechargée
// quand plus aucun champ n'est actif (pas de report de la transformation de l'instance ici : on est
// en train de dessiner la fenêtre).
void GLGizmoModifiers::begin_edit(const std::string &snapshot_name)
{
    if (!m_edit_snapshot) {
        wxGetApp().plater()->take_snapshot(snapshot_name);
        m_edit_snapshot = true;
    }
    m_pending_finish = true;
}

// « Charger » : un fichier 3D (STL, 3MF, OBJ, STEP…) devient la forme posée.
bool GLGizmoModifiers::load_shape_file()
{
    wxArrayString input_files;
    wxGetApp().import_model(wxGetApp().tab_panel()->GetPage(0), input_files);
    if (input_files.IsEmpty())
        return false;
    const std::string path = into_u8(input_files.Item(0));
    try {
        Model model = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::LoadModel);
        TriangleMesh mesh = model.mesh();
        if (mesh.its.indices.empty()) {
            show_error(nullptr, _L("This file contains no mesh."));
            return false;
        }
        mesh.translate(-mesh.bounding_box().center().cast<float>());
        m_loaded_mesh = std::make_shared<TriangleMesh>(std::move(mesh));
        m_loaded_name = boost::filesystem::path(path).filename().string();
        m_loaded_path = path;
    } catch (const std::exception &e) {
        show_error(nullptr, _L("Error!") + " " + _L("Failed to get the model data in the current file.") + " " + wxString::FromUTF8(e.what()));
        return false;
    }
    return true;
}

void GLGizmoModifiers::refresh_layer_ranges()
{
    wxGetApp().obj_list()->update_layer_ranges_items(object_index());
}

// =============================================================================================
// Rendu
// =============================================================================================

// Couleurs habituelles de la scène (GLVolume) ; vert pour de la matière ajoutée.
ColorRGBA GLGizmoModifiers::nature_color(Nature nature, float alpha)
{
    auto scene = [alpha](const std::array<float, 4> &rgba) { return ColorRGBA(rgba[0], rgba[1], rgba[2], alpha); };
    switch (nature) {
    case Nature::Join: return {8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, alpha};
    case Nature::Cut: return scene(GLVolume::MODEL_NEGTIVE_COL);
    case Nature::SupportBlocker: return scene(GLVolume::SUPPORT_BLOCKER_COL);
    case Nature::SupportEnforcer: return scene(GLVolume::SUPPORT_ENFORCER_COL);
    default: return scene(GLVolume::MODEL_MIDIFIER_COL);
    }
}

// Couleur d'un volume telle que la scène l'affiche (les modificateurs y prennent une couleur qui
// tranche avec les filaments de la pièce) : la pastille du tableau et les rayons X la reprennent.
ColorRGBA GLGizmoModifiers::volume_color(const ModelVolume &volume, float alpha) const
{
    const ModelObject *mo = model_object();
    if (mo != nullptr) {
        int volume_idx = -1;
        for (size_t i = 0; i < mo->volumes.size(); ++i)
            if (mo->volumes[i] == &volume)
                volume_idx = int(i);
        const int object_idx   = object_index();
        const int instance_idx = std::max(instance_index(), 0);
        for (const GLVolume *gl_volume : m_parent.get_volumes().volumes)
            if (gl_volume != nullptr && gl_volume->object_idx() == object_idx && gl_volume->volume_idx() == volume_idx &&
                gl_volume->instance_idx() == instance_idx)
                return ColorRGBA(gl_volume->color[0], gl_volume->color[1], gl_volume->color[2], alpha);
    }
    return nature_color(nature_of(volume), alpha);
}

// Aperçu d'un volume à poser : un modificateur, la couleur qu'il aura (elle tranche avec la pièce et avec les
// modificateurs déjà posés), sinon celle de sa nature.
ColorRGBA GLGizmoModifiers::ghost_color(Nature nature, float alpha) const
{
    if (nature == Nature::Modifier)
        if (const ModelObject *mo = model_object()) {
            const ColorRGB color = m_parent.new_modifier_color(*mo);
            return ColorRGBA(color.r(), color.g(), color.b(), alpha);
        }
    return nature_color(nature, alpha);
}

void GLGizmoModifiers::render_mesh(const std::shared_ptr<const TriangleMesh> &mesh, const Transform3d &world, const ColorRGBA &color)
{
    if (!mesh || mesh->its.indices.empty())
        return;
    if (m_mesh_models.size() > 64)
        m_mesh_models.clear();
    auto &entry = m_mesh_models[mesh.get()];
    if (!entry.second) {
        entry.first  = mesh;
        entry.second = std::make_shared<GLModel>();
        entry.second->init_from(mesh->its);
    }
    const Camera &camera = wxGetApp().plater()->get_camera();
    render_glmodel(*entry.second, color.get_data(), camera.get_view_matrix() * world, camera.get_projection_matrix());
}

void GLGizmoModifiers::render_sphere(const Vec3d &center, double radius, const ColorRGBA &color)
{
    if (!m_sphere_model.is_initialized())
        m_sphere_model.init_from(smooth_sphere(16, 1.0f));
    const Camera &camera = wxGetApp().plater()->get_camera();
    render_glmodel(m_sphere_model, color.get_data(), camera.get_view_matrix() * Geometry::translation_transform(center) * Geometry::scale_transform(radius),
                   camera.get_projection_matrix(), false, 0.25f);
}

// Poignées de la forme sélectionnée (boutons Pivoter / Déplacer / Échelle de sa ligne) : seulement quand on
// n'est pas en train de poser, mesurer, coter ou aligner.
bool GLGizmoModifiers::handles_visible() const
{
    const ModelVolume *volume = current_volume();
    return m_state == On && m_handle_mode != HandleMode::None && volume != nullptr && volume->id() == m_handle_volume &&
           volume->is_modifier_tool_volume() && !(m_handle_mode == HandleMode::Scale && volume->is_interval()) && !m_measuring &&
           m_ref_step == RefStep::None && m_align == AlignStep::None && !m_placing;
}

// Centre (celui de la forme), axes (ceux du plateau, comme les outils Pivoter et Déplacer) et taille
// des poignées.
void GLGizmoModifiers::handle_frame(const ModelVolume &volume, Vec3d &center, Matrix3d &axes, double &size) const
{
    // intervalle : son centre de gravité ; les axes du plateau, comme les autres formes
    if (volume.is_interval()) {
        center                = interval_center(volume);
        axes                  = Matrix3d::Identity();
        const ModelObject *mo = model_object();
        const double diagonal = mo != nullptr && instance_index() >= 0 ? mo->instance_convex_hull_bounding_box(size_t(instance_index())).size().norm() : 0.;
        size                  = std::max(0.3 * diagonal, 30. * pixel_size_at(center));
        return;
    }
    const BoundingBoxf3 box   = volume.mesh().bounding_box();
    const Transform3d   world = volume_world_matrix(volume);
    center                    = world * box.center();
    axes                      = Matrix3d::Identity();
    // (diagonale de la forme elle-même : ne change pas quand elle tourne, à l'inverse de sa boîte dans le monde)
    size                      = std::max(0.6 * (world.linear() * box.size()).norm(), 30. * pixel_size_at(center));
}

static std::array<int, 3> sides_along_plate(const Matrix3d &rotation);

// Échelle : pour chaque axe du plateau, le côté de la forme qui en est le plus proche (comme ses champs) et son cube,
// 25 pixels au-delà de la face de ce côté qui bouge le plus quand il change de taille (le centre gardé avec « Symétrie »,
// sinon le bas ou le plan de jonction, comme apply_size) ; à égalité, celle tournée vers cet axe.
std::array<GLGizmoModifiers::ScaleHandle, 3> GLGizmoModifiers::scale_handles(const ModelVolume &volume) const
{
    const BoundingBoxf3      box   = volume.mesh().bounding_box();
    const Transform3d        world = volume_world_matrix(volume);
    const std::array<int, 3> along = sides_along_plate(decompose(world, box).rotation);
    std::array<bool, 3>      set_sizes{{false, false, false}};
    if (has_dimensions(volume)) {
        Transform3d unused;
        solve_dimensions(volume, volume.modifier_placement->dimensions, unused, nullptr, &set_sizes);
    }
    const bool                 any_set = set_sizes[0] || set_sizes[1] || set_sizes[2];
    const double               px      = pixel_size_at(world * box.center());
    std::array<ScaleHandle, 3> handles;
    for (int i = 0; i < 3; ++i) {
        ScaleHandle &h     = handles[size_t(i)];
        h.side             = along[size_t(i)];
        const double fixed = m_symmetric[h.side] ? 0.5 : h.side == 2 ? placement_of(volume).base : 0.; // part du côté, depuis son bas
        const Vec3d  up    = world.linear().col(h.side).normalized(); // vers la face du haut du côté
        const bool   top   = fixed < 0.5 || (fixed == 0.5 && up(i) >= 0.);
        Vec3d        face  = box.center();
        face(h.side)       = top ? box.max(h.side) : box.min(h.side);
        h.dir              = top ? up : Vec3d(-up);
        h.tip              = world * face + 25. * px * h.dir;
        h.motion           = top ? 1. - fixed : fixed;
        h.blocked          = m_uniform_scale ? any_set : set_sizes[size_t(h.side)];
    }
    return handles;
}

// Point du monde → position de la souris (pixels, y vers le bas).
Vec2d GLGizmoModifiers::to_screen(const Vec3d &point) const
{
    const Camera             &camera   = wxGetApp().plater()->get_camera();
    const Matrix4d            pv       = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4> &viewport = camera.get_viewport();
    const Vec2d               ss       = TransformHelper::world_to_ss(point, pv, viewport);
    return {ss.x(), double(viewport[3]) - ss.y()};
}

// Anneau, flèche ou cube (sauf fixé par les cotes) sous la souris (0 X, 1 Y, 2 Z), à 8 pixels près, sinon -1.
int GLGizmoModifiers::handle_under_mouse(const Vec2d &mouse) const
{
    if (!handles_visible())
        return -1;
    Vec3d    center;
    Matrix3d axes;
    double   size = 0.;
    handle_frame(*current_volume(), center, axes, size);
    const bool                 scale = m_handle_mode == HandleMode::Scale;
    std::array<ScaleHandle, 3> cubes;
    if (scale)
        cubes = scale_handles(*current_volume());
    auto segment_distance = [&mouse](const Vec2d &a, const Vec2d &b) {
        const Vec2d  ab = b - a;
        const double l  = ab.squaredNorm();
        const double s  = l > 1e-12 ? std::clamp((mouse - a).dot(ab) / l, 0., 1.) : 0.;
        return (a + s * ab - mouse).norm();
    };
    double best      = 8.;
    int    best_axis = -1;
    for (int i = 0; i < 3; ++i) {
        if (m_handle_mode == HandleMode::Move || scale) {
            if (scale && cubes[size_t(i)].blocked)
                continue;
            const double d = segment_distance(to_screen(center), to_screen(scale ? cubes[size_t(i)].tip : Vec3d(center + size * axes.col(i))));
            if (d < best) {
                best      = d;
                best_axis = i;
            }
            continue;
        }
        const Vec3d b1   = axes.col((i + 1) % 3);
        const Vec3d b2   = axes.col((i + 2) % 3);
        Vec2d       prev = to_screen(center + size * b1);
        for (int k = 1; k <= 64; ++k) {
            const double a   = 2. * PI * k / 64.;
            const Vec2d  cur = to_screen(center + size * (std::cos(a) * b1 + std::sin(a) * b2));
            const double d   = segment_distance(prev, cur);
            if (d < best) {
                best      = d;
                best_axis = i;
            }
            prev = cur;
        }
    }
    return best_axis;
}

// Position (le long de l'axe) du point de l'axe le plus proche du rayon de la souris.
double GLGizmoModifiers::axis_parameter(const Vec3d &origin, const Vec3d &axis, const Vec2d &mouse) const
{
    Vec3d ray_origin, ray_dir;
    MeshRaycaster::line_from_mouse_pos_static(mouse, Transform3d::Identity(), wxGetApp().plater()->get_camera(), ray_origin, ray_dir);
    ray_dir.normalize();
    const Vec3d  w   = origin - ray_origin;
    const double b   = axis.dot(ray_dir);
    const double den = 1. - b * b;
    return den < 1e-3 ? 0. : (b * ray_dir.dot(w) - axis.dot(w)) / den; // (flèche vue de bout : rien)
}

// Angle (degrés, sens direct autour de l'axe) de la souris dans le plan de l'anneau, et sa distance au
// centre (pour les crans).
double GLGizmoModifiers::ring_angle(const Vec3d &center, const Vec3d &axis, const Vec2d &mouse, double &distance) const
{
    Vec3d ray_origin, ray_dir;
    MeshRaycaster::line_from_mouse_pos_static(mouse, Transform3d::Identity(), wxGetApp().plater()->get_camera(), ray_origin, ray_dir);
    ray_dir.normalize();
    const Vec3d  b1  = axis.cross(std::abs(axis.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
    const Vec3d  b2  = axis.cross(b1);
    const double den = ray_dir.dot(axis);
    Vec3d        v;
    if (std::abs(den) > 1e-6)
        v = ray_origin + ray_dir * ((center - ray_origin).dot(axis) / den) - center;
    else { // anneau vu par la tranche : le point du rayon le plus proche du centre
        const Vec3d q = ray_origin + ray_dir * (center - ray_origin).dot(ray_dir) - center;
        v             = q - axis * axis.dot(q);
    }
    distance = v.norm();
    return std::atan2(v.dot(b2), v.dot(b1)) * 180. / PI;
}

// Tourne la forme (depuis « start ») autour d'un axe passant par « center » ; sa pose garde son angle,
// l'inclinaison prend le reste ; ses cotes restent justes.
void GLGizmoModifiers::rotate_volume(ModelVolume &volume, const Transform3d &start_world, const ModifierPlacement &start_placement, const Vec3d &center,
                                     const Vec3d &axis, double degrees)
{
    const BoundingBoxf3 box   = volume.mesh().bounding_box();
    const Transform3d   world = Geometry::translation_transform(center) * Eigen::AngleAxisd(degrees * PI / 180., axis) *
                              Geometry::translation_transform(-center) * start_world;
    ModifierPlacement placement = start_placement;
    const Matrix3d    posed     = posed_frame(decompose(start_world, box), start_placement);
    placement.tilt              = (posed * rotation_z(placement.angle)).transpose() * decompose(world, box).rotation;
    volume.modifier_placement   = placement;
    set_volume_world_matrix(volume, world);
    enforce_dimensions(volume);
}

// Hors de la pièce, en glissant : la forme continue dans le plan de sa pose (elle peut dépasser) ; un
// intervalle reste où il est.
void GLGizmoModifiers::slide_off(ModelVolume &volume, const Vec2d &mouse)
{
    if (volume.is_interval())
        return;
    const BoundingBoxf3 box    = volume.mesh().bounding_box();
    Pose                pose   = decompose(volume_world_matrix(volume), box);
    const Vec3d         normal = posed_frame(pose, placement_of(volume)).col(2);
    Vec3d               ray_origin, ray_dir;
    MeshRaycaster::line_from_mouse_pos_static(mouse, Transform3d::Identity(), wxGetApp().plater()->get_camera(), ray_origin, ray_dir);
    ray_dir.normalize();
    const double den = ray_dir.dot(normal);
    if (std::abs(den) < 0.02) // rayon presque dans le plan (vers l'horizon) : la forme partirait au loin
        return;
    const Vec3d anchor = ray_origin + ray_dir * ((pose.anchor - ray_origin).dot(normal) / den);
    if ((anchor - pose.anchor).norm() > 500.)
        return;
    pose.anchor = anchor;
    set_volume_world_matrix(volume, compose(pose, box));
}

// Pivoter : crans de 45° la souris dans l'anneau, de 5° sur l'anneau et ses graduations, au degré
// au-delà (aimanté tous les 15°).
// Déplacer : la forme suit la flèche, dans les directions que ses cotes laissent libres.
// Échelle : la face du cube suit la souris ; avec « Échelle uniforme », les trois côtés dans la même proportion.
void GLGizmoModifiers::drag_handle(const Vec2d &mouse)
{
    HandleDrag  &drag   = *m_handle_drag;
    ModelVolume *volume = volume_by_id(drag.volume_id);
    if (volume == nullptr || !volume->is_modifier_tool_volume()) {
        m_handle_drag.reset();
        return;
    }
    if (!drag.moved) {
        if ((mouse - drag.start_mouse).norm() < 3.)
            return;
        drag.moved = true;
        wxGetApp().plater()->take_snapshot(drag.ring ? _u8L("Rotate modifier") : drag.scale ? _u8L("Resize modifier") : _u8L("Move modifier"));
        ensure_rigid_instance();
        volume = volume_by_id(drag.volume_id);
        if (volume == nullptr) {
            m_handle_drag.reset();
            return;
        }
        Matrix3d axes;
        handle_frame(*volume, drag.center, axes, drag.size);
        drag.axis_dir        = axes.col(drag.axis);
        drag.start_world     = volume_world_matrix(*volume);
        drag.start_placement = placement_of(*volume);
        drag.start_offset    = volume->get_offset();
        double distance      = 0.;
        drag.last_angle      = ring_angle(drag.center, drag.axis_dir, drag.start_mouse, distance);
        drag.start_param     = axis_parameter(drag.center, drag.axis_dir, drag.start_mouse);
        if (has_dimensions(*volume)) {
            Transform3d unused;
            solve_dimensions(*volume, volume->modifier_placement->dimensions, unused, &drag.free_dirs);
        }
        if (drag.scale) {
            const ScaleHandle cube = scale_handles(*volume)[size_t(drag.axis)];
            drag.side              = cube.side;
            drag.motion            = cube.motion;
            drag.center            = cube.tip;
            drag.axis_dir          = cube.dir;
            drag.start_size        = volume_size(*volume);
            drag.start_param       = axis_parameter(drag.center, drag.axis_dir, drag.start_mouse);
        }
    }
    if (drag.scale) {
        const double moved = axis_parameter(drag.center, drag.axis_dir, mouse) - drag.start_param;
        drag.value         = std::max(drag.start_size(drag.side) + moved / drag.motion, 0.1);
        Vec3d size         = drag.start_size;
        if (m_uniform_scale)
            size *= drag.value / std::max(drag.start_size(drag.side), 1e-6);
        else
            size(drag.side) = drag.value;
        apply_size(*volume, size.cwiseMax(Vec3d::Constant(0.1)));
    } else if (drag.ring) {
        double       distance = 0.;
        const double angle    = ring_angle(drag.center, drag.axis_dir, mouse, distance);
        drag.accumulated += std::remainder(angle - drag.last_angle, 360.);
        drag.last_angle = angle;
        double value    = drag.accumulated;
        if (distance < 0.9 * drag.size)
            value = 45. * std::round(value / 45.); // dans l'anneau
        else if (distance <= 1.25 * drag.size)
            value = 5. * std::round(value / 5.); // sur l'anneau et ses graduations
        else if (std::abs(value - 15. * std::round(value / 15.)) < 3.)
            value = 15. * std::round(value / 15.); // au-delà : au degré, aimanté tous les 15°
        else
            value = std::round(value);
        if (value == drag.value)
            return;
        drag.value = value;
        rotate_volume(*volume, drag.start_world, drag.start_placement, drag.center, drag.axis_dir, value);
    } else {
        drag.value         = axis_parameter(drag.center, drag.axis_dir, mouse) - drag.start_param;
        const Vec3d object = instance_matrix().linear().inverse() * (drag.value * drag.axis_dir);
        volume->set_offset(drag.start_offset + drag.free_dirs * object);
        sync_gl_volumes(*volume);
    }
}

// Valeur tapée dans un champ X / Y / Z de la ligne : position du centre de la forme sur le plateau
// (Déplacer), rotation à appliquer autour de cet axe du plateau (Pivoter), ou taille de la forme le long
// de cet axe (Échelle : champs 0 à 2 en %, 3 à 5 en mm ; « Échelle uniforme » : les trois tailles suivent).
void GLGizmoModifiers::apply_handle_field(int axis, double value)
{
    ModelVolume *volume = current_volume();
    const bool   scale  = m_handle_mode == HandleMode::Scale;
    if (volume == nullptr || !volume->is_modifier_tool_volume() || axis < 0 || axis > (scale ? 5 : 2))
        return;
    const bool     rotate = m_handle_mode == HandleMode::Rotate;
    const ObjectID id     = volume->id();
    if ((rotate && std::abs(value) < 1e-9) || (scale && value <= 0.))
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), scale ? _u8L("Resize modifier") : rotate ? _u8L("Rotate modifier") : _u8L("Move modifier"));
    ensure_rigid_instance();
    volume = volume_by_id(id);
    if (volume == nullptr)
        return;
    if (scale) {
        const Vec3d  size   = volume_size(*volume);
        const int    side   = sides_along_plate(decompose(volume_world_matrix(*volume), volume->mesh().bounding_box()).rotation)[size_t(axis % 3)];
        const double target = axis < 3 ? m_scale_reference(side) * value / 100. : value;
        Vec3d        new_size = size;
        if (!m_uniform_scale)
            new_size(side) = target;
        else if (size(side) > 1e-9)
            new_size = size * (target / size(side));
        apply_size(*volume, new_size.cwiseMax(Vec3d::Constant(0.1)));
        finish_change();
        return;
    }
    Vec3d    center;
    Matrix3d axes;
    double   size = 0.;
    handle_frame(*volume, center, axes, size);
    if (rotate)
        rotate_volume(*volume, volume_world_matrix(*volume), placement_of(*volume), center, axes.col(axis), value);
    else {
        Matrix3d free_dirs = Matrix3d::Identity();
        if (has_dimensions(*volume)) {
            Transform3d unused;
            solve_dimensions(*volume, volume->modifier_placement->dimensions, unused, &free_dirs);
        }
        const Vec3d delta = (value - center(axis)) * axes.col(axis);
        volume->set_offset(volume->get_offset() + free_dirs * (instance_matrix().linear().inverse() * delta));
        sync_gl_volumes(*volume);
    }
    finish_change();
}

// Champs X / Y / Z au bout de la ligne de la forme : position de son centre (Déplacer) ou rotation à
// appliquer (Pivoter, en degrés, le champ revient à 0).
void GLGizmoModifiers::render_handle_fields(const ModelVolume &volume)
{
    const bool  move   = m_handle_mode == HandleMode::Move;
    const float width  = 4.5f * m_imgui->get_font_size();
    Vec3d       center = Vec3d::Zero();
    Matrix3d    axes;
    double      size = 0.;
    if (move) // (Pivoter : les champs reviennent à 0, rien à lire)
        handle_frame(volume, center, axes, size);
    const char *labels[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        const ColorRGBA &c = MODIFIER_AXIS_COLORS[i];
        ImGui::SameLine();
        ImGuiWrapper::text_colored(ImVec4(c.r(), c.g(), c.b(), 1.f), labels[i]);
        ImGui::SameLine();
        ImGui::PushID(30000 + i);
        ImGui::SetNextItemWidth(width);
        if (ImGui::GetActiveID() != ImGui::GetID("##handle_field"))
            m_handle_fields[i] = move ? center(i) : 0.;
        double value = m_handle_fields[i];
        ImGui::InputDouble("##handle_field", &value, 0.0, 0.0, move ? "%.2f" : "%.1f");
        m_handle_fields[i] = value;
        if (ImGui::IsItemDeactivatedAfterEdit())
            m_pending_handle_field = std::make_pair(i, value);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(move ? _L("Position of the center of the shape on the plate (mm), then Enter.") :
                                    _L("Rotation to apply around this axis of the plate (degrees), then Enter."),
                             m_label_width + m_field_width);
        ImGui::PopID();
    }
}

// Échelle, au bout de la ligne de la forme : ses tailles le long de X, Y et Z du plateau (le côté de la forme le plus
// proche de chacun, même tournée), en % de celles qu'elle avait quand on a allumé Échelle, puis en mm ; enfin
// « Échelle uniforme ». Une taille que ses cotes fixent est grisée (toutes, en échelle uniforme).
void GLGizmoModifiers::render_scale_fields(const ModelVolume &volume)
{
    const float              width = 4.5f * m_imgui->get_font_size();
    const Vec3d              size  = volume_size(volume);
    const std::array<int, 3> along = sides_along_plate(decompose(volume_world_matrix(volume), volume.mesh().bounding_box()).rotation);
    std::array<bool, 3>      set_sizes{{false, false, false}};
    if (has_dimensions(volume)) {
        Transform3d unused;
        solve_dimensions(volume, volume.modifier_placement->dimensions, unused, nullptr, &set_sizes);
    }
    const bool  any_set   = set_sizes[0] || set_sizes[1] || set_sizes[2];
    const char *labels[3] = {"X", "Y", "Z"};
    for (int line = 0; line < 2; ++line) { // en %, puis en mm
        ImGui::AlignTextToFramePadding();
        for (int i = 0; i < 3; ++i) {
            const int        f    = 3 * line + i;
            const int        side = along[size_t(i)];
            const bool       set  = m_uniform_scale ? any_set : set_sizes[size_t(side)];
            const ColorRGBA &c    = MODIFIER_AXIS_COLORS[i];
            if (i > 0)
                ImGui::SameLine();
            ImGuiWrapper::text_colored(ImVec4(c.r(), c.g(), c.b(), 1.f), labels[i]);
            ImGui::SameLine();
            ImGui::PushID(31000 + f);
            ImGui::SetNextItemWidth(width);
            if (ImGui::GetActiveID() != ImGui::GetID("##handle_field"))
                m_handle_fields[f] = line == 0 ? 100. * size(side) / std::max(m_scale_reference(side), 1e-6) : size(side);
            double value = m_handle_fields[f];
            m_imgui->disabled_begin(set);
            ImGui::InputDouble("##handle_field", &value, 0.0, 0.0, line == 0 ? "%.1f" : "%.2f");
            m_imgui->disabled_end();
            m_handle_fields[f] = value;
            if (ImGui::IsItemDeactivatedAfterEdit() && !set)
                m_pending_handle_field = std::make_pair(f, value);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                m_imgui->tooltip(set       ? _L("Its dimensions set this value: change or delete one of them.") :
                                 line == 0 ? _L("Size along this axis of the plate (the side of the shape closest to it, even turned), in percent of its "
                                                "size when Scale was turned on; then Enter.") :
                                             _L("Size along this axis of the plate (the side of the shape closest to it, even turned), in mm; then Enter."),
                                 m_label_width + m_field_width);
            ImGui::PopID();
        }
        ImGui::SameLine();
        m_imgui->text(line == 0 ? "%" : "mm");
    }
    m_imgui->bbl_checkbox(_L("uniform scale"), m_uniform_scale);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Checked: the three sizes change together, in proportion. Unchecked: only the size typed changes."), m_label_width + m_field_width);
}

// Anneaux (ou flèches, ou cubes) aux couleurs des axes ; celui survolé ou tenu, plus épais et plus clair, avec
// ses graduations (crans de 5°) et les rayons des crans de 45° ; un anneau tenu est seul montré.
void GLGizmoModifiers::render_handles()
{
    if (!handles_visible() && !m_handle_drag.has_value())
        return;
    const ModelVolume *volume = current_volume();
    if (volume == nullptr)
        return;
    Vec3d      center;
    Matrix3d   axes;
    double     size     = 0.;
    const bool interval = volume->is_interval();
    if (interval && m_handle_drag.has_value() && m_handle_drag->moved) {
        // intervalle tourné ou déplacé : son centre du début du geste (son centre de gravité n'est pas refait à
        // chaque image), que la flèche tenue suit
        center = m_handle_drag->center + (m_handle_drag->ring ? 0. : m_handle_drag->value) * m_handle_drag->axis_dir;
        axes   = Matrix3d::Identity();
        size   = m_handle_drag->size;
    } else
        handle_frame(*volume, center, axes, size);
    if (m_handle_drag.has_value() && m_handle_drag->moved && m_handle_drag->ring) { // anneaux immobiles pendant qu'on tourne
        center = m_handle_drag->center;
        size   = m_handle_drag->size;
    }
    const double               px     = pixel_size_at(center);
    const Camera              &camera = wxGetApp().plater()->get_camera();
    const bool                 scale  = m_handle_drag.has_value() ? m_handle_drag->scale : m_handle_mode == HandleMode::Scale;
    std::array<ScaleHandle, 3> cubes;
    if (scale)
        cubes = scale_handles(*volume);
    glsafe(::glDisable(GL_DEPTH_TEST));
    for (int i = 0; i < 3; ++i) {
        if (m_handle_drag.has_value() && m_handle_drag->ring && m_handle_drag->axis != i)
            continue; // anneau tenu : les autres disparaissent jusqu'au relâchement du clic
        const bool active = m_handle_drag.has_value() ? m_handle_drag->axis == i : m_handle_hover == i;
        ColorRGBA  color  = MODIFIER_AXIS_COLORS[i];
        if (active)
            color = ColorRGBA(0.5f * (color.r() + 1.f), 0.5f * (color.g() + 1.f), 0.5f * (color.b() + 1.f), 1.f);
        const double thickness = (active ? 2.5 : 1.5) * px;
        if (scale) { // gris : taille fixée par ses cotes
            const ScaleHandle &cube = cubes[size_t(i)];
            if (cube.blocked)
                color = ColorRGBA::GRAY();
            render_segment(center, cube.tip, thickness, color);
            if (!m_cube_model.is_initialized()) {
                indexed_triangle_set mesh = its_make_cube(1., 1., 1.);
                its_translate(mesh, Vec3f(-0.5f, -0.5f, -0.5f));
                m_cube_model.init_from(mesh);
            }
            const Transform3d matrix = Geometry::translation_transform(cube.tip) * Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), cube.dir) *
                                       Geometry::scale_transform(9. * px);
            render_glmodel(m_cube_model, color.get_data(), camera.get_view_matrix() * matrix, camera.get_projection_matrix(), false, 0.25f);
            continue;
        }
        if (m_handle_mode == HandleMode::Move) {
            const Vec3d tip = center + size * axes.col(i);
            render_segment(center, tip, thickness, color);
            if (!m_cone_model.is_initialized())
                m_cone_model.init_from(its_make_cone(1., 1.));
            const Transform3d matrix = Geometry::translation_transform(tip) * Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), axes.col(i)) *
                                       Geometry::scale_transform(Vec3d(5. * px, 5. * px, 14. * px));
            render_glmodel(m_cone_model, color.get_data(), camera.get_view_matrix() * matrix, camera.get_projection_matrix(), false, 0.25f);
            continue;
        }
        const std::array<double, 8> key{center.x(), center.y(), center.z(), size, axes(0, i), axes(1, i), axes(2, i), thickness};
        if (!m_ring_models[i] || key != m_ring_keys[i]) {
            m_ring_models[i] = init_torus_data(64, 16, center.cast<float>(), float(size), float(thickness), Vec3f(axes.col(i).cast<float>()), Transform3f::Identity());
            m_ring_keys[i]   = key;
        }
        if (m_ring_models[i])
            render_glmodel(*m_ring_models[i], color.get_data(), camera.get_view_matrix(), camera.get_projection_matrix(), false, 0.25f);
        if (!active)
            continue;
        const Vec3d axis = axes.col(i);
        const Vec3d b1   = axis.cross(std::abs(axis.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY()).normalized();
        const Vec3d b2   = axis.cross(b1);
        for (int k = 0; k < 72; ++k) { // graduations : 5°, les longues tous les 10°
            const double a   = k * 5. * PI / 180.;
            const Vec3d  dir = std::cos(a) * b1 + std::sin(a) * b2;
            render_segment(center + size * dir, center + size * (k % 2 == 0 ? 1.1 : 1.05) * dir, 0.6 * px, color);
        }
        for (int k = 0; k < 8; ++k) { // rayons des crans de 45°
            const double a   = k * 45. * PI / 180.;
            const Vec3d  dir = std::cos(a) * b1 + std::sin(a) * b2;
            render_segment(center + 0.2 * size * dir, center + 0.85 * size * dir, 0.6 * px, color);
        }
    }
}

// Segment (cylindre fin) de a à b.
void GLGizmoModifiers::render_segment(const Vec3d &a, const Vec3d &b, double radius, const ColorRGBA &color)
{
    const Vec3d ab = b - a;
    if (ab.norm() < 1e-9)
        return;
    if (!m_cylinder_model.is_initialized())
        m_cylinder_model.init_from(smooth_cylinder(16, 1.0f, 1.0f));
    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Transform3d matrix = Geometry::translation_transform(a) * Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), ab) *
                               Geometry::scale_transform(Vec3d(radius, radius, ab.norm()));
    render_glmodel(m_cylinder_model, color.get_data(), camera.get_view_matrix() * matrix, camera.get_projection_matrix(), false, 0.25f);
}

// Face plane surlignée, décalée de 0,05 mm hors d'elle dans le monde.
void GLGizmoModifiers::render_plane_highlight(const PickMesh &pick, int plane, const ColorRGBA &color)
{
    if (pick.mesh == nullptr || plane < 0)
        return;
    PickMesh                                  copy      = pick;
    const std::shared_ptr<Measure::Measuring> measuring = measuring_of(copy);
    const std::vector<int>                   &triangles = measuring->get_plane_triangle_indices(plane);
    const float                               offset    = triangles.empty() ? 0.05f : highlight_offset(pick.mesh->its, pick.trafo, triangles.front());
    const std::tuple<const TriangleMesh *, int, int> key{pick.mesh, plane, int(std::lround(double(offset) * 1e6))};
    if (m_plane_models.size() > 16 && m_plane_models.count(key) == 0)
        m_plane_models.clear();
    std::shared_ptr<GLModel> &model = m_plane_models[key];
    if (!model)
        model = init_plane_data(pick.mesh->its, triangles, offset);
    if (!model)
        return;
    glsafe(::glEnable(GL_DEPTH_TEST));
    const Camera &camera = wxGetApp().plater()->get_camera();
    render_glmodel(*model, color.get_data(), camera.get_view_matrix() * pick.trafo, camera.get_projection_matrix(), false, 0.25f);
}

// Référence d'une cote : face surlignée, arête ou axe d'un cercle (son contour et son axe), point.
void GLGizmoModifiers::render_reference(const PickedRef &ref, const ColorRGBA &color)
{
    if (!ref.valid)
        return;
    if (ref.kind == RefKind::Plane) {
        if (const PickMesh *pick = pick_mesh_of(ref.volume_id, ref.on_tool))
            render_plane_highlight(*pick, ref.plane, color);
        else if (const ModelVolume *volume = volume_by_id(ref.volume_id); volume != nullptr && volume->is_interval() && ref.plane >= 0)
            render_outlines(interval_outlines(m_interval_outlines[volume->id().id], interval_planes(*volume)), color, 4.f, ref.plane); // un plan d'intervalle
        return;
    }
    glsafe(::glDisable(GL_DEPTH_TEST));
    const double px = pixel_size_at(ref.point);
    if (ref.kind == RefKind::Point) {
        if (ref.circle)
            render_circle(ref.circle_center, ref.circle_radius, ref.circle_normal, 2.0 * px, color);
        render_sphere(ref.point, 4.0 * px, color);
    } else if (ref.circle) {
        render_circle(ref.circle_center, ref.circle_radius, ref.circle_normal, 2.0 * px, color);
        const double half = std::max(ref.circle_radius, 10.0 * px);
        render_segment(ref.circle_center - half * ref.circle_normal, ref.circle_center + half * ref.circle_normal, 1.2 * px, color);
    } else
        render_segment(ref.edge_a, ref.edge_b, 2.0 * px, color);
}

// Cotes de la forme sélectionnée : de la référence de la pièce au pied de la cote (fin, gris), puis
// la cote elle-même jusqu'à la référence de la forme (bleue, rouge si elle n'est plus respectée).
void GLGizmoModifiers::render_dimensions()
{
    const ModelVolume *volume = current_volume();
    if (volume == nullptr || !has_dimensions(*volume))
        return;
    const Transform3d instance = instance_matrix();
    glsafe(::glDisable(GL_DEPTH_TEST));
    for (const ModifierDimension &dimension : volume->modifier_placement->dimensions) {
        const ColorRGBA color = dimension_error_mm(*volume, dimension) < 0.01 ? MODIFIER_DIMENSION_COLOR : MODIFIER_DIMENSION_ERROR_COLOR;
        const Vec3d     pm    = dimension_end(*volume, dimension);
        const Vec3d     pp    = instance * dimension.part_point;
        const double    px    = pixel_size_at(pm);
        if (dimension.coincident) {
            render_segment(pp, pm, 0.8 * px, MODIFIER_ANCHOR_COLOR);
            render_sphere(pm, 4.0 * px, color);
            continue;
        }
        const Vec3d dir  = (instance.linear() * dimension.direction).normalized();
        const Vec3d foot = pm - dir * dir.dot(pm - pp);
        render_segment(pp, foot, 0.8 * px, MODIFIER_ANCHOR_COLOR);
        render_segment(foot, pm, 1.5 * px, color);
        render_sphere(foot, 3.0 * px, color);
        render_sphere(pm, 3.0 * px, color);
    }
}

// Contour d'un cercle (bord d'un trou survolé), comme l'outil Coupe.
void GLGizmoModifiers::render_circle(const Vec3d &center, double radius, const Vec3d &normal, double thickness, const ColorRGBA &color)
{
    const std::array<double, 8> key{center.x(), center.y(), center.z(), radius, normal.x(), normal.y(), normal.z(), thickness};
    if (!m_circle_model || key != m_circle_key) {
        m_circle_model = init_torus_data(64, 16, center.cast<float>(), float(radius), float(thickness), normal.cast<float>(), Transform3f::Identity());
        m_circle_key   = key;
    }
    if (!m_circle_model)
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();
    render_glmodel(*m_circle_model, color.get_data(), camera.get_view_matrix(), camera.get_projection_matrix(), false, 0.25f);
}

// =============================================================================================
// Intervalles : une tranche entre deux plans (le dessous et le dessus de la boîte de son maillage, un
// cube étiré), qui couvre toute la pièce. La scène ne la dessine pas (GLVolume::is_interval) : l'outil
// montre les contours de ses deux plans sur la pièce, des liserés d'une couleur qui tranche avec elle.
// Horizontal, il est aussi une plage de hauteur pour la hauteur de couche
// (ModelObject::interval_layer_heights) ; incliné, sa hauteur de couche ne s'applique pas.
// =============================================================================================

// Pièces pleines de l'objet (celles posées par l'outil comprises) : ce que les intervalles coupent.
std::vector<const ModelVolume *> GLGizmoModifiers::solid_parts() const
{
    std::vector<const ModelVolume *> parts;
    if (const ModelObject *mo = model_object(); mo != nullptr && instance_index() >= 0)
        for (const ModelVolume *volume : mo->volumes)
            if (volume->is_model_part())
                parts.push_back(volume);
    return parts;
}

// Clé de ce qui a été calculé sur les pièces : leurs maillages et leurs poses.
std::vector<double> GLGizmoModifiers::parts_key() const
{
    std::vector<double> key;
    const Transform3d   instance = instance_matrix();
    for (const ModelVolume *part : solid_parts()) {
        key.push_back(double(reinterpret_cast<std::uintptr_t>(&part->mesh())));
        const Transform3d trafo = instance * part->get_matrix();
        key.insert(key.end(), trafo.data(), trafo.data() + 16);
    }
    return key;
}

// Étendue des pièces le long d'un axe (monde), par leurs enveloppes convexes.
void GLGizmoModifiers::parts_extent(const Vec3d &axis, double &low, double &high) const
{
    low  = std::numeric_limits<double>::max();
    high = -low;
    const Transform3d instance = instance_matrix();
    for (const ModelVolume *part : solid_parts()) {
        const Transform3d                          trafo = instance * part->get_matrix();
        const std::shared_ptr<const TriangleMesh> &hull  = part->get_convex_hull_shared_ptr();
        const indexed_triangle_set                &its   = hull && !hull->its.vertices.empty() ? hull->its : part->mesh().its;
        for (const Vec3f &v : its.vertices) {
            const double d = axis.dot(trafo * v.cast<double>());
            low            = std::min(low, d);
            high           = std::max(high, d);
        }
    }
    if (low > high)
        low = high = 0.;
}

GLGizmoModifiers::IntervalPlanes GLGizmoModifiers::interval_planes(const ModelVolume &volume) const
{
    const BoundingBoxf3 box  = volume.mesh().bounding_box();
    const Pose          pose = decompose(volume_world_matrix(volume), box);
    IntervalPlanes      planes;
    planes.origin    = pose.anchor;
    planes.frame     = pose.rotation;
    planes.thickness = std::abs(pose.scale.z()) * box.size().z();
    return planes;
}

// Contours des deux plans sur les pièces pleines (monde), et leurs traits à dessiner.
const GLGizmoModifiers::IntervalOutlines &GLGizmoModifiers::interval_outlines(IntervalOutlines &cache, const IntervalPlanes &planes)
{
    std::vector<double> key = parts_key();
    key.insert(key.end(), planes.origin.data(), planes.origin.data() + 3);
    key.insert(key.end(), planes.frame.data(), planes.frame.data() + 9);
    key.push_back(planes.thickness);
    if (key == cache.key)
        return cache;
    cache     = IntervalOutlines();
    cache.key = std::move(key);
    // repère du plan du bas : ses axes, puis la hauteur le long de sa normale
    Transform3d to_plane   = Transform3d::Identity();
    to_plane.linear()      = planes.frame.transpose();
    to_plane.translation() = -(planes.frame.transpose() * planes.origin);
    // un rien dans la tranche : un plan posé sur une face de la pièce (son dessous) la coupe quand même
    const double                inset    = std::min(0.005, 0.25 * planes.thickness);
    const std::array<double, 2> heights  = {inset, planes.thickness - inset};
    const Transform3d           instance = instance_matrix();
    for (const ModelVolume *part : solid_parts()) {
        MeshSlicingParams params;
        params.trafo = to_plane * instance * part->get_matrix();
        for (int i = 0; i < 2; ++i)
            for (const Polygon &polygon : slice_mesh(part->mesh().its, float(heights[i]), params)) {
                if (polygon.points.size() < 2)
                    continue;
                std::vector<Vec3d> loop;
                loop.reserve(polygon.points.size());
                for (const Point &p : polygon.points)
                    loop.emplace_back(planes.origin + planes.frame * Vec3d(unscale<double>(p.x()), unscale<double>(p.y()), heights[i]));
                cache.loops[i].push_back(std::move(loop));
            }
    }
    for (int i = 0; i < 2; ++i) {
        if (cache.loops[i].empty())
            continue;
        GLModel::Geometry geometry;
        geometry.format    = {GLModel::PrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
        unsigned int count = 0;
        for (const std::vector<Vec3d> &loop : cache.loops[i]) {
            for (size_t k = 0; k < loop.size(); ++k) {
                geometry.add_vertex(Vec3f(loop[k].cast<float>()));
                geometry.add_line(count + unsigned(k), count + unsigned((k + 1) % loop.size()));
            }
            count += unsigned(loop.size());
        }
        cache.models[i] = std::make_shared<GLModel>();
        cache.models[i]->init_from(std::move(geometry));
    }
    return cache;
}

// Centre de gravité de la matière entre les deux plans : le pivot de « Pivoter » ; sans matière entre eux, le
// centre de la pièce ramené au milieu de la tranche.
Vec3d GLGizmoModifiers::interval_center(const ModelVolume &volume) const
{
    const IntervalPlanes planes = interval_planes(volume);
    std::vector<double>  key    = parts_key();
    key.insert(key.end(), planes.origin.data(), planes.origin.data() + 3);
    key.insert(key.end(), planes.frame.data(), planes.frame.data() + 9);
    key.push_back(planes.thickness);
    std::pair<std::vector<double>, Vec3d> &cached = m_interval_centers[volume.id().id];
    if (cached.first == key)
        return cached.second;
    Transform3d to_plane   = Transform3d::Identity();
    to_plane.linear()      = planes.frame.transpose();
    to_plane.translation() = -(planes.frame.transpose() * planes.origin);
    const Transform3d instance = instance_matrix();
    double            total    = 0.;
    Vec3d             moment   = Vec3d::Zero();
    for (const ModelVolume *part : solid_parts()) {
        // la pièce dans le repère du plan du bas, coupée à ses deux plans (faces de coupe refermées)
        indexed_triangle_set its = part->mesh().its;
        its_transform(its, Transform3d(to_plane * instance * part->get_matrix()), true);
        indexed_triangle_set upper, band;
        cut_mesh(its, 0.f, &upper, nullptr);
        cut_mesh(upper, float(planes.thickness), nullptr, &band);
        // somme des tétraèdres (origine, triangle), comme le centre d'un volume (corners_of)
        for (const auto &triangle : band.indices) {
            const Vec3d  a = band.vertices[triangle[0]].cast<double>();
            const Vec3d  b = band.vertices[triangle[1]].cast<double>();
            const Vec3d  c = band.vertices[triangle[2]].cast<double>();
            const double v = a.dot(b.cross(c)) / 6.;
            total += v;
            moment += v * (a + b + c) / 4.;
        }
    }
    const Vec3d n = planes.frame.col(2);
    Vec3d       center;
    if (std::abs(total) > 1e-9)
        center = planes.origin + planes.frame * Vec3d(moment / total);
    else {
        const ModelObject *mo     = model_object();
        const Vec3d        middle = planes.origin + 0.5 * planes.thickness * n;
        const Vec3d        c      = mo != nullptr && instance_index() >= 0 ? mo->instance_convex_hull_bounding_box(size_t(instance_index())).center() : middle;
        center                    = c - n * n.dot(c - middle);
    }
    cached = std::make_pair(std::move(key), center);
    return center;
}

// La tranche couvre de nouveau toute la pièce : ses côtés suivent la pièce vue le long de son axe (plus 1 mm) ;
// ses deux plans ne bougent pas.
void GLGizmoModifiers::fit_interval(ModelVolume &volume)
{
    const BoundingBoxf3 box  = volume.mesh().bounding_box();
    Pose                pose = decompose(volume_world_matrix(volume), box);
    double              low[2], high[2];
    for (int i = 0; i < 2; ++i)
        parts_extent(pose.rotation.col(i), low[i], high[i]);
    if (high[0] <= low[0] || high[1] <= low[1] || box.size().x() < 1e-9 || box.size().y() < 1e-9)
        return;
    const Vec3d n  = pose.rotation.col(2);
    pose.anchor    = 0.5 * (low[0] + high[0]) * pose.rotation.col(0) + 0.5 * (low[1] + high[1]) * pose.rotation.col(1) + n.dot(pose.anchor) * n;
    pose.scale.x() = (high[0] - low[0] + 2.) / box.size().x();
    pose.scale.y() = (high[1] - low[1] + 2.) / box.size().y();
    set_volume_world_matrix(volume, compose(pose, box));
}

// Plan du bas (« offset » : sa position le long de son axe) et épaisseur d'un intervalle, son axe gardé ; ses
// cotes restent justes.
void GLGizmoModifiers::set_interval_planes(ModelVolume &volume, double offset, double thickness)
{
    const BoundingBoxf3 box = volume.mesh().bounding_box();
    if (box.size().z() < 1e-9)
        return;
    Pose        pose = decompose(volume_world_matrix(volume), box);
    const Vec3d n    = pose.rotation.col(2);
    pose.anchor += (offset - n.dot(pose.anchor)) * n;
    pose.scale.z() = std::max(thickness, 0.01) / box.size().z();
    set_volume_world_matrix(volume, compose(pose, box));
    enforce_dimensions(volume);
}

// Référence d'un intervalle : le contour de son plan du bas ou du haut le plus proche de la souris, à 10 pixels
// près ; la référence est ce plan (une face).
GLGizmoModifiers::PickedRef GLGizmoModifiers::pick_interval_reference(const ModelVolume &volume, const Vec2d &mouse)
{
    PickedRef               ref;
    const IntervalPlanes    planes   = interval_planes(volume);
    const IntervalOutlines &outlines = interval_outlines(m_interval_outlines[volume.id().id], planes);
    double                  best     = 10.;
    for (int i = 0; i < 2; ++i)
        for (const std::vector<Vec3d> &loop : outlines.loops[i])
            for (size_t k = 0; k < loop.size(); ++k) {
                const Vec3d  &a  = loop[k];
                const Vec3d  &b  = loop[(k + 1) % loop.size()];
                const Vec2d   sa = to_screen(a);
                const Vec2d   ab = to_screen(b) - sa;
                const double  l  = ab.squaredNorm();
                const double  s  = l > 1e-12 ? std::clamp((mouse - sa).dot(ab) / l, 0., 1.) : 0.;
                const double  d  = (sa + s * ab - mouse).norm();
                if (d < best) {
                    best      = d;
                    ref.valid = true;
                    ref.point = a + s * (b - a);
                    ref.plane = i;
                }
            }
    if (!ref.valid)
        return ref;
    ref.kind      = RefKind::Plane;
    ref.on_tool   = true;
    ref.volume_id = volume.id();
    ref.dir       = ref.plane == 0 ? Vec3d(-planes.frame.col(2)) : Vec3d(planes.frame.col(2));
    ref.name      = ref.plane == 0 ? _L("Bottom of the interval") : _L("Top of the interval");
    return ref;
}

// Référence sur le liseré d'un intervalle, pour coter une autre forme (côté pièce) : un de ses sommets (coin du
// contour) à 10 pixels près, sinon son arête (le côté droit du contour, d'un coin à l'autre) à 8 pixels près ; Maj,
// ou un contour courbe : le point du liseré le plus proche. Gardée dans le repère de l'objet comme une référence
// de la pièce (ponytail : elle ne suit pas l'intervalle s'il bouge ensuite ; la lier à lui si cela compte).
GLGizmoModifiers::PickedRef GLGizmoModifiers::pick_outline_reference(const Vec2d &mouse, bool point_only)
{
    PickedRef          ref;
    const ModelObject *mo      = model_object();
    const ModelVolume *current = current_volume();
    if (mo == nullptr || instance_index() < 0)
        return ref;
    // un coin : le contour y tourne de plus de 15°
    auto is_corner = [](const std::vector<Vec3d> &loop, size_t k) {
        const size_t n  = loop.size();
        const Vec3d  d0 = loop[k] - loop[(k + n - 1) % n];
        const Vec3d  d1 = loop[(k + 1) % n] - loop[k];
        return d0.norm() > 1e-9 && d1.norm() > 1e-9 && d0.normalized().dot(d1.normalized()) < std::cos(15. * PI / 180.);
    };
    const std::vector<Vec3d> *best_loop   = nullptr;
    size_t                    best_k      = 0;
    double                    best_s      = 0.;
    double                    best_edge   = 8.;
    double                    best_vertex = 10.;
    std::optional<Vec3d>      vertex;
    ObjectID                  vertex_id, edge_id;
    for (const ModelVolume *volume : mo->volumes) {
        if (!volume->is_interval() || volume == current) // (pas le liseré de l'intervalle que l'on cote)
            continue;
        const IntervalOutlines &outlines = interval_outlines(m_interval_outlines[volume->id().id], interval_planes(*volume));
        for (int i = 0; i < 2; ++i)
            for (const std::vector<Vec3d> &loop : outlines.loops[i])
                for (size_t k = 0; k < loop.size(); ++k) {
                    const Vec2d  sa = to_screen(loop[k]);
                    const Vec2d  ab = to_screen(loop[(k + 1) % loop.size()]) - sa;
                    const double l  = ab.squaredNorm();
                    const double s  = l > 1e-12 ? std::clamp((mouse - sa).dot(ab) / l, 0., 1.) : 0.;
                    const double d  = (sa + s * ab - mouse).norm();
                    if (d < best_edge) {
                        best_edge = d;
                        best_loop = &loop;
                        best_k    = k;
                        best_s    = s;
                        edge_id   = volume->id();
                    }
                    if ((sa - mouse).norm() < best_vertex && is_corner(loop, k)) {
                        best_vertex = (sa - mouse).norm();
                        vertex      = loop[k];
                        vertex_id   = volume->id();
                    }
                }
    }
    if (vertex.has_value()) {
        ref.valid     = true;
        ref.kind      = RefKind::Point;
        ref.point     = *vertex;
        ref.volume_id = vertex_id;
        ref.name      = _L("Vertex of an outline");
        return ref;
    }
    if (best_loop == nullptr)
        return ref;
    const std::vector<Vec3d> &loop = *best_loop;
    const size_t              n    = loop.size();
    ref.valid                      = true;
    ref.kind                       = RefKind::Point;
    ref.point                      = loop[best_k] + best_s * (loop[(best_k + 1) % n] - loop[best_k]);
    ref.volume_id                  = edge_id;
    ref.name                       = _L("Point of an outline");
    if (point_only)
        return ref;
    // son arête : du coin d'avant au coin d'après, si le contour reste droit entre eux
    size_t first = best_k, last = (best_k + 1) % n, steps = 0;
    while (!is_corner(loop, first) && steps < n) {
        first = (first + n - 1) % n;
        ++steps;
    }
    while (!is_corner(loop, last) && steps < n) {
        last = (last + 1) % n;
        ++steps;
    }
    if (steps >= n || (loop[last] - loop[first]).norm() < 1e-6)
        return ref; // aucun coin (contour courbe) : le point
    const Vec3d a   = loop[first];
    const Vec3d b   = loop[last];
    const Vec3d dir = (b - a).normalized();
    for (size_t k = first; k != last; k = (k + 1) % n)
        if (((loop[k] - a) - (loop[k] - a).dot(dir) * dir).norm() > 0.01)
            return ref; // pas droit : le point
    ref.kind   = RefKind::Axis;
    ref.point  = a;
    ref.dir    = dir;
    ref.edge_a = a;
    ref.edge_b = b;
    ref.name   = _L("Edge of an outline");
    return ref;
}

// Couleur qui tranche avec les filaments de la pièce : « own » si elle tranche assez, sinon une autre.
ColorRGBA GLGizmoModifiers::standing_out(const ColorRGBA &own, float alpha) const
{
    static const std::vector<ColorRGB> others = {
        ColorRGB(0.90f, 0.10f, 0.80f), // magenta
        ColorRGB(0.00f, 0.80f, 1.00f), // cyan
        ColorRGB(1.00f, 0.55f, 0.00f), // orange
        ColorRGB(0.60f, 0.25f, 1.00f), // violet
        ColorRGB(0.00f, 0.68f, 0.26f), // vert
        ColorRGB(1.00f, 0.90f, 0.10f), // jaune
        ColorRGB(0.20f, 0.40f, 1.00f), // bleu
        ColorRGB(0.95f, 0.15f, 0.15f)  // rouge
    };
    std::vector<ColorRGB> preferred{ColorRGB(own.r(), own.g(), own.b())};
    preferred.insert(preferred.end(), others.begin(), others.end());
    const ModelObject *mo    = model_object();
    const ColorRGB     color = mo != nullptr ? m_parent.color_standing_out(*mo, preferred) : preferred.front();
    return ColorRGBA(color.r(), color.g(), color.b(), alpha);
}

// Contours (traits) : nets là où on les voit, pâles et fins là où la pièce les cache ; avancés de deux pixels
// vers la caméra (ils sont sur la surface de la pièce). « only » : le plan du bas (0) ou du haut (1) seul.
void GLGizmoModifiers::render_outlines(const IntervalOutlines &outlines, const ColorRGBA &color, float width, int only)
{
    const auto &shader = wxGetApp().get_shader("flat");
    if (!shader)
        return;
    const Camera   &camera     = wxGetApp().plater()->get_camera();
    const auto     &ogl        = wxGetApp().get_opengl_manager();
    const GLboolean depth_test = ::glIsEnabled(GL_DEPTH_TEST);
    glsafe(::glEnable(GL_DEPTH_TEST));
    wxGetApp().bind_shader(shader);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    for (int i = 0; i < 2; ++i) {
        if (!outlines.models[i] || (only >= 0 && only != i))
            continue;
        GLModel     &model = *outlines.models[i];
        const double px    = pixel_size_at(outlines.loops[i].front().front());
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(-2. * px * camera.get_dir_forward()));
        glsafe(::glDepthFunc(GL_GREATER)); // cachés par la pièce
        if (ogl)
            ogl->set_line_width(1.f);
        model.set_color(ColorRGBA(color.r(), color.g(), color.b(), 0.35f * color.a()));
        model.render_geometry();
        glsafe(::glDepthFunc(GL_LEQUAL)); // vus
        if (ogl)
            ogl->set_line_width(width);
        model.set_color(color);
        model.render_geometry();
    }
    glsafe(::glDepthFunc(GL_LESS));
    if (ogl)
        ogl->set_line_width(1.f);
    wxGetApp().unbind_shader();
    if (!depth_test)
        glsafe(::glDisable(GL_DEPTH_TEST));
}

// Intervalles de l'objet : les contours de leurs deux plans (plus épais pour celui sélectionné) ; la plage de
// hauteur choisie dans la liste d'objets de même.
void GLGizmoModifiers::render_intervals()
{
    const ModelObject *mo = model_object();
    if (mo == nullptr || instance_index() < 0)
        return;
    const ModelVolume *current = current_volume();
    std::set<size_t>   shown;
    for (const ModelVolume *volume : mo->volumes) {
        if (!volume->is_interval())
            continue;
        shown.insert(volume->id().id);
        // (un modificateur : la couleur que la scène lui donne ; choisie par l'utilisateur, telle quelle, sinon une qui
        // tranche avec la pièce)
        const ColorRGBA own    = volume->is_modifier() ? volume_color(*volume, 1.f) : nature_color(nature_of(*volume), 1.f);
        const bool      chosen = volume->is_modifier() && volume->modifier_placement.has_value() && !volume->modifier_placement->color.empty();
        render_outlines(interval_outlines(m_interval_outlines[volume->id().id], interval_planes(*volume)), chosen ? own : standing_out(own, 1.f),
                        volume == current ? 3.f : 1.5f);
    }
    for (auto it = m_interval_outlines.begin(); it != m_interval_outlines.end();)
        it = shown.count(it->first) > 0 ? std::next(it) : m_interval_outlines.erase(it);
    for (auto it = m_interval_centers.begin(); it != m_interval_centers.end();)
        it = shown.count(it->first) > 0 ? std::next(it) : m_interval_centers.erase(it);
    if (m_current_range.has_value()) {
        const t_layer_height_range range = m_range_buffer.value_or(*m_current_range);
        const BoundingBoxf3        box   = mo->instance_bounding_box(size_t(instance_index()));
        IntervalPlanes             planes;
        planes.origin    = Vec3d(box.center().x(), box.center().y(), box.min.z() + range.first);
        planes.thickness = range.second - range.first;
        render_outlines(interval_outlines(m_range_outlines, planes), standing_out(MODIFIER_RANGE_COLOR, 1.f), 3.f);
    }
}

void GLGizmoModifiers::on_render()
{
    if (model_object() == nullptr)
        return;
    sync_with_selection();
    // « Mesurer » : seulement les repères et les cotes de la Mesure
    if (m_measuring && m_measure) {
        m_measure->render();
        return;
    }
    update_picks();
    // Maj enfoncée ou relâchée sans bouger la souris : la touche a pu aller à la fenêtre de l'outil (qui
    // garde le clavier après un clic dedans) et non à l'outil ; son état est relu à chaque image (pas avec
    // la souris sur une fenêtre : le survol y est éteint)
    if (const bool shift = wxGetKeyState(WXK_SHIFT); shift != m_shift_down && !mouse_on_imgui()) {
        if (m_align != AlignStep::None || (m_placing && !m_drag.has_value()))
            update_hover(m_mouse, shift);
        else {
            m_shift_down = shift;
            if (m_ref_step == RefStep::VolumeRef || m_ref_step == RefStep::PartRef)
                m_ref_hover = pick_reference(m_mouse, m_ref_step == RefStep::VolumeRef, shift);
        }
    }

    GLint depth_func = GL_LESS;
    glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_func));
    const GLboolean depth_test = ::glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blend      = ::glIsEnabled(GL_BLEND);
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    render_xray();
    render_intervals();
    render_ghost();

    glsafe(::glDepthFunc(depth_func));
    glsafe(::glDepthMask(GL_TRUE));
    render_markers();
    render_handles();

    glsafe(::glDepthFunc(depth_func));
    glsafe(::glDepthMask(GL_TRUE));
    if (depth_test)
        glsafe(::glEnable(GL_DEPTH_TEST));
    else
        glsafe(::glDisable(GL_DEPTH_TEST));
    if (!blend)
        glsafe(::glDisable(GL_BLEND));
}

// Volumes de l'outil cachés par la pièce : dessinés pâles par transparence (rayons X), plus nets
// pour le volume sélectionné. Leur partie visible est déjà dessinée par la scène.
void GLGizmoModifiers::render_xray()
{
    const ModelVolume *current = current_volume();
    glsafe(::glDepthFunc(GL_GREATER));
    glsafe(::glDepthMask(GL_FALSE));
    for (const PickMesh &pick : m_tools) {
        const ModelVolume *volume = volume_by_id(pick.volume_id);
        if (volume == nullptr)
            continue;
        render_mesh(volume->get_mesh_shared_ptr(), pick.trafo, volume_color(*volume, volume == current ? 0.4f : 0.18f));
    }
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthMask(GL_TRUE));
}

// Aperçu de ce que le clic va poser (« Placer »), au point aimanté (et pâle là où la pièce le
// cache) ; pour un intervalle, les contours de ses deux plans, horizontaux, son bas au point visé ;
// « Depuis une face », la face entière, éclairée.
void GLGizmoModifiers::render_ghost()
{
    const bool placing = m_state == On && m_placing && !m_drag.has_value() && m_align == AlignStep::None && m_hover.valid && m_hover_tool < 0;
    if (!placing || m_shape == Shape::Svg)
        return;
    if (m_shape == Shape::Face) {
        FaceSelection &face = m_face_selection;
        if (!face.mesh || face.facets.empty())
            return;
        if (!face.highlight)
            face.highlight = init_plane_data(face.mesh->its, face.facets, highlight_offset(face.mesh->its, face.trafo, face.facets.front()));
        if (!face.highlight)
            return;
        glsafe(::glEnable(GL_DEPTH_TEST));
        const Camera &camera = wxGetApp().plater()->get_camera();
        render_glmodel(*face.highlight, ghost_color(m_nature, 0.6f).get_data(), camera.get_view_matrix() * face.trafo, camera.get_projection_matrix(), false,
                       0.25f);
        return;
    }
    if (m_shape == Shape::Interval) {
        IntervalPlanes planes;
        planes.origin    = m_hover.point;
        planes.thickness = m_range_height;
        render_outlines(interval_outlines(m_ghost_outlines, planes), standing_out(ghost_color(m_nature, 1.f), 1.f), 2.f);
        return;
    }
    glsafe(::glDepthMask(GL_TRUE));
    const std::shared_ptr<const TriangleMesh> mesh = panel_mesh();
    if (!mesh)
        return;
    const BoundingBoxf3 box = mesh->bounding_box();
    const Vec3d         size = default_size(m_shape);
    Pose                pose;
    pose.anchor   = m_hover.point;
    pose.rotation = surface_frame(m_hover.normal, m_hover.xref, default_outside(m_nature)) * rotation_z(m_new_angle);
    for (int i = 0; i < 3; ++i)
        pose.scale(i) = box.size()(i) > 1e-9 ? std::max(size(i), 0.01) / box.size()(i) : 1.;
    const Transform3d world = compose(pose, box);
    glsafe(::glDepthMask(GL_FALSE));
    render_mesh(mesh, world, ghost_color(m_nature, 0.45f));
    glsafe(::glDepthFunc(GL_GREATER));
    render_mesh(mesh, world, ghost_color(m_nature, 0.15f));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthMask(GL_TRUE));
}

// Repères : cotes de la forme sélectionnée ; pendant les références, celle survolée (orange) et
// celle choisie (vert) ; pendant « Joindre », faces et points (survolés orange, choisis verts, ancrages
// gris) ; sinon, ancrages de la face survolée (gris) et celui où le point s'aimante (vert).
void GLGizmoModifiers::render_markers()
{
    render_dimensions();

    if (m_ref_step != RefStep::None) {
        if (m_ref_first.valid && m_ref_step != RefStep::VolumeRef)
            render_reference(m_ref_first, MODIFIER_ANCHOR_HOVER_COLOR);
        if (m_ref_second.valid && m_ref_step == RefStep::Value)
            render_reference(m_ref_second, MODIFIER_ANCHOR_HOVER_COLOR);
        // Maj sur une forme ronde : ses anneaux, de la couleur de l'axe normal à leur plan, comme « Joindre »
        if (m_ref_step == RefStep::VolumeRef && m_shift_down)
            if (const ModelVolume *volume = current_volume(); volume != nullptr)
                if (const PickMesh *own = pick_mesh_of(volume->id(), true)) {
                    PickMesh mesh = *own;
                    glsafe(::glDisable(GL_DEPTH_TEST));
                    for (const PrincipalPoint &q : ring_points(mesh))
                        render_sphere(q.point, 3.0 * pixel_size_at(q.point), MODIFIER_AXIS_COLORS[q.axis]);
                }
        if (m_ref_hover.valid && (m_ref_step == RefStep::VolumeRef || m_ref_step == RefStep::PartRef)) {
            render_reference(m_ref_hover, MODIFIER_ALIGN_COLOR);
            // aligné sur le premier point : le trait qui les relie, de la couleur de l'axe
            if (m_ref_hover.aligned >= 0 && m_ref_first.valid)
                render_segment(m_ref_first.point, m_ref_hover.point, 0.8 * pixel_size_at(m_ref_hover.point), MODIFIER_AXIS_COLORS[m_ref_hover.aligned % 3]);
        }
        return;
    }

    // « Joindre » : ce qui est choisi sur la forme et la face choisie (vert), la face survolée (orange),
    // les ancrages montrés (gris), le point survolé (orange)
    if (m_align != AlignStep::None) {
        auto highlight = [this](const JointPick &pick, const ColorRGBA &color) {
            if (pick.valid && pick.face && pick.plane >= 0)
                if (const PickMesh *mesh = pick_mesh_of(pick.volume_id, pick.on_tool))
                    render_plane_highlight(*mesh, pick.plane, color);
        };
        if (m_align == AlignStep::TargetPoint)
            highlight(m_join_first, MODIFIER_ANCHOR_HOVER_COLOR);
        highlight(m_join_lock, MODIFIER_ANCHOR_HOVER_COLOR);
        if (!m_join_lock.valid || m_join_hover.volume_id != m_join_lock.volume_id || m_join_hover.plane != m_join_lock.plane)
            highlight(m_join_hover, MODIFIER_ALIGN_COLOR);
        glsafe(::glDisable(GL_DEPTH_TEST));
        for (const auto &[p, axis] : m_join_points)
            render_sphere(p, 3.0 * pixel_size_at(p), axis >= 0 ? MODIFIER_AXIS_COLORS[axis] : MODIFIER_ANCHOR_COLOR);
        if (m_align == AlignStep::TargetPoint && m_join_first.valid && !m_join_first.face)
            render_sphere(m_join_first.point, 5.0 * pixel_size_at(m_join_first.point), MODIFIER_ANCHOR_HOVER_COLOR);
        if (m_join_hover.valid && !m_join_hover.face)
            render_sphere(m_join_hover.point, 5.0 * pixel_size_at(m_join_hover.point), MODIFIER_ALIGN_COLOR);
        return;
    }

    // ancrages : pendant « Placer » (pas pour « Depuis une face » : la face entière est prise) ou en glissant
    // une forme posée ; Maj pendant « Placer » : tous les sommets et le centre de chaque volume
    const bool active = m_drag.has_value() || (m_placing && m_hover_tool < 0 && m_shape != Shape::Face);
    if (!active)
        return;
    glsafe(::glDisable(GL_DEPTH_TEST));
    if (!m_drag.has_value())
        for (const auto &[p, axis] : m_join_points)
            render_sphere(p, 3.0 * pixel_size_at(p), axis >= 0 ? MODIFIER_AXIS_COLORS[axis] : MODIFIER_ANCHOR_COLOR);
    if (!m_hover.valid)
        return;
    // bord d'un trou survolé : son contour
    if (m_magnetism && m_hover.snapped && m_hover.on_circle)
        render_circle(m_hover.circle_center, m_hover.circle_radius, m_hover.circle_normal, 2.0 * pixel_size_at(m_hover.circle_center),
                      MODIFIER_ANCHOR_HOVER_COLOR);
    if (m_magnetism && m_hover.plane >= 0 && m_face.part == m_hover.part && m_face.plane == m_hover.plane)
        for (const FaceAnchors::Anchor &anchor : m_face.face.anchors) {
            if (anchor.kind >= FaceAnchors::AnchorKind::Fraction)
                continue;
            const Vec3d p = m_face.origin + anchor.pos.x() * m_face.axis_x + anchor.pos.y() * m_face.axis_y;
            render_sphere(p, 3.0 * pixel_size_at(p), MODIFIER_ANCHOR_COLOR);
        }
    if (m_hover.snapped)
        render_sphere(m_hover.point, 5.0 * pixel_size_at(m_hover.point), MODIFIER_ANCHOR_HOVER_COLOR);
}

// =============================================================================================
// Fenêtre
// =============================================================================================

// Réglet + cote saisie au clavier (comme les connecteurs de la coupe) ; une cote au-delà du réglet
// reste possible, le réglet s'allonge jusqu'à elle.
bool GLGizmoModifiers::slider_row(const wxString &label, const std::string &id, double &value, double lo, double hi, const char *format, float reserve)
{
    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(m_label_width);
    const float field_width  = m_field_width - reserve; // « reserve » : place laissée au bout de la ligne
    const float slider_width = 0.6f * field_width;
    const float gap          = 0.02f * field_width;
    ImGui::SetNextItemWidth(slider_width);
    bool  changed      = false;
    float slider_value = float(value);
    if (m_imgui->bbl_slider_float_style("##slider_" + id, &slider_value, float(lo), float(std::max(hi, value)), format)) {
        value   = double(slider_value);
        changed = true;
    }
    ImGui::SameLine(m_label_width + slider_width + gap);
    ImGui::SetNextItemWidth(field_width - slider_width - gap);
    double input_value = value;
    if (ImGui::BBLInputDouble(("##input_" + id).c_str(), &input_value, 0.0, 0.0, format)) {
        value   = std::max(input_value, lo);
        changed = true;
    }
    return changed;
}

void GLGizmoModifiers::on_render_input_window(float x, float y, float bottom_limit)
{
    ModelObject *mo = model_object();
    if (mo == nullptr)
        return;
    sync_with_selection();

    // Orio : fenêtre flottante et réductible du cadre commun (ImGuiWrapper::begin_tool_window, ouverte par
    // GizmoImguiBegin) : place par défaut sous l'icône ; déplacée, elle garde sa place (mémorisée par le cadre).
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    // raccourcis : montrés par l'icône clavier de la barre de titre, comme les autres outils
    m_imgui->tool_window_shortcuts({{_L("Left mouse button") + ": ", _L("Place: puts the shape on the part; on a shape: selects and drags it")},
                                    {_L("Shift + Left mouse button") + ": ", _L("Place and Join: also every vertex and the center of each volume. References: a point")},
                                    {_L("Esc") + ": ", _L("One step back: field, reference, Join, Place, selection, then closes the tool")},
                                    {_L("Del") + ": ", _L("Deletes the selected shape (or the dimension whose field is open)")}});

    const float font = m_imgui->get_font_size();
    float       label_size = 0.f;
    for (const wxString &label : {_L("Nature"), _L("Shape"), _L("Length"), _L("Width"), _L("Height"), _L("Diameter"), _L("Outer diameter"),
                                  _L("Shape thickness"), _L("Range bottom"), _L("Range top"), _L("Interval bottom"), _L("Interval top")})
        label_size = std::max(label_size, m_imgui->calc_text_size(label).x);
    m_label_width = label_size + 2.f * font;
    m_field_width = 14.f * font;
    const float wrap_width = m_label_width + m_field_width;

    m_imgui->set_requires_extra_frame();
    ModelVolume *volume = current_volume();

    // « Mesurer », tout en haut à droite (la 1re ligne lui laisse sa place) ; ses résultats juste dessous
    {
        const float  size  = ImGui::GetFrameHeight();
        const ImVec2 start = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(start.x + wrap_width - size, start.y));
        render_measure_button(size);
        ImGui::SetCursorPos(start);
        m_top_row_reserve = size + ImGui::GetStyle().ItemSpacing.x;
    }

    if (volume != nullptr && !volume->is_modifier_tool_volume()) {
        // texte ou SVG : ils ont leur outil (et « Placer » s'éteint, son bouton n'est pas là)
        m_placing = false;
        m_imgui->text(wxString::FromUTF8(volume->name));
        if (m_measuring)
            render_measure_panel();
        const bool is_text = volume->is_text();
        m_imgui->text_wrapped(is_text ? _L("This text is edited with the Text tool.") : _L("This SVG is edited with the SVG tool."), wrap_width);
        if (m_imgui->button(is_text ? _L("Open the Text tool") : _L("Open the SVG tool")))
            m_pending_tool = is_text ? int(GLGizmosManager::Emboss) : int(GLGizmosManager::Svg);
    } else {
        // Forme, Nature ; les cotes sont celles de la ligne sélectionnée, sous son nom dans le tableau (la forme à
        // poser reprend les dernières, elle se règle une fois posée)
        render_shape_input(volume);
        if (m_measuring)
            render_measure_panel();
        render_nature_input(volume);
    }
    if (!m_warning.IsEmpty())
        m_imgui->warning_text_wrapped(m_warning, wrap_width);

    // « Placer » au-dessus du tableau (pas pour un texte ou un SVG : ils ont leur outil), puis le tableau, une
    // croix au bout de chaque ligne pour la supprimer
    if (volume == nullptr || volume->is_modifier_tool_volume())
        render_place_input(volume);
    render_table();

    // Orio: read before the end of the window, in its ImGui context (its own one when it is detached).
    const bool any_item_active = ImGui::IsAnyItemActive();
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();

    // cotes : leurs étiquettes en 3D (cliquables), puis le champ de la distance
    render_dimension_labels();
    render_dimension_popup();

    // actions qui changent la sélection, la liste d'objets ou ferment l'outil : après la fenêtre
    if (m_pending_nature.has_value()) {
        const Nature nature = *m_pending_nature;
        m_pending_nature.reset();
        if (ModelVolume *v = current_volume(); v != nullptr && v->is_modifier_tool_volume())
            change_nature(*v, nature);
    }
    if (m_pending_shape.has_value()) {
        const Shape shape = *m_pending_shape;
        m_pending_shape.reset();
        if (ModelVolume *v = current_volume(); v != nullptr && v->is_modifier_tool_volume())
            change_shape(*v, shape);
    }
    if (m_pending_flip.has_value()) {
        const ObjectID id = *m_pending_flip;
        m_pending_flip.reset();
        if (ModelVolume *v = volume_by_id(id); v != nullptr && v->is_modifier_tool_volume()) {
            if (v != current_volume())
                select_volume(v);
            // l'outil en cours laisse la place
            m_align       = AlignStep::None;
            m_handle_mode = HandleMode::None;
            m_placing     = false;
            stop_references();
            stop_measure();
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Flip modifier"));
            ensure_rigid_instance();
            if (ModelVolume *flipped = volume_by_id(id)) {
                flip(*flipped);
                finish_change();
            }
        }
    }
    // cadenas : verrouillée, la forme quitte la sélection et l'outil en cours ; recliqué, elle redevient comme avant
    // (rien à retrancher : seul l'outil la traite autrement)
    if (m_pending_lock.has_value()) {
        const ObjectID id = *m_pending_lock;
        m_pending_lock.reset();
        if (ModelVolume *v = volume_by_id(id); v != nullptr && v->is_modifier_tool_volume()) {
            const bool           lock = !is_locked(*v);
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), lock ? _u8L("Lock modifier") : _u8L("Unlock modifier"));
            if (lock && v == current_volume()) {
                m_align       = AlignStep::None;
                m_handle_mode = HandleMode::None;
                stop_references();
                stop_measure();
                select_object();
            }
            ModifierPlacement placement = placement_of(*v);
            placement.locked            = lock;
            v->modifier_placement       = placement;
            m_parent.set_as_dirty();
        }
    }
    if (m_pending_join.has_value()) {
        const ObjectID     id      = *m_pending_join;
        const ModelVolume *current = current_volume();
        m_pending_join.reset();
        if (m_align != AlignStep::None && current != nullptr && current->id() == id)
            m_align = AlignStep::None; // recliqué : annulé
        else if (ModelVolume *v = volume_by_id(id); v != nullptr && v->is_modifier_tool_volume()) {
            if (v != current)
                select_volume(v);
            // l'outil en cours laisse la place
            stop_references();
            stop_measure();
            m_placing = false;
            m_align   = AlignStep::ShapePoint;
        }
        m_handle_mode = HandleMode::None; // (Pivoter / Déplacer éteints pendant « Joindre »)
        m_join_hover  = JointPick();
        m_join_lock   = JointPick();
        m_join_first  = JointPick();
        m_join_points.clear();
        m_parent.set_as_dirty();
    }
    if (m_pending_select_object) {
        m_pending_select_object = false;
        select_object();
    }
    if (m_pending_select.has_value()) {
        const ObjectID id = *m_pending_select;
        m_pending_select.reset();
        if (ModelVolume *v = volume_by_id(id))
            select_volume(v);
    }
    if (m_pending_select_range.has_value()) {
        const t_layer_height_range range = *m_pending_select_range;
        m_pending_select_range.reset();
        select_range(range);
    }
    if (m_pending_delete_volume.has_value()) {
        const ObjectID id = *m_pending_delete_volume;
        m_pending_delete_volume.reset();
        delete_volume(id);
    }
    if (m_pending_delete_range.has_value()) {
        const t_layer_height_range range = *m_pending_delete_range;
        m_pending_delete_range.reset();
        delete_range(range);
    }
    if (m_pending_references.has_value()) {
        const ObjectID     id      = *m_pending_references;
        const ModelVolume *current = current_volume();
        m_pending_references.reset();
        if (m_ref_step != RefStep::None && current != nullptr && current->id() == id)
            stop_references(); // recliqué : arrêtées
        else if (ModelVolume *v = volume_by_id(id); v != nullptr && v->is_modifier_tool_volume()) {
            if (v != current)
                select_volume(v);
            start_references();
        }
    }
    if (m_pending_dimension_open.has_value()) {
        const size_t index = *m_pending_dimension_open;
        m_pending_dimension_open.reset();
        open_dimension(index);
    }
    if (m_pending_ref_apply) {
        m_pending_ref_apply = false;
        apply_reference_value();
    }
    if (m_pending_ref_cancel) {
        m_pending_ref_cancel = false;
        close_value();
    }
    if (m_pending_ref_delete) {
        m_pending_ref_delete = false;
        if (m_ref_edit >= 0) {
            const size_t index = size_t(m_ref_edit);
            close_value();
            delete_dimension(index);
        }
    }
    if (m_pending_dimension_value.has_value()) {
        const auto [index, value] = *m_pending_dimension_value;
        m_pending_dimension_value.reset();
        set_dimension_value(index, value);
    }
    if (m_pending_dimension_delete.has_value()) {
        const size_t index = *m_pending_dimension_delete;
        m_pending_dimension_delete.reset();
        delete_dimension(index);
    }
    if (m_pending_dimension_rename.has_value()) {
        const auto [index, name] = *m_pending_dimension_rename;
        m_pending_dimension_rename.reset();
        rename_dimension(index, name);
    }
    if (m_pending_enforcer.has_value()) {
        const auto [id, key, value] = *m_pending_enforcer;
        m_pending_enforcer.reset();
        set_enforcer_setting(id, key, value);
    }
    if (m_pending_dimensions_reapply) {
        m_pending_dimensions_reapply = false;
        reapply_dimensions();
    }
    if (m_pending_handle_toggle.has_value()) {
        const auto [id, mode] = *m_pending_handle_toggle;
        m_pending_handle_toggle.reset();
        if (ModelVolume *v = volume_by_id(id)) {
            const bool active = m_handle_mode == mode && m_handle_volume == id;
            if (v != current_volume())
                select_volume(v);
            m_handle_mode   = active ? HandleMode::None : mode;
            m_handle_volume = id;
            if (m_handle_mode == HandleMode::Scale) // ses tailles du moment : 100 %
                m_scale_reference = volume_size(*v);
            m_placing     = false;
            m_align       = AlignStep::None;
            stop_references();
            stop_measure();
        }
    }
    if (m_pending_handle_field.has_value()) {
        const auto [axis, value] = *m_pending_handle_field;
        m_pending_handle_field.reset();
        apply_handle_field(axis, value);
    }
    if (m_pending_measure.has_value()) {
        const bool measure = *m_pending_measure;
        m_pending_measure.reset();
        if (measure)
            start_measure();
        else
            stop_measure();
    }
    if (m_pending_load) {
        m_pending_load = false;
        if (load_shape_file())
            m_shape = Shape::Loaded;
    }
    // bord d'une plage de hauteur modifié, quand plus aucun champ n'est actif
    if (m_range_buffer.has_value() && m_current_range.has_value() && !any_item_active) {
        const t_layer_height_range old_range = *m_current_range;
        const t_layer_height_range new_range = *m_range_buffer;
        m_range_buffer.reset();
        if (new_range != old_range) {
            // les plages peuvent se chevaucher, pas avoir les mêmes bords (la liste les confondrait)
            if (mo->layer_config_ranges.count(new_range) > 0)
                m_warning = _L("Another height range already has these bounds.");
            else if (new_range.second > new_range.first + 0.01) {
                wxGetApp().obj_list()->edit_layer_range(old_range, new_range, true);
                select_range(new_range);
            }
        }
    }
    // fin d'une modification au réglet ou au clavier : la scène et le tranchage suivent
    if (m_pending_finish && !any_item_active) {
        m_pending_finish = false;
        m_edit_snapshot  = false;
        finish_change();
    }
    if (m_pending_color && !any_item_active) { // une couleur choisie : un instantané par geste, rien à retrancher
        m_pending_color = false;
        m_edit_snapshot = false;
    }
    if (m_pending_tool >= 0) {
        const int tool = m_pending_tool;
        m_pending_tool = -1;
        m_parent.get_gizmos_manager().open_gizmo(GLGizmosManager::EType(tool));
    }
}

// Nature (menu déroulant ; seule la ligne survolée s'allume, la nature en cours se lit dans le champ) :
// Joindre / Couper / Modifier (comme l'outil Texte), Bloquer / Forcer les supports. Pour un volume
// sélectionné, elle change son type.
void GLGizmoModifiers::render_nature_input(ModelVolume *volume)
{
    const Nature   natures[] = {Nature::Join, Nature::Cut, Nature::Modifier, Nature::SupportBlocker, Nature::SupportEnforcer};
    const wxString labels[]  = {_L("Join"), _L("Cut"), _L("Modifier"), _L("Block supports"), _L("Enforce supports")};
    const bool     tool      = volume != nullptr && volume->is_modifier_tool_volume();
    const bool     last_part = tool && volume->is_model_part() && solid_parts_count() <= 1;
    // un intervalle ne se joint pas (sa tranche, plus large que la pièce, s'y ajouterait)
    const bool     interval  = tool ? volume->is_interval() : m_shape == Shape::Interval;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Nature"));
    ImGui::SameLine(m_label_width);
    ImGui::SetNextItemWidth(m_field_width);
    std::optional<Nature> clicked;
    int                   current = 0;
    for (int i = 0; i < 5; ++i)
        if (natures[i] == m_nature)
            current = i;
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    if (ImGui::BBLBeginCombo("##modifier_nature", into_u8(labels[current]).c_str(), 0)) {
        for (int i = 0; i < 5; ++i) {
            const bool disabled = (last_part && natures[i] != Nature::Join) || (interval && natures[i] == Nature::Join);
            if (ImGui::Selectable(into_u8(labels[i]).c_str(), false, disabled ? ImGuiSelectableFlags_Disabled : 0))
                clicked = natures[i];
        }
        ImGui::EndCombo();
    }
    ImGuiWrapper::pop_combo_style();

    if (!clicked.has_value() || *clicked == m_nature)
        return;
    if (tool)
        m_pending_nature = *clicked;
    else {
        if (m_current_range.has_value())
            m_pending_select_object = true;
        m_nature = *clicked;
    }
}

// Forme (première ligne) ; un intervalle posé garde la sienne (la ligne reste à sa place, grisée).
void GLGizmoModifiers::render_shape_input(ModelVolume *volume)
{
    const bool  tool    = volume != nullptr && volume->is_modifier_tool_volume();
    const Shape current = tool ? panel_shape(placement_of(*volume).shape) : m_shape;
    const bool  locked  = tool && volume->is_interval();

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Shape"));
    ImGui::SameLine(m_label_width);
    ImGui::SetNextItemWidth(m_field_width - m_top_row_reserve); // « Mesurer » au bout de la ligne
    std::optional<Shape> chosen;
    m_imgui->disabled_begin(locked);
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    const std::string preview = into_u8(shape_label(current));
    const float       image   = 1.6f * m_imgui->get_font_size(); // hauteur d'une ligne du menu et de son image
    if (ImGui::BBLBeginCombo("##modifier_shape", preview.c_str(), ImGuiComboFlags_HeightLargest)) {
        // lignes assez larges pour le nom le plus long et l'image (le menu s'élargit au besoin)
        float text = 0.f;
        for (const Shape shape : PANEL_SHAPES)
            text = std::max(text, m_imgui->calc_text_size(shape_label(shape)).x);
        const float width = std::max(text + ImGui::GetStyle().ItemSpacing.x + image, ImGui::GetContentRegionAvail().x);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
        for (const Shape shape : PANEL_SHAPES) {
            // un volume posé change de forme parmi les primitives seulement
            if (tool && !is_primitive_shape(shape))
                continue;
            const ImVec2 pos = ImGui::GetCursorScreenPos(); // (le rectangle de l'élément déborde de l'espacement)
            if (ImGui::Selectable(into_u8(shape_label(shape)).c_str(), false, 0, ImVec2(width, image)))
                chosen = shape;
            // son image, carrée, au bout de la ligne, comme dans l'ancien menu « Ajouter un modificateur »
            if (const ImTextureID texture = shape_image(shape))
                ImGui::GetWindowDrawList()->AddImage(texture, ImVec2(pos.x + width - image, pos.y), ImVec2(pos.x + width, pos.y + image));
            else if (shape == Shape::Interval) // ses deux liserés
                for (const float f : {0.3f, 0.7f})
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x + width - image, pos.y + f * image), ImVec2(pos.x + width, pos.y + f * image),
                                                        ImGui::GetColorU32(ImGuiCol_Text), 2.f);
            else if (shape == Shape::Face) { // une face, en perspective
                const float x = pos.x + width - image;
                ImGui::GetWindowDrawList()->AddQuadFilled(ImVec2(x + 0.3f * image, pos.y + 0.25f * image), ImVec2(x + image, pos.y + 0.25f * image),
                                                          ImVec2(x + 0.7f * image, pos.y + 0.75f * image), ImVec2(x, pos.y + 0.75f * image),
                                                          ImGui::GetColorU32(ImGuiCol_Text));
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }
    ImGuiWrapper::pop_combo_style();
    m_imgui->disabled_end();
    if (locked)
        return;

    if (chosen.has_value() && *chosen != current) {
        if (tool)
            m_pending_shape = *chosen;
        else if (*chosen == Shape::Loaded && !m_loaded_mesh)
            m_pending_load = true;
        else {
            m_shape = *chosen;
            // (un intervalle ne se joint pas ; une face sert d'abord de modificateur)
            if ((m_shape == Shape::Interval || m_shape == Shape::Face) && m_nature == Nature::Join)
                m_nature = Nature::Modifier;
            if (m_shape == Shape::Face) // la face se choisit aussitôt
                set_placing(true);
        }
    }
    // fichier chargé : son nom, et « Choisir… » pour un autre
    if (!tool && m_shape == Shape::Loaded) {
        ImGui::SetCursorPosX(m_label_width);
        m_imgui->text(m_loaded_mesh ? wxString::FromUTF8(m_loaded_name) : _L("No file"));
        ImGui::SameLine();
        if (m_imgui->button(_L("Choose...")))
            m_pending_load = true;
    }
}

// Côté de la forme (axe de son maillage) le long de chaque axe du plateau (X, Y, Z) : celui qui en est le plus proche, sans
// qu'un côté serve deux fois (la meilleure des 6 permutations ; à égalité, l'ordre de la forme).
static std::array<int, 3> sides_along_plate(const Matrix3d &rotation)
{
    std::array<int, 3> best{{0, 1, 2}}, permutation{{0, 1, 2}};
    double             best_score = -1.;
    do {
        double score = 0.;
        for (int axis = 0; axis < 3; ++axis)
            score += std::abs(rotation(axis, permutation[size_t(axis)]));
        if (score > best_score + 1e-9) {
            best_score = score;
            best       = permutation;
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    return best;
}

// Cotes de la forme sélectionnée (sous sa ligne du tableau) : diamètre et hauteur pour les formes rondes,
// longueur × largeur × hauteur sinon (avec « Garder les proportions » pour les formes qui ne sont ni des
// cubes ni faites sur une face). Longueur, largeur et hauteur sont celles du plateau (X, Y, Z absolus) : chacune règle le côté de la forme
// le plus proche de cet axe, même tournée (posée sur une paroi, jointe à une face penchée…) ; l'axe d'une forme
// ronde est nommé de même. Une taille que ses cotes (références) fixent est grisée.
void GLGizmoModifiers::render_size_input(ModelVolume &volume)
{
    const Shape shape = placement_of(volume).shape;
    const Vec3d size  = volume_size(volume);
    double      hi    = 10.;
    if (const ModelObject *mo = model_object(); mo != nullptr && instance_index() >= 0)
        hi = std::max(hi, 2. * mo->instance_bounding_box(size_t(instance_index())).size().maxCoeff());

    Vec3d      new_size   = size;
    const bool free_ratio = shape == Shape::Cube || shape == Shape::Face || shape == Shape::Sphere || is_round_shape(shape);
    std::array<bool, 3> set_sizes{{false, false, false}};
    if (has_dimensions(volume)) {
        Transform3d unused;
        solve_dimensions(volume, volume.modifier_placement->dimensions, unused, nullptr, &set_sizes);
    }
    const bool any_set = set_sizes[0] || set_sizes[1] || set_sizes[2];
    // « Symétrie » au bout de chaque ligne, pour ses axes
    const float button  = ImGui::GetFrameHeight();
    const float reserve = button + ImGui::GetStyle().ItemSpacing.x;
    // axe 0 (X), 1 (Y), 2 (Z) ; -1 = X et Y ensemble ; 3 = les trois
    auto row = [&](const wxString &label, const std::string &id, int axis) {
        double     value = axis == 2 ? size.z() : (axis == 1 ? size.y() : size.x());
        // (une forme qui change de taille en entier : grisée si l'un de ses côtés est fixé)
        const bool set   = axis == 3 || (!free_ratio && m_keep_ratio) ? any_set : axis == -1 ? set_sizes[0] || set_sizes[1] : set_sizes[size_t(axis)];
        m_imgui->disabled_begin(set);
        const bool changed = slider_row(label, id, value, 0.1, hi, "%.2f mm", reserve) && !set;
        m_imgui->disabled_end();
        if (set && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            m_imgui->tooltip(_L("Its dimensions set this value: change or delete one of them."), m_label_width + m_field_width);
        const int  first   = axis < 0 || axis > 2 ? 0 : axis;
        ImGui::SameLine(m_label_width + m_field_width - button);
        ImGui::PushID(id.c_str());
        if (icon_button(m_symmetric_icon, m_symmetric_icon_dark, "modifier_symmetric", button, m_symmetric[first])) {
            const bool on = !m_symmetric[first];
            for (int i = 0; i < 3; ++i)
                if (i == axis || axis == 3 || (axis == -1 && i < 2))
                    m_symmetric[i] = on;
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(m_symmetric[first] ? _L("Symmetric: this dimension changes on both sides of the center of the shape. Click: on one side only.") :
                                                  _L("Symmetric: this dimension changes on both sides of the center of the shape (now: on one side only)."),
                             m_label_width + m_field_width);
        if (!changed)
            return;
        value = std::max(value, 0.1);
        if (!free_ratio && m_keep_ratio) {
            const double old_value = axis == 2 ? size.z() : (axis == 1 ? size.y() : size.x());
            if (old_value > 1e-9)
                new_size = size * (value / old_value);
            return;
        }
        if (axis == 3)
            new_size = Vec3d::Constant(value);
        else if (axis == -1)
            new_size.x() = new_size.y() = value;
        else
            new_size(axis) = value;
    };
    const wxString           plate_labels[3] = {_L("Length"), _L("Width"), _L("Height")};
    const std::array<int, 3> along           = sides_along_plate(decompose(volume_world_matrix(volume), volume.mesh().bounding_box()).rotation);
    // forme ronde étirée par Échelle : longueur, largeur et hauteur (un diamètre la referait ronde)
    const double tolerance = 1e-3 * size.maxCoeff();
    const bool   stretched = ((shape == Shape::Sphere || is_round_shape(shape)) && std::abs(size.y() - size.x()) > tolerance) ||
                           (shape == Shape::Sphere && std::abs(size.z() - size.x()) > tolerance);
    switch (stretched ? Shape::Cube : shape) {
    case Shape::Sphere: row(_L("Diameter"), "size_d", 3); break;
    case Shape::Cylinder:
    case Shape::Disc: // un disque déjà posé : un cylindre
    case Shape::Cone:
        row(_L("Diameter"), "size_d", -1);
        row(plate_labels[std::find(along.begin(), along.end(), 2) - along.begin()], "size_h", 2); // (son axe : couché, une longueur)
        break;
    case Shape::Torus:
        row(_L("Outer diameter"), "size_d", -1);
        row(_L("Shape thickness"), "size_h", 2);
        break;
    default:
        for (int axis = 0; axis < 3; ++axis)
            row(plate_labels[axis], std::string("size_") + "xyz"[axis], along[size_t(axis)]);
        if (!free_ratio) {
            ImGui::SetCursorPosX(m_label_width);
            m_imgui->bbl_checkbox(_L("Keep proportions"), m_keep_ratio);
        }
        break;
    }
    if (new_size.isApprox(size, 1e-9))
        return;
    begin_edit(_u8L("Resize modifier"));
    apply_size(volume, new_size);
}

// Forceur de supports sélectionné (sous ses cotes) : les supports qu'il génère et leurs styles, réglages du volume
// (support_enforcer_kind, support_classic_style, support_style) comme dans sa page Supports ; sans réglage propre, ceux de
// l'objet ; le style de l'autre sorte de supports est grisé.
void GLGizmoModifiers::render_enforcer_input(ModelVolume &volume)
{
    const ModelObject *mo = model_object();
    if (mo == nullptr)
        return;
    auto value_of = [&](const char *key) {
        const ConfigOption *option = volume.config.get().option(key);
        if (option == nullptr)
            option = mo->config.get().option(key);
        if (option == nullptr)
            option = wxGetApp().preset_bundle->prints.get_edited_preset().config.option(key);
        return option != nullptr ? option->getInt() : 0;
    };
    const bool tree = volume.generates_tree_supports(SupportType(value_of("support_type")));
    auto       row  = [&](const char *key, bool enabled) {
        const ConfigOptionDef *def = print_config_def.get(key);
        if (def == nullptr || def->enum_keys_map == nullptr)
            return;
        const int current = value_of(key);
        int       shown   = 0;
        for (size_t i = 0; i < def->enum_values.size(); ++i)
            if (def->enum_keys_map->count(def->enum_values[i]) > 0 && def->enum_keys_map->at(def->enum_values[i]) == current)
                shown = int(i);
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L(def->label.c_str()));
        ImGui::SameLine(m_label_width);
        ImGui::SetNextItemWidth(m_field_width);
        m_imgui->disabled_begin(!enabled);
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        const std::string id = std::string("##enforcer_") + key;
        if (ImGui::BBLBeginCombo(id.c_str(), size_t(shown) < def->enum_labels.size() ? into_u8(_L(def->enum_labels[size_t(shown)].c_str())).c_str() : "", 0)) {
            for (size_t i = 0; i < def->enum_labels.size() && i < def->enum_values.size(); ++i)
                if (ImGui::Selectable(into_u8(_L(def->enum_labels[i].c_str())).c_str(), false) && int(i) != shown)
                    m_pending_enforcer = std::make_tuple(volume.id(), std::string(key), def->enum_keys_map->at(def->enum_values[i]));
            ImGui::EndCombo();
        }
        ImGuiWrapper::pop_combo_style();
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !def->tooltip.empty())
            m_imgui->tooltip(_L(def->tooltip.c_str()), m_label_width + m_field_width);
    };
    row("support_enforcer_kind", true);
    row("support_classic_style", !tree);
    row("support_style", tree);
}

// Réglage de supports d'un forceur, choisi dans la fenêtre : écrit dans ses réglages (« Type de supports de l'objet » :
// retiré), puis la liste d'objets, sa page de réglages et le tranchage suivent, comme depuis sa page Supports.
void GLGizmoModifiers::set_enforcer_setting(const ObjectID &volume_id, const std::string &key, int value)
{
    ModelVolume *volume = volume_by_id(volume_id);
    if (volume == nullptr || !volume->is_support_enforcer())
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Change support enforcer settings"));
    if (key == "support_enforcer_kind") {
        if (value == int(sekAuto))
            volume->config.erase(key);
        else
            volume->config.set_key_value(key, new ConfigOptionEnum<SupportEnforcerKind>(SupportEnforcerKind(value)));
    } else
        volume->config.set_key_value(key, new ConfigOptionEnum<SupportMaterialStyle>(SupportMaterialStyle(value)));
    wxGetApp().obj_list()->object_config_options_changed({volume->get_object(), volume});
    wxGetApp().params_panel()->notify_object_config_changed();
    if (auto *tab = dynamic_cast<TabPrintModel *>(wxGetApp().get_model_tab(true)); tab != nullptr && tab->has_model_config())
        tab->update_model_config();
    m_parent.set_as_dirty();
}

// « Placer » marche ou s'arrête ; l'outil en cours (références, Pivoter, Déplacer, Mesurer) laisse la place.
void GLGizmoModifiers::set_placing(bool placing)
{
    m_placing = placing;
    m_align   = AlignStep::None;
    m_hover   = SurfacePoint();
    if (m_placing) {
        stop_references();
        m_handle_mode = HandleMode::None;
        if (m_measuring)
            m_pending_measure = false;
    }
    m_parent.set_as_dirty();
}

// Au-dessus du tableau : le grand bouton à bascule « Placer », au milieu (allumé, vert, il devient « Terminer » :
// la forme suit la souris sur la pièce et un clic la pose ; éteint, rien ne suit la souris ; l'aide est dans son
// infobulle),
// dessous « Magnétisme des ancrages », centré sur sa ligne, puis l'ancrage où la forme s'aimante, sur une
// ligne toujours gardée (la fenêtre ne saute pas).
void GLGizmoModifiers::render_place_input(const ModelVolume *volume)
{
    const ImGuiStyle &style      = ImGui::GetStyle();
    const float       wrap_width = m_label_width + m_field_width;
    ImGui::Separator();

    const ImVec2 button(8.f * m_imgui->get_font_size(), 2.f * ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 0.5f * (wrap_width - button.x));
    const bool active = m_placing;
    if (active)
        ImGuiWrapper::push_confirm_button_style();
    const bool bold    = m_imgui->push_bold_font();
    const bool clicked = m_imgui->button((active ? _L("Finish") : _L("Place")) + "###place", button.x, button.y); // (le libellé dit ce que fera le clic)
    if (bold)
        m_imgui->pop_bold_font();
    if (active)
        ImGuiWrapper::pop_confirm_button_style();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(!active                                    ? _L("The shape follows the mouse over the part and a click places it.") :
                         m_shape == Shape::Interval                 ? _L("Click on the part at the height where the interval begins.") :
                         m_shape == Shape::Svg && volume == nullptr ? _L("Click on the part: the SVG tool asks for the file and opens for its settings.") :
                         m_shape == Shape::Face ? _L("Click on a flat face of the part: a volume of its shape is created, 2 mm thick, into the part "
                                                     "(out of it for \"Join\"). A modifier is 3 mm wider than the face, so that a bridge keeps "
                                                     "its anchors. Esc: stop placing.") :
                                                  _L("Click on the part to place. Shift + click: place over another one. Esc: stop placing."),
                         wrap_width);
    if (clicked)
        set_placing(!m_placing);

    const wxString label = _L("Anchor magnetism");
    const float    box   = 0.78f * ImGui::GetFrameHeight() + style.ItemInnerSpacing.x; // (la case de BBLCheckbox)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, 0.5f * (wrap_width - box - m_imgui->calc_text_size(label).x)));
    m_imgui->bbl_checkbox(label + "###anchor_magnetism", m_magnetism);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("The shape snaps to the remarkable points of the part near the mouse: centers of circles and holes, centroid and center "
                            "of a face, farthest point from the edges, vertices, middles of edges, subdivisions of the medians and diagonals. "
                            "Elsewhere it stays free. Unchecked: the shape goes exactly under the mouse and stays straight, along the axes of the "
                            "plate, whatever the slope of the surface."),
                         wrap_width);

    // l'ancrage où la forme s'aimante, centré dessous (raccourci : la fenêtre ne s'élargit pas au gré des
    // noms) ; ligne vide sinon
    if ((m_placing || m_drag.has_value()) && m_hover.valid && m_hover.snapped && (m_drag.has_value() || m_hover_tool < 0)) {
        wxString name = m_hover.anchor_name;
        if (m_imgui->calc_text_size(name).x > wrap_width) {
            while (name.length() > 1 && m_imgui->calc_text_size(name + "...").x > wrap_width)
                name.RemoveLast();
            name += "...";
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 0.5f * (wrap_width - m_imgui->calc_text_size(name).x));
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, name);
    } else
        ImGui::NewLine();
}

// Petit bouton à icône (resources/images/<name>.svg, <name>_dark.svg) ; vert quand il est actif.
bool GLGizmoModifiers::icon_button(ImTextureID &light, ImTextureID &dark, const std::string &name, float size, bool active)
{
    ImTextureID &icon = m_is_dark_mode ? dark : light;
    if (icon == nullptr) {
        const std::string file = Slic3r::resources_dir() + "/images/" + name + (m_is_dark_mode ? "_dark.svg" : ".svg");
        const unsigned    px   = unsigned(std::max(16.f, size * 2.f));
        if (!IMTexture::load_from_svg_file(file, px, px, icon))
            icon = nullptr;
    }
    if (active)
        ImGuiWrapper::push_confirm_button_style();
    const float padding = std::max(1.f, 0.12f * size);
    ImGui::PushID(name.c_str());
    const bool clicked = icon != nullptr ? m_imgui->image_button(icon, ImVec2(size - 2.f * padding, size - 2.f * padding), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), int(padding)) :
                                           ImGui::Button("?", ImVec2(size, size));
    ImGui::PopID();
    if (active)
        ImGuiWrapper::pop_confirm_button_style();
    return clicked;
}

// Bouton « Mesurer » (icône de la Mesure de la barre ; vert quand elle est allumée).
void GLGizmoModifiers::render_measure_button(float size)
{
    ImTextureID &icon = m_is_dark_mode ? m_measure_icon_dark : m_measure_icon;
    if (icon == nullptr) {
        const std::string file = Slic3r::resources_dir() + (m_is_dark_mode ? "/images/toolbar_measure_dark.svg" : "/images/toolbar_measure.svg");
        const unsigned    px   = unsigned(std::max(16.f, size * 2.f));
        if (!IMTexture::load_from_svg_file(file, px, px, icon))
            icon = nullptr;
    }
    const bool active = m_measuring;
    if (active)
        ImGuiWrapper::push_confirm_button_style();
    const float padding = std::max(1.f, 0.12f * size);
    bool        clicked = false;
    if (icon != nullptr)
        clicked = m_imgui->image_button(icon, ImVec2(size - 2.f * padding, size - 2.f * padding), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), int(padding));
    else
        clicked = m_imgui->button(_L("Measure"));
    if (active)
        ImGuiWrapper::pop_confirm_button_style();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(active ? _L("Leave the measure (Esc).") :
                                  _L("Measure without leaving the tool: click two features of the part or of its shapes. Nothing is moved or scaled."),
                         m_label_width + m_field_width);
    if (clicked)
        m_pending_measure = !m_measuring;
}

// Résultats de « Mesurer » : éléments choisis, distances, angle (comme la Mesure de la barre).
void GLGizmoModifiers::render_measure_panel()
{
    if (!m_measure)
        return;
    ImGui::Separator();
    m_measure->render_panel();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_GREY_LIGHT);
    m_imgui->text_wrapped(_L("Click two features (Shift + click: a point). Del: start again. Esc: undo the last choice, then leave the measure."),
                          m_label_width + m_field_width);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// Sous la ligne de la forme sélectionnée, dans son liseré : l'étape en cours de « Joindre » ou des
// références (quoi cliquer, ce qui est survolé), l'erreur d'une référence refusée.
void GLGizmoModifiers::render_tool_steps()
{
    const float wrap_width = m_label_width + m_field_width;
    if (m_align != AlignStep::None) {
        const wxString step = m_join_lock.valid ? _L("Face selected: click one of its points, or the face again to join the face itself.") :
                              m_align == AlignStep::ShapePoint ?
                                                  _L("Click a point or a face of the shape (Shift: also its vertices and its center).") :
                                                  _L("Now click a point or a face of the part or of another shape (Shift: also the vertices and the centers of the volumes).");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_ORANGE_LIGHT);
        m_imgui->text_wrapped(step, wrap_width);
        ImGui::PopStyleColor();
        if (m_join_hover.valid)
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, "> " + m_join_hover.name);
    }
    if (m_ref_step != RefStep::None) {
        const ModelVolume *volume = current_volume();
        const wxString     step   = m_ref_step == RefStep::VolumeRef ?
                                        (volume != nullptr && volume->is_interval() ? _L("Click the outline of the bottom or of the top of the interval.") :
                                                                                      _L("Click a reference of the shape: a face, an edge or the center of a circle (Shift + click: a point).")) :
                              m_ref_step == RefStep::PartRef   ? (m_ref_first.kind == RefKind::Point ? _L("Now click a reference of the part: an edge gives the distance perpendicular to it. Shift + click: a point, which snaps in line with the first one along X, Y or Z (edges stay available).") :
                                                                                                         _L("Now click a reference of the part.")) :
                                                                 _L("Type the distance: Enter applies it, Esc cancels.");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_ORANGE_LIGHT);
        m_imgui->text_wrapped(step, wrap_width);
        ImGui::PopStyleColor();
        // référence survolée
        if (m_ref_hover.valid && (m_ref_step == RefStep::VolumeRef || m_ref_step == RefStep::PartRef))
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, "> " + m_ref_hover.name);
        if (!m_ref_error.IsEmpty() && m_ref_step != RefStep::Value)
            m_imgui->warning_text_wrapped(m_ref_error, wrap_width);
    } else if (!m_ref_error.IsEmpty())
        m_imgui->warning_text_wrapped(m_ref_error, wrap_width);
}

// « Références » de la forme sélectionnée : menu replié sous sa ligne du tableau ; nom (un clic : le renommer),
// valeur modifiable, croix ; en rouge celles qui ne sont plus respectées (et le titre du menu), « Réappliquer ».
void GLGizmoModifiers::render_dimension_list(const ModelVolume &volume, float row_x)
{
    const std::vector<ModifierDimension> &dimensions = volume.modifier_placement->dimensions;
    const float                           wrap_width = m_label_width + m_field_width;
    bool                                  broken     = false;
    for (const ModifierDimension &dimension : dimensions)
        broken |= dimension_error_mm(volume, dimension) >= 0.01;
    const std::string header = into_u8(_L("References")) + " (" + std::to_string(dimensions.size()) + ")###dimensions";
    if (broken)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_RED);
    const bool open = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_None);
    if (broken)
        ImGui::PopStyleColor();
    if (!open)
        return;
    m_dimension_buffers.resize(dimensions.size(), 0.);
    const float cross   = ImGui::GetTextLineHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    // la valeur (« 123.45 mm ») au bout de la ligne, avant la croix ; le nom prend le reste
    const float value_width = 7.f * m_imgui->get_font_size();
    const float value_x     = row_x + wrap_width - cross - spacing - value_width;
    for (size_t i = 0; i < dimensions.size(); ++i) {
        const ModifierDimension &dimension = dimensions[i];
        const bool               kept      = dimension_error_mm(volume, dimension) < 0.01;
        ImGui::PushID(int(20000 + i));
        // son nom : un champ discret (comme du texte, légèrement surligné au survol) qui se modifie d'un clic ; Entrée ou un
        // clic ailleurs le garde, Échap l'annule ; vidé ou remis à ses références, il redevient « Face - Arête ». Il va
        // jusqu'à la valeur, sans passer dessous (le menu est en retrait).
        const std::string default_name = into_u8(kind_label(dimension.volume_kind) + " - " + kind_label(dimension.part_kind));
        const bool        editing      = ImGui::GetActiveID() == ImGui::GetID("##dimension_name");
        char              shown[sizeof(m_dimension_name)];
        std::snprintf(shown, sizeof(shown), "%s", dimension.name.empty() ? default_name.c_str() : dimension.name.c_str());
        // (le fond d'un champ ImGui ne suit pas le survol : il se lit avant de le dessiner)
        const float  name_width = std::max(2.f * cross, value_x - ImGui::GetCursorPosX() - spacing);
        const ImVec2 name_pos   = ImGui::GetCursorScreenPos();
        const bool   over       = !editing && ImGui::IsWindowHovered() &&
                                  ImGui::IsMouseHoveringRect(name_pos, ImVec2(name_pos.x + name_width, name_pos.y + ImGui::GetFrameHeight()));
        ImVec4       highlight  = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        highlight.w             = 0.12f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, editing ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBg) : over ? highlight : ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Text, kept ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGuiWrapper::COL_RED);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::SetNextItemWidth(name_width);
        ImGui::InputText("##dimension_name", editing ? m_dimension_name : shown, sizeof(m_dimension_name), ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemActivated()) // (le texte saisi va dans m_dimension_name dès l'image suivante)
            std::snprintf(m_dimension_name, sizeof(m_dimension_name), "%s", shown);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            std::string  name  = m_dimension_name;
            const size_t first = name.find_first_not_of(' ');
            name               = first == std::string::npos ? std::string() : name.substr(first, name.find_last_not_of(' ') - first + 1);
            m_pending_dimension_rename = std::make_pair(i, name == default_name ? std::string() : name);
        }
        if (!editing && ImGui::IsItemHovered())
            m_imgui->tooltip(wxString::FromUTF8(shown) + "\n" + _L("Click: rename this dimension."), wrap_width);
        ImGui::SameLine(value_x);
        ImGui::SetNextItemWidth(value_width);
        // la valeur suit la cote, sauf pendant la saisie
        if (ImGui::GetActiveID() != ImGui::GetID("##dimension_value"))
            m_dimension_buffers[i] = dimension_target_mm(dimension);
        double value = m_dimension_buffers[i];
        ImGui::InputDouble("##dimension_value", &value, 0.0, 0.0, "%.2f mm");
        m_dimension_buffers[i] = std::max(0., value);
        if (ImGui::IsItemDeactivatedAfterEdit())
            m_pending_dimension_value = std::make_pair(i, m_dimension_buffers[i]);
        ImGui::SameLine(row_x + wrap_width - cross);
        if (cross_button("##dimension_delete", true, cross))
            m_pending_dimension_delete = i;
        ImGui::PopID();
    }
    if (broken) {
        m_imgui->warning_text_wrapped(_L("The dimensions in red are no longer kept (part cut or changed, object scaled unevenly...)."), wrap_width);
        if (m_imgui->button(_L("Reapply")))
            m_pending_dimensions_reapply = true;
    }
    ImGui::TreePop();
}

// Point d'une cote où mettre son étiquette ou son champ (monde) : le milieu de la cote.
// (« pm » : son bout sur la forme, dimension_end)
static Vec3d dimension_middle(const Transform3d &instance, const Vec3d &pm, const ModifierDimension &dimension)
{
    if (dimension.coincident)
        return pm;
    const Vec3d pp   = instance * dimension.part_point;
    const Vec3d dir  = (instance.linear() * dimension.direction).normalized();
    const Vec3d foot = pm - dir * dir.dot(pm - pp);
    return 0.5 * (foot + pm);
}

// Étiquettes des cotes de la forme sélectionnée, sur elles en 3D ; un clic ouvre le champ. Pendant
// qu'on tourne, déplace ou redimensionne la forme avec ses poignées : l'angle, la distance ou la taille
// (et son % d'Échelle), près de la souris.
void GLGizmoModifiers::render_dimension_labels()
{
    if (m_handle_drag.has_value() && m_handle_drag->moved) {
        char text[64];
        if (m_handle_drag->ring)
            std::snprintf(text, sizeof(text), u8"%.1f°", m_handle_drag->value);
        else if (m_handle_drag->scale)
            std::snprintf(text, sizeof(text), "%.2f mm (%.1f %%)", m_handle_drag->value,
                          100. * m_handle_drag->value / std::max(m_scale_reference(m_handle_drag->side), 1e-6));
        else
            std::snprintf(text, sizeof(text), "%.2f mm", m_handle_drag->value);
        const Vec2d mouse = m_parent.get_local_mouse_position();
        m_imgui->set_next_window_pos(float(mouse.x()) + 18.f, float(mouse.y()) + 18.f, ImGuiCond_Always, 0.f, 0.f);
        m_imgui->set_next_window_bg_alpha(0.8f);
        m_imgui->begin(std::string("##modifier_handle_value"), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
        m_imgui->text(wxString::FromUTF8(text));
        m_imgui->end();
    }

    const ModelVolume *volume = current_volume();
    if (volume == nullptr || !has_dimensions(*volume) || m_measuring)
        return;
    const Camera             &camera   = wxGetApp().plater()->get_camera();
    const Matrix4d            pv       = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4> &viewport = camera.get_viewport();
    const Transform3d         instance = instance_matrix();
    const auto               &dims     = volume->modifier_placement->dimensions;
    for (size_t i = 0; i < dims.size(); ++i) {
        if (m_ref_step == RefStep::Value && m_ref_edit == int(i))
            continue; // son champ est ouvert
        const Vec3d middle = dimension_middle(instance, dimension_end(*volume, dims[i]), dims[i]);
        const Vec2d ss     = TransformHelper::world_to_ss(middle, pv, viewport);
        if ((middle - camera.get_position()).dot(camera.get_dir_forward()) <= 0. || !ss.allFinite() || ss.cwiseAbs().maxCoeff() > 1e5)
            continue; // derrière la caméra (vue de très près) : une fenêtre ImGui à l'infini se dessinerait mal
        const bool  kept = dimension_error_mm(*volume, dims[i]) < 0.01;
        char        text[64];
        std::snprintf(text, sizeof(text), "%.2f mm", dimension_target_mm(dims[i]));
        m_imgui->set_next_window_pos(float(ss.x()), float(viewport[3] - ss.y()), ImGuiCond_Always, 0.5f, 0.5f);
        m_imgui->set_next_window_bg_alpha(0.8f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 2.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        m_imgui->begin("##modifier_dimension_label_" + std::to_string(i), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                                                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                                                         ImGuiWindowFlags_NoFocusOnAppearing);
        if (kept)
            m_imgui->text(wxString::FromUTF8(text));
        else
            ImGuiWrapper::text_colored(ImGuiWrapper::COL_RED, wxString::FromUTF8(text));
        if (ImGui::IsWindowHovered()) {
            m_imgui->tooltip(dimension_label(dims[i]) + "\n" + _L("Click to change it (Del in its field: delete it)."), 20.f * m_imgui->get_font_size());
            if (ImGui::IsMouseClicked(0))
                m_pending_dimension_open = i;
        }
        m_imgui->end();
        ImGui::PopStyleVar(3);
    }
}

// Champ de la distance, près de la cote : nouvelle cote (deux points : le long de X, Y, Z de la
// pièce ou en ligne droite) ou cote cliquée. Entrée applique, Échap annule.
void GLGizmoModifiers::render_dimension_popup()
{
    if (m_ref_step != RefStep::Value)
        return;
    ModelVolume *volume = current_volume();
    if (volume == nullptr)
        return;
    const bool  editing = m_ref_edit >= 0 && has_dimensions(*volume) && m_ref_edit < int(volume->modifier_placement->dimensions.size());
    if (!editing && (!m_ref_first.valid || !m_ref_second.valid))
        return;
    const Camera             &camera   = wxGetApp().plater()->get_camera();
    const Matrix4d            pv       = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4> &viewport = camera.get_viewport();
    const Vec3d where = editing ? dimension_middle(instance_matrix(), dimension_end(*volume, volume->modifier_placement->dimensions[m_ref_edit]),
                                                   volume->modifier_placement->dimensions[m_ref_edit]) :
                                  Vec3d(0.5 * (m_ref_first.point + m_ref_second.point));
    Vec2d       ss    = TransformHelper::world_to_ss(where, pv, viewport);
    const float font  = m_imgui->get_font_size();
    if ((where - camera.get_position()).dot(camera.get_dir_forward()) <= 0. || !ss.allFinite() || ss.cwiseAbs().maxCoeff() > 1e5)
        ss = Vec2d(0.5 * viewport[2], 0.5 * viewport[3]); // cote derrière la caméra : champ au milieu de la vue

    m_imgui->set_next_window_pos(float(ss.x()) + font, float(viewport[3] - ss.y()), ImGuiCond_Always, 0.f, 0.5f);
    m_imgui->begin(std::string("##modifier_dimension_input"), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                                                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    m_imgui->text(editing ? dimension_label(volume->modifier_placement->dimensions[m_ref_edit]) : m_ref_first.name + " - " + m_ref_second.name);
    // deux points : la direction de la cote
    if (!editing && m_ref_first.kind == RefKind::Point && m_ref_second.kind == RefKind::Point) {
        const wxString labels[] = {"X", "Y", "Z", _L("In a straight line")};
        ImGuiWrapper::push_radio_style();
        for (int i = 0; i < 4; ++i) {
            if (i > 0)
                ImGui::SameLine();
            if (ImGui::RadioButton(into_u8(labels[i]).c_str(), m_ref_along == i) && m_ref_along != i) {
                m_ref_along = i;
                ModifierDimension probe;
                wxString          error;
                double            current = 0.;
                if (make_dimension(*volume, m_ref_first, m_ref_second, m_ref_along, 0., probe, error, &current))
                    m_ref_value = current;
                m_ref_focus = true;
            }
        }
        ImGuiWrapper::pop_radio_style();
    }
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Distance"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(6.f * font);
    if (m_ref_focus) {
        ImGui::SetKeyboardFocusHere();
        m_ref_focus = false;
    }
    double value = m_ref_value;
    if (ImGui::InputDouble("##modifier_dimension_value", &value, 0.0, 0.0, "%.2f", ImGuiInputTextFlags_AutoSelectAll))
        m_ref_value = std::max(0., value);
    ImGui::SameLine();
    m_imgui->text("mm");
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool enter   = focused && (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Enter)) || ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_KeyPadEnter)));
    const bool escape  = focused && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape));
    ImGuiWrapper::push_confirm_button_style();
    if (m_imgui->button(_L("Apply")) || enter)
        m_pending_ref_apply = true;
    ImGuiWrapper::pop_confirm_button_style();
    ImGui::SameLine();
    if (m_imgui->button(_L("Cancel")) || escape)
        m_pending_ref_cancel = true;
    if (editing) {
        ImGui::SameLine();
        if (m_imgui->button(_L("Delete")))
            m_pending_ref_delete = true;
    }
    if (!m_ref_error.IsEmpty())
        m_imgui->warning_text_wrapped(m_ref_error, 18.f * font);
    m_imgui->end();
}

// Bas et haut d'un intervalle (sous sa ligne du tableau) : distances le long de son axe depuis le point le plus
// bas de la pièce (horizontal : les bornes de sa plage de hauteur).
void GLGizmoModifiers::render_interval_input(ModelVolume &volume)
{
    const IntervalPlanes planes = interval_planes(volume);
    const Vec3d          n      = planes.frame.col(2);
    double               low = 0., high = 0.;
    parts_extent(n, low, high);
    const double   old_bottom = n.dot(planes.origin) - low;
    const double   old_top    = old_bottom + planes.thickness;
    const double   hi         = std::max(high - low, old_top);
    const wxString tip        = _L("Distance from the lowest point of the part, along the axis of the interval (horizontal interval: its height).");
    double         bottom     = old_bottom;
    double         top        = old_top;
    // un plan qu'une cote fixe : grisé
    std::array<bool, 2> set{{false, false}};
    if (has_dimensions(volume))
        for (const ModifierDimension &dimension : volume.modifier_placement->dimensions)
            set[interval_target(volume, dimension).offset > 0. ? 1 : 0] = true;
    auto row = [&](const wxString &label, const std::string &id, double &value, bool fixed) {
        m_imgui->disabled_begin(fixed);
        const bool changed = slider_row(label, id, value, 0., hi, "%.2f mm") && !fixed;
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            m_imgui->tooltip(fixed ? _L("Its dimensions set this value: change or delete one of them.") : tip, m_label_width + m_field_width);
        return changed;
    };
    bool edited = row(_L("Interval bottom"), "interval_bottom", bottom, set[0]);
    edited |= row(_L("Interval top"), "interval_top", top, set[1]);
    if (!edited)
        return;
    if (bottom != old_bottom && top <= bottom + 0.01)
        top = bottom + (old_top - old_bottom); // le bas passe au-dessus du haut : l'intervalle garde son épaisseur
    top = std::max(top, bottom + 0.01);
    begin_edit(_u8L("Resize modifier"));
    set_interval_planes(volume, low + bottom, top - bottom);
}

// Plage de hauteur sélectionnée (sous sa ligne du tableau) : ses bords.
void GLGizmoModifiers::render_range_input()
{
    const ModelObject *mo = model_object();
    if (mo == nullptr || instance_index() < 0 || !m_current_range.has_value())
        return;
    const double         height = mo->instance_bounding_box(size_t(instance_index())).size().z();
    t_layer_height_range range  = m_range_buffer.value_or(*m_current_range);
    bool                 edited = slider_row(_L("Range bottom"), "range_bottom", range.first, 0., height, "%.2f mm");
    edited |= slider_row(_L("Range top"), "range_top", range.second, 0., height, "%.2f mm");
    if (edited) {
        range.first  = std::clamp(range.first, 0., height);
        range.second = std::clamp(range.second, 0., height);
        m_range_buffer = range;
    }
    // chevauchement permis : le tranchage garde, dans la partie commune, la plage qui commence
    // le plus bas (PrintApply / Slicing : chaque plage est rognée par les précédentes)
    for (const auto &other : mo->layer_config_ranges)
        if (other.first != *m_current_range && range.first < other.first.second - EPSILON && range.second > other.first.first + EPSILON) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_GREY_LIGHT);
            m_imgui->text_wrapped(_L("This range overlaps another one: where they overlap, the range that starts lowest applies."),
                                  m_label_width + m_field_width);
            ImGui::PopStyleColor();
            break;
        }
}

// Petite croix au bout d'une ligne du tableau : supprime la ligne (rouge au survol, grisée sinon).
bool GLGizmoModifiers::cross_button(const char *id, bool enabled, float size)
{
    const ImVec2 pos     = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton(id, ImVec2(size, size)) && enabled;
    const bool   hovered = enabled && ImGui::IsItemHovered();
    const ImU32  color   = !enabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled) :
                           hovered  ? ImGui::GetColorU32(ImVec4(0.95f, 0.3f, 0.3f, 1.f)) :
                                      ImGui::GetColorU32(ImGuiCol_Text);
    const float  margin  = 0.28f * size;
    ImDrawList  *draw    = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(pos.x + margin, pos.y + margin), ImVec2(pos.x + size - margin, pos.y + size - margin), color, 1.5f);
    draw->AddLine(ImVec2(pos.x + size - margin, pos.y + margin), ImVec2(pos.x + margin, pos.y + size - margin), color, 1.5f);
    if (hovered)
        m_imgui->tooltip(_L("Delete"), m_label_width + m_field_width);
    return clicked;
}

// Réglages modifiés d'un volume ou d'une plage, en clair (« Densité du remplissage 40% · … »). Le
// filament 0 (celui de l'objet) et l'épaisseur de couche par défaut d'une plage ne comptent pas.
std::string GLGizmoModifiers::settings_summary(const ModelConfig &config, bool layer_range) const
{
    const DynamicPrintConfig &cfg = config.get();
    double                    object_layer_height = 0.;
    if (layer_range) {
        const ModelObject *mo = model_object();
        if (mo != nullptr && mo->config.has("layer_height"))
            object_layer_height = mo->config.opt_float("layer_height");
        else if (const ConfigOption *opt = wxGetApp().preset_bundle->prints.get_edited_preset().config.option("layer_height"))
            object_layer_height = opt->getFloat();
    }
    std::string summary;
    for (const std::string &key : cfg.keys()) {
        const ConfigOption *opt = cfg.option(key);
        if (opt == nullptr)
            continue;
        if (key == "extruder" && opt->getInt() == 0)
            continue;
        if (layer_range && key == "layer_height" && std::abs(opt->getFloat() - object_layer_height) < EPSILON)
            continue;
        const ConfigOptionDef *def = print_config_def.get(key);
        if (def == nullptr)
            continue;
        std::string value;
        if (def->type == coBool)
            value = opt->getBool() ? _u8L("Yes") : _u8L("No");
        else if (def->type == coEnum) {
            value = opt->serialize();
            for (size_t i = 0; i < def->enum_values.size(); ++i)
                if (def->enum_values[i] == value && i < def->enum_labels.size()) {
                    value = _u8L(def->enum_labels[i].c_str());
                    break;
                }
        } else {
            value = opt->serialize();
            if (!def->sidetext.empty() && value.find('%') == std::string::npos)
                value += " " + _u8L(def->sidetext.c_str());
        }
        if (!summary.empty())
            summary += "  ·  ";
        summary += (def->label.empty() ? key : _u8L(def->label.c_str())) + " " + value;
    }
    return summary;
}

// Tableau des modificateurs de l'objet : bouton de sélection (recliqué : la ligne se désélectionne ; un
// clic sur le nom aussi), nature (pastille), nom comme dans la liste « Objets ». Au bout, les outils de la
// forme : Inverser, Joindre, Références, Pivoter, Déplacer (anneaux ou flèches sur la forme, champs X / Y / Z
// après la croix), et la croix qui la supprime ; les boutons ont la hauteur de la ligne. La ligne
// sélectionnée est encadrée d'un liseré bleu sur toute sa hauteur : sous son nom, l'étape de Joindre ou des
// références, ses cotes (réglets) ; puis, comme pour les autres, ses réglages modifiés ; enfin ses
// références.
void GLGizmoModifiers::render_table()
{
    ModelObject *mo = model_object();
    if (mo == nullptr)
        return;
    const ModelVolume *current    = current_volume();
    const float        font       = m_imgui->get_font_size();
    const float        wrap_width = m_label_width + m_field_width;

    ImGui::Separator();
    m_imgui->text(_L("Modifiers of the object"));
    const float row_x      = ImGui::GetCursorPosX();
    const float row_h      = ImGui::GetFrameHeight();
    const float spacing    = ImGui::GetStyle().ItemSpacing.x;
    const float pastille   = 0.8f * font;
    const float name_x     = row_h + spacing + pastille + spacing; // retrait du nom (et du résumé) dans la ligne
    const bool  last_part  = solid_parts_count() <= 1;
    int         rows       = 0;
    // k-ième bouton depuis le bout de la ligne (0 : la croix)
    auto at_end = [&](int k) { ImGui::SameLine(row_x + wrap_width - float(k + 1) * row_h - float(k) * spacing); };
    // pastille de couleur, centrée sur la hauteur de la ligne ; « clickable » (un modificateur) : un clic choisit sa couleur
    auto draw_pastille = [&](const ColorRGBA &color, bool clickable) {
        const ImVec2 pos     = ImGui::GetCursorScreenPos();
        bool         clicked = false;
        if (clickable) {
            clicked = ImGui::InvisibleButton("##pastille", ImVec2(pastille, row_h));
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_L("Click: choose the color of this modifier."), wrap_width);
        } else
            ImGui::Dummy(ImVec2(pastille, row_h));
        const float y = pos.y + 0.5f * (row_h - pastille);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + pastille, y + pastille),
                                                  ImGui::GetColorU32(ImVec4(color.r(), color.g(), color.b(), 1.f)), 2.f);
        ImGui::SameLine();
        return clicked;
    };
    // bouton de sélection (bleu, comme le liseré, quand la ligne l'est)
    auto select_button = [&](bool selected) {
        if (selected)
            for (const ImGuiCol col : {ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive, ImGuiCol_Border})
                ImGui::PushStyleColor(col, ImGuiWrapper::COL_BLUE_LIGHT);
        const bool clicked = icon_button(m_select_icon, m_select_icon_dark, "modifier_select", row_h, false);
        if (selected)
            ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(selected ? _L("Selected (framed in blue). Click: deselect.") : _L("Select this line."), wrap_width);
        ImGui::SameLine();
        return clicked;
    };
    // le nom, cliquable lui aussi, sans surlignage (la sélection se voit au liseré), jusqu'aux « buttons »
    // boutons du bout de la ligne
    auto selectable = [&](const char *name, int buttons) {
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
        for (const ImGuiCol col : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive})
            ImGui::PushStyleColor(col, ImVec4(0.f, 0.f, 0.f, 0.f));
        const float width   = wrap_width - name_x - float(buttons) * (row_h + spacing);
        const bool  clicked = ImGui::Selectable(name, false, ImGuiSelectableFlags_NoPadWithHalfSpacing, ImVec2(width, row_h));
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        return clicked;
    };
    auto summary_line = [&](const std::string &summary, bool warn) {
        ImGui::Indent(name_x);
        if (!summary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_GREY_LIGHT);
            m_imgui->text_wrapped(summary, wrap_width - name_x);
            ImGui::PopStyleColor();
        } else if (warn)
            m_imgui->warning_text_wrapped(_L("No setting changed: no effect yet."), wrap_width - name_x);
        ImGui::Unindent(name_x);
    };
    // liseré bleu de la ligne sélectionnée : de son haut (« top », écran) jusqu'à ce qui vient d'être affiché,
    // dans l'espace entre les lignes
    auto frame = [&](const ImVec2 &top, float right) {
        const ImGuiStyle &style  = ImGui::GetStyle();
        const ImVec2      pad(0.5f * style.ItemSpacing.x, 0.5f * style.ItemSpacing.y);
        const float       bottom = ImGui::GetCursorScreenPos().y - style.ItemSpacing.y;
        ImGui::GetWindowDrawList()->AddRect(ImVec2(top.x - pad.x, top.y - pad.y), ImVec2(right + pad.x, bottom + pad.y),
                                            ImGui::GetColorU32(ImGuiWrapper::COL_BLUE_LIGHT), style.FrameRounding, 0, std::max(1.f, 0.1f * font));
    };

    for (size_t i = 0; i < mo->volumes.size(); ++i) {
        ModelVolume *volume = mo->volumes[i];
        if (!is_tool_listed(*volume))
            continue;
        ++rows;
        ImGui::PushID(int(i));
        const bool   selected = volume == current;
        const bool   tool     = volume->is_modifier_tool_volume(); // (un texte ou un SVG a son outil)
        const bool   locked   = is_locked(*volume); // (cadenas : sa ligne ne la sélectionne plus, ses outils sont grisés)
        const ImVec2 top      = ImGui::GetCursorScreenPos();
        float        right    = top.x + wrap_width;
        m_imgui->disabled_begin(locked);
        bool clicked = select_button(selected) && !locked;
        m_imgui->disabled_end();
        if (locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            m_imgui->tooltip(_L("Locked: click its padlock to unlock it."), wrap_width);
        if (draw_pastille(volume_color(*volume, 1.f), volume->is_modifier())) {
            const ColorRGBA color = volume_color(*volume, 1.f);
            m_color_rgb           = {color.r(), color.g(), color.b()};
            ImGui::OpenPopup("##modifier_color");
        }
        clicked |= selectable(volume->name.c_str(), tool ? 8 : 1) && !locked;
        if (clicked) {
            // recliquée : la ligne se désélectionne
            if (!selected)
                m_pending_select = volume->id();
            else
                m_pending_select_object = true;
            // (la Mesure ne garderait que la ligne choisie, la pièce disparaîtrait : on la quitte)
            if (m_measuring)
                m_pending_measure = false;
        }
        // outils de la forme : un clic sur l'un quitte celui en cours (Joindre, Références, Pivoter, Déplacer,
        // Placer, Mesurer)
        const bool handles_on = selected && volume->id() == m_handle_volume && m_handle_mode != HandleMode::None;
        const bool interval   = tool && volume->is_interval(); // (une tranche : ni Inverser ni Joindre)
        if (tool) {
            at_end(7);
            if (icon_button(locked ? m_lock_icon : m_unlock_icon, locked ? m_lock_icon_dark : m_unlock_icon_dark, locked ? "modifier_lock" : "modifier_unlock", row_h,
                            locked))
                m_pending_lock = volume->id();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(locked ? _L("Locked: it can no longer be clicked on the part nor selected here, and its tools are grayed. Click: unlock.") :
                                          _L("Lock this shape: it can no longer be clicked on the part nor selected here, and its tools are grayed. "
                                             "Click again: unlock."),
                                 wrap_width);
        }
        m_imgui->disabled_begin(locked);
        if (tool && !interval) {
            at_end(6);
            if (icon_button(m_flip_icon, m_flip_icon_dark, "modifier_flip", row_h, false))
                m_pending_flip = volume->id();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_L("Passes the shape to the other side of the plane of its anchor point: into the part or out of it."), wrap_width);

            const bool joining = selected && m_align != AlignStep::None;
            at_end(5);
            if (icon_button(m_join_icon, m_join_icon_dark, "modifier_join", row_h, joining))
                m_pending_join = volume->id();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(joining ? _L("Joining: click again (or Esc) to cancel.") :
                                           _L("Like the Joint tool of Fusion 360: click a point of the shape, then a point of the part or of another shape: the shape moves "
                                              "until they meet, without turning (into the plane of the face, when a face is chosen). Points: vertices, middles of edges, centers "
                                              "of faces, of polygons and of circles; Shift: also every vertex and the center of each volume. A click on a face away from its points "
                                              "selects it: its points are then joined along this face; a second click on it joins the face itself. Two faces joined become "
                                              "parallel: the shape turns as little as possible."),
                                 wrap_width);
        }
        if (tool) {
            const bool referencing = selected && m_ref_step != RefStep::None;
            at_end(4);
            if (icon_button(m_references_icon, m_references_icon_dark, "modifier_references", row_h, referencing))
                m_pending_references = volume->id();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(referencing ? _L("References: click again (or Esc) to stop.") :
                                 interval    ? _L("Dimensions which fix the plane of the interval: click the outline of its bottom or of its top, then a "
                                                  "reference of the part, and type the distance; the plane passes there, fixed in space. A dimension on its bottom "
                                                  "and one on its top set its thickness; two dimensions apart on the same plane, at different distances, tilt it.") :
                                               _L("Dimensions which fix the place of the shape: click a reference of the shape (face, edge, center of a circle; "
                                                  "Shift + click: a point), then one of the part, and type the distance. Several dimensions can be set: they move the shape, "
                                                  "and resize it when moving it is not enough (two dimensions on opposite sides set its length); it then slides only "
                                                  "along the directions they leave free."),
                                 wrap_width);

            at_end(3);
            if (icon_button(m_rotate_icon, m_rotate_icon_dark, "toolbar_rotate", row_h, m_handle_mode == HandleMode::Rotate && handles_on))
                m_pending_handle_toggle = std::make_pair(volume->id(), HandleMode::Rotate);
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(interval ? _L("Rotate this interval with rings around its center of gravity, that of the part between its two planes; tilted, "
                                               "its layer height no longer applies. Click again or Esc: stop.") :
                                            _L("Rotate this shape with rings: steps of 45° inside a ring, of 5° on the ring and its graduations, of 1° beyond (snapping every "
                                               "15°). Angles can also be typed in the fields after the cross. Click again or Esc: stop."),
                                 wrap_width);
            at_end(2);
            if (icon_button(m_move_icon, m_move_icon_dark, "toolbar_move", row_h, m_handle_mode == HandleMode::Move && handles_on))
                m_pending_handle_toggle = std::make_pair(volume->id(), HandleMode::Move);
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(interval ? _L("Move this interval with arrows along X, Y and Z of the plate; its position can also be typed in the fields after "
                                               "the cross. Its slice always covers the whole part: only a move across its planes changes it. Click again or Esc: "
                                               "stop.") :
                                            _L("Move this shape with arrows, like the Move tool; its position can also be typed in the fields after the cross. Click "
                                               "again or Esc: stop."),
                                 wrap_width);
        }
        m_imgui->disabled_end();
        if (tool) {
            at_end(1);
            m_imgui->disabled_begin(interval || locked); // (sa tranche couvre toute la pièce)
            if (icon_button(m_scale_icon, m_scale_icon_dark, "toolbar_scale", row_h, m_handle_mode == HandleMode::Scale && handles_on))
                m_pending_handle_toggle = std::make_pair(volume->id(), HandleMode::Scale);
            m_imgui->disabled_end();
            if (!locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                m_imgui->tooltip(interval ? _L("An interval always covers the whole part: set its bottom and its top under its name.") :
                                            _L("Resize this shape by dragging the cubes at the end of its sides, or in percent or in mm in the fields after the "
                                               "cross, along X, Y and Z of the plate; uniform scale unchecked: along one axis only (the side of the shape closest "
                                               "to it, even turned). Gray cube: its dimensions set this size. Click again or Esc: stop."),
                                 wrap_width);
        }
        at_end(0);
        // la dernière pièce pleine de l'objet ne se supprime pas, ni une forme verrouillée
        if (cross_button("##delete", !(volume->is_model_part() && last_part) && !locked, row_h))
            m_pending_delete_volume = volume->id();
        std::optional<float> fields_bottom; // Échelle : bas de ses champs, à droite des lignes du dessous
        if (handles_on && tool && m_handle_mode == HandleMode::Scale) {
            const ImVec2 below = ImGui::GetCursorScreenPos();
            ImGui::SameLine();
            ImGui::BeginGroup();
            render_scale_fields(*volume);
            ImGui::EndGroup();
            right         = std::max(right, ImGui::GetItemRectMax().x);
            fields_bottom = ImGui::GetItemRectMax().y;
            ImGui::SetCursorScreenPos(below);
        } else if (handles_on && tool) {
            render_handle_fields(*volume);
            right = std::max(right, ImGui::GetItemRectMax().x);
        }
        // ligne sélectionnée : sous son nom, l'étape de Joindre ou des références, puis ses cotes
        if (selected && tool) {
            render_tool_steps();
            if (interval)
                render_interval_input(*volume);
            else
                render_size_input(*volume);
            if (volume->is_support_enforcer()) // (ses supports générés et leurs styles)
                render_enforcer_input(*volume);
            // elle ne touche plus la pièce : sans effet
            if (!volume->is_model_part()) {
                BoundingBoxf3 parts_box;
                for (const ModelVolume *v : mo->volumes)
                    if (v->is_model_part() && !v->is_modifier_tool_volume())
                        parts_box.merge(v->mesh().transformed_bounding_box(volume_world_matrix(*v)));
                const BoundingBoxf3 box = volume->mesh().transformed_bounding_box(volume_world_matrix(*volume));
                if (parts_box.defined && !parts_box.intersects(box))
                    m_imgui->warning_text_wrapped(_L("This shape does not touch the part: it has no effect."), wrap_width);
            }
        }
        summary_line(settings_summary(volume->config, false), volume->is_modifier());
        if (selected && has_dimensions(*volume))
            render_dimension_list(*volume, row_x);
        if (const float y = ImGui::GetCursorScreenPos().y; fields_bottom && y - ImGui::GetStyle().ItemSpacing.y < *fields_bottom) // (liseré et ligne suivante sous les champs)
            ImGui::Dummy(ImVec2(0.f, std::max(0.f, *fields_bottom - y)));
        if (selected)
            frame(top, right);
        // couleur du modificateur, choisie dans la petite fenêtre ouverte par sa pastille (un instantané par geste)
        if (ImGui::BeginPopup("##modifier_color")) {
            ImGui::SetNextItemWidth(12.f * font);
            if (ImGui::ColorPicker3("##modifier_color_picker", m_color_rgb.data(), ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs)) {
                if (!m_edit_snapshot) {
                    wxGetApp().plater()->take_snapshot(_u8L("Change modifier color"));
                    m_edit_snapshot = true;
                }
                m_pending_color                = true;
                ModifierPlacement placement    = placement_of(*volume);
                placement.color                = encode_color(ColorRGB(m_color_rgb[0], m_color_rgb[1], m_color_rgb[2]));
                volume->modifier_placement     = placement;
                m_parent.set_as_dirty();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    int range_idx = 0;
    for (const auto &[range, config] : mo->layer_config_ranges) {
        ++rows;
        ImGui::PushID(10000 + range_idx++);
        const ImVec2 top      = ImGui::GetCursorScreenPos();
        const bool   selected = m_current_range.has_value() && *m_current_range == range;
        bool         clicked  = select_button(selected);
        draw_pastille(MODIFIER_RANGE_COLOR, false); // (une plage : sa couleur ne se choisit pas)
        char bounds[64];
        std::snprintf(bounds, sizeof(bounds), "%.2f - %.2f", range.first, range.second);
        const std::string name = into_u8(format_wxstr(_L("Height range %1% mm"), bounds));
        clicked |= selectable(name.c_str(), 1);
        if (clicked) {
            if (!selected)
                m_pending_select_range = range;
            else
                m_pending_select_object = true; // recliquée : désélectionnée
            if (m_measuring)
                m_pending_measure = false;
        }
        at_end(0);
        if (cross_button("##delete", true, row_h))
            m_pending_delete_range = range;
        if (selected)
            render_range_input(); // ses bords, sous son nom
        summary_line(settings_summary(config, true), true);
        if (selected)
            frame(top, top.x + wrap_width);
        ImGui::PopID();
    }
    if (rows == 0)
        ImGuiWrapper::text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("None yet."));
}

} // namespace GUI
} // namespace Slic3r
