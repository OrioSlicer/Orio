#ifndef slic3r_PrepareCut_hpp_
#define slic3r_PrepareCut_hpp_

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "GLModel.hpp"

#include <array>
#include <functional>
#include <set>
#include <utility>

class wxMouseEvent;

namespace Slic3r {

class GLShaderProgram;
class GLVolume;
class GLVolumeCollection;

namespace GUI {

class IMSlider;
struct Camera;
class PartPlate;

// Orio : outil de coupe X/Y/Z de l'onglet Preparation, avec le meme curseur que celui de l'Apercu
// (lettre d'axe, fleche de remise a zero). Les trois axes coupent geometriquement (mm). La coupe ne
// touche que les pieces du plateau courant ; les shaders gouraud (objets), silhouette (contour de
// selection) et flat_clip (selection a la souris) rejettent ce qui sort de la boite gardee.
// Faces de coupe pleines et hachurees : render_caps(), avec le pochoir (stencil) du tampon d'image, pour chaque
// piece coupee puis chaque modificateur (et bloqueur / forceur de supports, volume negatif) coupe, dont la section
// est dessinee juste devant, hachures dans l'autre sens. Le point de coupe vu a travers un pixel est l'entree du
// rayon de vue dans la boite gardee ; il est dans le volume si le compte des faces du volume au-dela (+1 par face
// arriere, -1 par face avant) n'est pas nul : les faces de la boite gardee y sont dessinees.
// Sans pochoir : faces arriere vues par l'ouverture, peintes en face de coupe par gouraud (ce qui suffit avec
// un seul plan, mais laisse des trous quand le rayon ressort de la boite par un autre plan de coupe).
class PrepareCut
{
public:
    PrepareCut();
    ~PrepareCut();
    PrepareCut(const PrepareCut&) = delete;
    PrepareCut& operator=(const PrepareCut&) = delete;

    // A chaque image, avant tout rendu des objets : etendue des pieces du plateau courant (curseurs
    // reconstruits quand elle change, coupe memorisee reprise) et boite de coupe. enabled = outil
    // disponible (onglet Preparation, ni gizmo ni outil de hauteur ouvert) ; sinon ni curseur ni coupe.
    void update(const GLVolumeCollection& volumes, PartPlate* plate, bool enabled);
    // Curseur de l'axe affiche, lettre d'axe et fleche de remise a zero, a droite du canevas, dans la
    // bande verticale qui commence a band_top (sous la barre d'outils). Renvoie vrai si la coupe a
    // change : un nouveau rendu est a demander.
    bool render(int canvas_width, int canvas_height, float band_top, float scale, bool is_dark);
    // Une coupe est appliquee aux pieces
    bool is_active() const { return m_active; }
    // Uniforms de coupe du shader courant (gouraud, silhouette ou flat_clip). Sans coupe active, la
    // desactive : ces shaders servent aussi aux autres canevas et a d'autres rendus.
    // camera : pour placer les hachures des faces de coupe (gouraud), peut etre nulle sinon.
    void apply_uniforms(GLShaderProgram& shader, const Camera* camera) const;
    // Desactive la coupe dans le shader courant, apres usage
    static void disable_uniforms(GLShaderProgram& shader);

    // Faces de coupe du shader courant (u_clip_cap_mode de gouraud, flat_clip et silhouette) : rendu normal,
    // comptage des faces d'un volume, face de coupe d'une piece ou section d'un modificateur
    enum class CapMode { Parts = 0, Count = 1, Cap = 2, Section = 3 };
    static void set_cap_mode(GLShaderProgram& shader, CapMode mode);
    // Faces de coupe par le pochoir possibles : coupe active et tampon d'image courant avec un pochoir
    bool can_render_caps() const;
    // Faces de coupe des volumes du plateau courant que la coupe traverse (pieces puis modificateurs), apres le
    // rendu des pieces ; shader : le shader courant (gouraud pour la vue, flat_clip pour la selection a la souris,
    // silhouette pour le contour de selection) ; camera : celle de ce rendu.
    //  filter(volume) : volumes concernes (les selectionnes pour le contour...) ;
    //  draw(volume, indice dans volumes) : dessine ce volume (comptage) ;
    //  box(volume, indice, matrice) : pose les matrices du shader pour la boite gardee de ce volume (cube unite x
    //  matrice -> monde) et renvoie sa couleur : render_caps la dessine ensuite (GLModel, qui pose uniform_color).
    void render_caps(GLShaderProgram& shader, const Camera& camera, const GLVolumeCollection& volumes,
                     const std::function<bool(const GLVolume&)>& filter,
                     const std::function<void(GLVolume&, size_t)>& draw,
                     const std::function<std::array<float, 4>(const GLVolume&, size_t, const Transform3d&)>& box) const;
    // Molette au-dessus du curseur affiche : le deplace (~1 mm par cran) ; vrai s'il a bouge
    bool on_mouse_wheel(wxMouseEvent& evt);
    // Fleche de remise a zero : les trois axes en pleine plage, memoire effacee
    void reset();

private:
    void rebuild_sliders(const BoundingBoxf3& extent, const Vec3d& origin);
    // memorise (en coordonnees du plateau) la coupe de l'axe que l'utilisateur vient de deplacer
    void remember(int axis);
    double label_offset(int axis) const { return axis < 2 ? m_origin(axis) : 0.0; }
    // volume d'une instance du plateau courant (coupee)
    bool on_cut_plate(const GLVolume& volume) const;
    // boite gardee (monde), les bornes qui ne coupent pas ramenees a la zone coupee
    BoundingBoxf3 cap_box() const;

    std::array<IMSlider*, 3> m_sliders{ { nullptr, nullptr, nullptr } };
    // axe affiche : 0 = X, 1 = Y, 2 = Z (Z par defaut : couper par le haut est le plus courant)
    int  m_axis{ 2 };
    bool m_visible{ false };
    bool m_active{ false };
    bool m_is_dark{ false };
    // etendue des pieces du plateau courant (monde) et origine de ce plateau
    BoundingBoxf3 m_extent;
    Vec3d m_origin{ Vec3d::Zero() };
    // boite gardee (monde) ; une borne qui ne coupe pas est repoussee a -1e5 / +1e5
    std::array<float, 3> m_box_min{ { -1.0e5f, -1.0e5f, -1.0e5f } };
    std::array<float, 3> m_box_max{ { 1.0e5f, 1.0e5f, 1.0e5f } };
    // zone du plateau courant (xmin, ymin, xmax, ymax) : seule partie de la scene coupee ; en Z, etendue de ce
    // qui y est coupe (pieces et modificateurs)
    std::array<float, 4> m_region{ { 0.0f, 0.0f, 0.0f, 0.0f } };
    std::array<float, 2> m_region_z{ { 0.0f, 0.0f } };
    // instances (objet, instance) du plateau courant
    std::set<std::pair<int, int>> m_plate_instances;
    // cube unite : boite gardee de render_caps()
    mutable GLModel m_cap_box;
    // derniere coupe choisie a la main, par axe (borne basse, haute), en coordonnees du plateau, et
    // bornes qui coupaient : reprise quand l'etendue change (piece deplacee, autre plateau...)
    std::array<std::array<double, 2>, 3> m_memory_mm{ { { { 0.0, 0.0 } }, { { 0.0, 0.0 } }, { { 0.0, 0.0 } } } };
    std::array<std::array<bool, 2>, 3>   m_memory_active{ { { { false, false } }, { { false, false } }, { { false, false } } } };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PrepareCut_hpp_
