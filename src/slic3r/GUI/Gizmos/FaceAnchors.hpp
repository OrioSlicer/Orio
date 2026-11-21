#ifndef slic3r_FaceAnchors_hpp_
#define slic3r_FaceAnchors_hpp_

// Orio : ancrages d'une face plane (outil « Modificateurs »). Mêmes calculs que les ancrages
// de la face coupée des connecteurs de l'outil Coupe (GLGizmoAdvancedCut::update_sections), repris
// ici sans toucher à la Coupe : îlots de la face, repère propre de chacun (barycentre ; longueur et
// largeur = côtés du plus petit rectangle englobant), point le plus éloigné des bords, centres des
// trous, foyers d'une ellipse, puis les ancrages (points remarquables, subdivisions des médianes et
// des diagonales, grille des quarts / tiers / moitiés).

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"

#include <wx/string.h>

#include <vector>

namespace Slic3r {
namespace GUI {
namespace FaceAnchors {

// Un îlot de la face, dans le repère 2D de la face, en mm.
struct Island
{
    ExPolygon                       expoly;            // à l'échelle (scale_), pour Clipper
    std::vector<std::vector<Vec2d>> rings;             // contour puis trous, en mm
    double                          area{0.0};
    Vec2d                           centroid{Vec2d::Zero()};
    Vec2d                           axis_u{Vec2d::UnitX()}; // longueur
    Vec2d                           axis_v{Vec2d::UnitY()}; // largeur
    Vec2d                           box_min{Vec2d::Zero()}; // rectangle englobant, en (u, v) depuis le barycentre
    Vec2d                           box_max{Vec2d::Zero()};
    Vec2d                           pole{Vec2d::Zero()};    // point le plus éloigné des bords
    double                          pole_radius{0.0};
    std::vector<Vec2d>              hole_centers;
};

// Les premiers (barycentre … foyers) passent avant les subdivisions pour l'aimantation.
enum class AnchorKind : unsigned char { Centroid, Pole, BoxCenter, HoleCenter, Focus, Fraction, LengthMedian, WidthMedian, Diagonal };

struct Anchor
{
    Vec2d      pos{Vec2d::Zero()};
    AnchorKind kind{AnchorKind::Fraction};
    int        island{-1};
    Vec2i      frac_a{0, 1}; // fraction : de la longueur, ou le long de la médiane / diagonale
    Vec2i      frac_b{0, 1}; // Fraction : de la largeur
};

struct Face
{
    std::vector<Island> islands; // le plus grand d'abord
    std::vector<Anchor> anchors;
    bool                empty() const { return islands.empty(); }
};

// Îlots et ancrages d'une face (ExPolygons à l'échelle, dans le repère 2D de la face).
Face compute(const ExPolygons &expolys);

// Îlot contenant le point (mm), ou -1.
int island_at(const Face &face, const Vec2d &point);

// Distance signée d'un point au bord d'un îlot (positive dans la matière).
double edge_distance(const Face &face, int island, const Vec2d &point);

// Ancrage le plus proche du point à « tolerance » près (mm) : points remarquables d'abord, puis
// subdivisions ; -1 s'il n'y en a pas.
int snap(const Face &face, const Vec2d &point, double tolerance);

// Nom de l'ancrage, affiché au survol.
wxString anchor_name(const Anchor &anchor);

} // namespace FaceAnchors
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_FaceAnchors_hpp_
