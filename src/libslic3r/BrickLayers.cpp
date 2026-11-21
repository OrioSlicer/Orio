// Orio : « Couches en briques » (brick layers).
//
// Une paroi interne sur deux (numéros impairs : 1, 3, 5...) est imprimée une demi-couche plus haut que les autres,
// pour que les parois s'emboîtent comme des briques et que la pièce résiste mieux entre les couches (brevet expiré
// US 5,653,925, FIG. 2 ; règles de base reprises du script Bricklayers de Tenger Technologies).
//
// Cette passe s'exécute à la fin de PrintObject::make_perimeters(), quand les parois de toutes les couches existent :
//  1. Candidates : dans chaque couche à partir de la 2e (la 1re reste plate), les parois internes impaires des
//     régions où l'option est cochée, sans partie en surplomb.
//  2. Hauteur de buse : une candidate monte d'une demi-couche (ExtrusionPath::z_shift) si des candidates de la couche
//     du dessus la recouvrent ; sinon elle reste au Z de la couche (« chapeau ») pour que le dessus reste plat.
//  3. Débit : chaque morceau de paroi prend la hauteur réelle entre le dessus de ce qui est dessous et la buse :
//     150 % pour une paroi montée posée sur du plat, 100 % sur une paroi montée, 50 % pour un chapeau (ou une paroi
//     normale) posé sur une paroi montée. Les parois décalées reçoivent en plus le débit brick_layers_flow.
// La génération du G-code imprime ensuite les parois montées en dernier sur chaque objet, à print_z + z_shift.
// Le remplissage continue l'alternance (PrintObject::apply_brick_infill, à la fin de l'étape du remplissage).

// clipper_z.hpp doit être inclus avant clipper.hpp (inclus par ClipperUtils.hpp, Print.hpp...).
#include "ClipperZUtils.hpp"
#include "Print.hpp"
#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace {

// Au-delà de cette longueur non recouverte par des parois décalées du dessus, une candidate ne monte pas.
constexpr double MAX_UNCOVERED_RATIO = 0.05;
constexpr double MAX_UNCOVERED_MM    = 3.;
// Longueur à l'air libre (rien au-dessus) tolérée pour une candidate qui monte : au-delà, elle dépasserait du dessus.
constexpr double MAX_EXPOSED_MM      = 0.5;
// Un changement de débit sur moins de cette longueur n'en vaut pas la peine : le morceau suit son voisin.
constexpr double MIN_PIECE_MM        = 0.5;

struct BrickWall
{
    ExtrusionEntity *entity { nullptr };
    ExtrusionPaths  *paths  { nullptr };
    // Paroi interne impaire, susceptible d'être décalée.
    bool             candidate { false };
    // Débit des parois décalées de sa région (1.05 pour 105 %).
    double           flow_ratio { 1. };
};

struct BrickLayer
{
    // Toutes les parois des régions où l'option est cochée.
    std::vector<BrickWall>   walls;
    // Empreinte (largeur d'extrusion) des candidates, et de celles qui montent finalement.
    ExPolygons               candidates;
    std::vector<BoundingBox> candidates_bboxes;
    ExPolygons               raised;
    std::vector<BoundingBox> raised_bboxes;
    // Hauteur de buse des parois montées de cette couche, au-dessus de son print_z (mm).
    double                   shift { 0. };
};

// Les chemins d'une paroi : boucle, ou ligne ouverte d'Arachne.
ExtrusionPaths* wall_paths(ExtrusionEntity *entity)
{
    if (auto *loop = dynamic_cast<ExtrusionLoop*>(entity))
        return &loop->paths;
    if (auto *multi_path = dynamic_cast<ExtrusionMultiPath*>(entity))
        return &multi_path->paths;
    return nullptr;
}

bool is_candidate(const ExtrusionPaths &paths)
{
    if (paths.empty())
        return false;
    const int wall_index = paths.front().wall_index;
    if (wall_index < 1 || wall_index % 2 == 0)
        return false;
    for (const ExtrusionPath &path : paths)
        if (path.role() != erPerimeter || path.overhang_degree > 0. || path.is_force_no_extrusion() || path.wall_index != wall_index)
            return false;
    return true;
}

// Seuls les morceaux de paroi ordinaires (ni pont, ni surplomb) voient leur débit adapté.
bool is_adjustable(const ExtrusionPath &path)
{
    return (path.role() == erPerimeter || path.role() == erExternalPerimeter) && !path.is_force_no_extrusion() && path.height > 0.f;
}

std::vector<BoundingBox> bboxes_of(const ExPolygons &expolygons)
{
    std::vector<BoundingBox> out;
    out.reserve(expolygons.size());
    for (const ExPolygon &expolygon : expolygons)
        out.emplace_back(get_extents(expolygon));
    return out;
}

// Les ExPolygons dont la boîte englobante touche bbox.
ExPolygons select_by_bbox(const ExPolygons &src, const std::vector<BoundingBox> &bboxes, const BoundingBox &bbox)
{
    ExPolygons out;
    for (size_t i = 0; i < src.size(); ++i)
        if ((i < bboxes.size() ? bboxes[i] : get_extents(src[i])).overlap(bbox))
            out.emplace_back(src[i]);
    return out;
}

// Morceau consécutif d'un chemin, posé ou non sur une paroi montée de la couche du dessous.
struct Piece
{
    Polyline polyline;
    bool     on_raised { false };
};

// Découpe un chemin là où ce qui est dessous change (paroi montée ou non), avec une précision de MIN_PIECE_MM :
// l'état de chaque pas est celui de son milieu. Les morceaux se suivent et partagent leurs points de jonction.
std::vector<Piece> split_by_area(const Polyline &polyline, const ExPolygons &area, const std::vector<BoundingBox> &bboxes)
{
    auto on_raised = [&area, &bboxes](const Point &pt) {
        for (size_t i = 0; i < area.size(); ++i)
            if (bboxes[i].contains(pt) && area[i].contains(pt))
                return true;
        return false;
    };
    const double       step = scale_(MIN_PIECE_MM);
    std::vector<Piece> pieces;
    for (size_t i = 1; i < polyline.points.size(); ++i) {
        const Point &a      = polyline.points[i - 1];
        const Point &b      = polyline.points[i];
        const int    nsteps = std::max(1, int(std::ceil((b - a).cast<double>().norm() / step)));
        for (int k = 0; k < nsteps; ++k) {
            const bool state = on_raised(lerp(a, b, (k + 0.5) / nsteps));
            if (pieces.empty()) {
                pieces.push_back({ Polyline(), state });
                pieces.back().polyline.points.push_back(a);
            } else if (pieces.back().on_raised != state) {
                const Point start = k == 0 ? a : lerp(a, b, double(k) / nsteps);
                if (pieces.back().polyline.points.back() != start)
                    pieces.back().polyline.points.push_back(start);
                pieces.push_back({ Polyline(), state });
                pieces.back().polyline.points.push_back(start);
            }
        }
        if (!pieces.empty() && pieces.back().polyline.points.back() != b)
            pieces.back().polyline.points.push_back(b);
    }
    return pieces;
}

// Les morceaux trop courts prennent l'état de leur voisin, puis les morceaux voisins de même état sont réunis.
void merge_short_pieces(std::vector<Piece> &pieces, const double min_length)
{
    if (pieces.size() > 1)
        for (size_t i = 0; i < pieces.size(); ++i)
            if (pieces[i].polyline.points.size() < 2 || pieces[i].polyline.length() < min_length)
                pieces[i].on_raised = i > 0 ? pieces[i - 1].on_raised : pieces[i + 1].on_raised;

    std::vector<Piece> merged;
    merged.reserve(pieces.size());
    for (Piece &piece : pieces) {
        if (piece.polyline.points.empty())
            continue;
        if (!merged.empty() && merged.back().on_raised == piece.on_raised) {
            // Deux morceaux consécutifs partagent leur point de jonction.
            Points &dst = merged.back().polyline.points;
            auto    it  = piece.polyline.points.begin();
            if (!dst.empty() && dst.back() == *it)
                ++it;
            dst.insert(dst.end(), it, piece.polyline.points.end());
        } else
            merged.emplace_back(std::move(piece));
    }
    pieces = std::move(merged);
}

// ---- Remplissage (PrintObject::apply_brick_infill) ----

// Tolérance sur la zone de remplissage de la couche du dessus : absorbe les écarts de tranchage d'une couche à l'autre.
constexpr double AREA_TOLERANCE_MM   = 0.05;
// Ce qui est dessous est moyenné sur cette longueur : un cordon qui croise les cordons montés et plats de la couche du
// dessous (tous les 0,4 mm environ) prend leur moyenne, la buse ne pouvant pas changer de débit aussi vite.
constexpr double AVERAGE_WINDOW_MM   = 2.;
constexpr double AVERAGE_STEP_MM     = 0.1;
// Cordon d'un remplissage rectiligne : segment dans le sens du motif (à 0,6° près) et sur sa grille de lignes (à 20 %
// de l'espacement près). Le motif est reconnu quand ces cordons font au moins 60 % de la longueur.
constexpr double STRAND_MAX_SIN      = 0.01;
constexpr double STRAND_GRID_TOL     = 0.2;
constexpr double STRAND_MIN_RATIO    = 0.6;
// Boucle d'un remplissage concentrique : ses deux bouts sont à moins de 2 largeurs l'un de l'autre. Le motif est
// reconnu quand ces boucles font au moins la moitié de la longueur.
constexpr double LOOP_MAX_GAP_WIDTHS = 2.;
constexpr double LOOP_MIN_RATIO      = 0.5;

// Zone, avec les boîtes englobantes de ses morceaux.
struct BrickArea
{
    ExPolygons               expolygons;
    std::vector<BoundingBox> bboxes;

    bool empty() const { return expolygons.empty(); }
    void set(ExPolygons &&src)
    {
        expolygons = std::move(src);
        bboxes     = bboxes_of(expolygons);
    }
    // Les morceaux qui touchent bbox.
    BrickArea local(const BoundingBox &bbox) const
    {
        BrickArea out;
        for (size_t i = 0; i < expolygons.size(); ++i)
            if (bboxes[i].overlap(bbox)) {
                out.expolygons.emplace_back(expolygons[i]);
                out.bboxes.emplace_back(bboxes[i]);
            }
        return out;
    }
    bool contains(const Point &pt) const
    {
        for (size_t i = 0; i < expolygons.size(); ++i)
            if (bboxes[i].contains(pt) && expolygons[i].contains(pt))
                return true;
        return false;
    }
};

// Les chemins d'une boucle ou d'une suite de chemins (lecture seule).
const ExtrusionPaths* const_wall_paths(const ExtrusionEntity &entity)
{
    if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&entity))
        return &loop->paths;
    if (const auto *multi_path = dynamic_cast<const ExtrusionMultiPath*>(&entity))
        return &multi_path->paths;
    return nullptr;
}

// Premier chemin d'une entité : le chemin lui-même, ou celui d'une boucle ou d'une suite de chemins.
const ExtrusionPath* first_path_of(const ExtrusionEntity &entity)
{
    if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity))
        return path;
    const ExtrusionPaths *paths = const_wall_paths(entity);
    return paths == nullptr || paths->empty() ? nullptr : &paths->front();
}

// Numéro d'une paroi (-1 pour ce qui n'en est pas une) et hauteur de buse de ce qui est monté (0 sinon).
int wall_index_of(const ExtrusionEntity &entity)
{
    const ExtrusionPaths *paths = const_wall_paths(entity);
    return paths == nullptr || paths->empty() ? -1 : paths->front().wall_index;
}
float z_shift_of(const ExtrusionEntity &entity)
{
    const ExtrusionPath *path = first_path_of(entity);
    return path == nullptr ? 0.f : path->z_shift;
}

// Espacement de deux cordons voisins (modèle de Flow : rectangle aux bouts arrondis), mis à l'échelle.
double extrusion_spacing(const ExtrusionPath &path)
{
    const double spacing = double(path.width) - double(path.height) * (1. - 0.25 * PI);
    return scale_(spacing > 0. ? spacing : double(path.width));
}

// Appelle fn sur chaque paroi d'une couche (les collections par île sont dépliées).
template<typename Fn> void for_each_wall(const Layer &layer, Fn &&fn)
{
    for (const LayerRegion *layerm : layer.regions())
        for (const ExtrusionEntity *island : layerm->perimeters.entities) {
            if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(island)) {
                for (const ExtrusionEntity *entity : collection->entities)
                    fn(*entity);
            } else
                fn(*island);
        }
}

// Île (lslice) qui contient pt, -1 sinon.
int island_of(const Layer &layer, const Point &pt)
{
    for (size_t i = 0; i < layer.lslices.size(); ++i)
        if ((i >= layer.lslices_bboxes.size() || layer.lslices_bboxes[i].contains(pt)) && layer.lslices[i].contains(pt))
            return int(i);
    return -1;
}

// Remplissage qui suit l'alternance des parois : creux ou plein interne, à la hauteur de la couche (pas de remplissage
// combiné sur plusieurs couches).
bool is_brick_infill(const ExtrusionPath &path, const double layer_height)
{
    return (path.role() == erInternalInfill || path.role() == erSolidInfill) && !path.is_force_no_extrusion() &&
           std::abs(path.height - layer_height) < EPSILON;
}
// Une suite de chemins peut contenir des déplacements sans extrusion (motif « Monotone ligne »).
bool is_brick_infill(const ExtrusionEntity &entity, const double layer_height)
{
    if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity))
        return is_brick_infill(*path, layer_height);
    const ExtrusionPaths *paths = const_wall_paths(entity);
    if (paths == nullptr)
        return false;
    bool extruded = false;
    for (const ExtrusionPath &path : *paths) {
        if (path.is_force_no_extrusion())
            continue;
        if (!is_brick_infill(path, layer_height))
            return false;
        extruded = true;
    }
    return extruded;
}

enum class Coverage { None, Partial, All };

// Part des lignes dans une zone (à MIN_PIECE_MM près).
Coverage coverage_of(const Polylines &lines, const ExPolygons &area)
{
    if (lines.empty() || area.empty())
        return Coverage::None;
    const double length = total_length(lines);
    const double inside = total_length(intersection_pl(lines, area));
    if (inside < scale_(MIN_PIECE_MM))
        return Coverage::None;
    return length - inside < scale_(MIN_PIECE_MM) ? Coverage::All : Coverage::Partial;
}

// État d'un morceau de remplissage : monté ou non.
constexpr int RAISED = 1;

struct StatePiece
{
    Polyline polyline;
    int      state { 0 };
};

// Comme split_by_area(), avec un état quelconque.
template<typename StateFn> std::vector<StatePiece> split_by_state(const Polyline &polyline, StateFn &&state_of)
{
    const double            step = scale_(MIN_PIECE_MM);
    std::vector<StatePiece> pieces;
    for (size_t i = 1; i < polyline.points.size(); ++i) {
        const Point &a      = polyline.points[i - 1];
        const Point &b      = polyline.points[i];
        const int    nsteps = std::max(1, int(std::ceil((b - a).cast<double>().norm() / step)));
        for (int k = 0; k < nsteps; ++k) {
            const int state = state_of(lerp(a, b, (k + 0.5) / nsteps));
            if (pieces.empty()) {
                pieces.push_back({ Polyline(), state });
                pieces.back().polyline.points.push_back(a);
            } else if (pieces.back().state != state) {
                const Point start = k == 0 ? a : lerp(a, b, double(k) / nsteps);
                if (pieces.back().polyline.points.back() != start)
                    pieces.back().polyline.points.push_back(start);
                pieces.push_back({ Polyline(), state });
                pieces.back().polyline.points.push_back(start);
            }
        }
        if (!pieces.empty() && pieces.back().polyline.points.back() != b)
            pieces.back().polyline.points.push_back(b);
    }
    return pieces;
}

// Comme merge_short_pieces(), avec un état quelconque.
void merge_short_states(std::vector<StatePiece> &pieces, const double min_length)
{
    if (pieces.size() > 1)
        for (size_t i = 0; i < pieces.size(); ++i)
            if (pieces[i].polyline.points.size() < 2 || pieces[i].polyline.length() < min_length)
                pieces[i].state = i > 0 ? pieces[i - 1].state : pieces[i + 1].state;

    std::vector<StatePiece> merged;
    merged.reserve(pieces.size());
    for (StatePiece &piece : pieces) {
        if (piece.polyline.points.empty())
            continue;
        if (!merged.empty() && merged.back().state == piece.state) {
            Points &dst = merged.back().polyline.points;
            auto    it  = piece.polyline.points.begin();
            if (!dst.empty() && dst.back() == *it)
                ++it;
            dst.insert(dst.end(), it, piece.polyline.points.end());
        } else
            merged.emplace_back(std::move(piece));
    }
    pieces = std::move(merged);
}

// Longueurs cumulées (mises à l'échelle) aux points d'une polyligne.
std::vector<double> cumulated_lengths(const Polyline &polyline)
{
    std::vector<double> out(polyline.points.size(), 0.);
    for (size_t i = 1; i < polyline.points.size(); ++i)
        out[i] = out[i - 1] + (polyline.points[i] - polyline.points[i - 1]).cast<double>().norm();
    return out;
}

// Point d'une polyligne (au moins 2 points) à l'abscisse curviligne t.
Point point_at(const Polyline &polyline, const std::vector<double> &lengths, const double t)
{
    const Points &pts = polyline.points;
    const size_t  i   = std::min<size_t>(size_t(std::upper_bound(lengths.begin(), lengths.end(), t) - lengths.begin()), pts.size() - 1);
    if (i == 0)
        return pts.front();
    const double length = lengths[i] - lengths[i - 1];
    return length > 0. ? lerp(pts[i - 1], pts[i], std::clamp((t - lengths[i - 1]) / length, 0., 1.)) : pts[i];
}

// Morceau d'une polyligne entre les abscisses curvilignes t0 et t1.
Polyline sub_polyline(const Polyline &polyline, const std::vector<double> &lengths, const double t0, const double t1)
{
    Polyline out;
    out.points.emplace_back(point_at(polyline, lengths, t0));
    for (size_t i = 0; i < polyline.points.size(); ++i)
        if (lengths[i] > t0 && lengths[i] < t1 && polyline.points[i] != out.points.back())
            out.points.emplace_back(polyline.points[i]);
    const Point end = point_at(polyline, lengths, t1);
    if (end != out.points.back() || out.points.size() < 2)
        out.points.emplace_back(end);
    return out;
}

// Abscisse curviligne d'un point posé sur la polyligne.
double param_on(const Polyline &polyline, const std::vector<double> &lengths, const Point &pt)
{
    const Vec2d p         = pt.cast<double>();
    double      best_dist = std::numeric_limits<double>::max();
    double      best_t    = 0.;
    for (size_t i = 1; i < polyline.points.size(); ++i) {
        const Vec2d  a  = polyline.points[i - 1].cast<double>();
        const Vec2d  ab = polyline.points[i].cast<double>() - a;
        const double l2 = ab.squaredNorm();
        const double u  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
        const double d2 = (a + u * ab - p).squaredNorm();
        if (d2 < best_dist) {
            best_dist = d2;
            best_t    = lengths[i - 1] + u * (lengths[i] - lengths[i - 1]);
        }
    }
    return best_t;
}

// Morceau d'un chemin, et part de sa longueur posée sur ce qui est monté dans la couche du dessous, en quarts (0 : rien,
// 4 : tout).
struct BelowPiece
{
    Polyline polyline;
    int      quarters { 0 };
};

// Parties de chaque polyligne posées sur ce qui est monté dessous (below), comme intersection_pl(), mais en un seul
// découpage pour toutes : un découpage par chemin contre une grande empreinte montée coûtait des secondes par couche
// pleine (des milliers de cordons, chacun contre des milliers d'arêtes). Chaque point porte en Z le numéro de sa
// polyligne (à partir de 1, -1 pour l'empreinte), et les points d'intersection aussi (rappel ZFill) : chaque morceau
// retrouve ainsi sa polyligne.
std::vector<Polylines> clip_on_below(const std::vector<const Polyline*> &polylines, const ExPolygons &below)
{
    std::vector<Polylines> out(polylines.size());
    if (polylines.empty() || below.empty())
        return out;
    ClipperLib_Z::Clipper clipper;
    clipper.ZFillFunction([](const ClipperLib_Z::IntPoint &e1bot, const ClipperLib_Z::IntPoint &e1top, const ClipperLib_Z::IntPoint &e2bot,
                             const ClipperLib_Z::IntPoint &e2top, ClipperLib_Z::IntPoint &pt) {
        pt.z() = std::max({ e1bot.z(), e1top.z(), e2bot.z(), e2top.z() });
    });
    for (size_t i = 0; i < polylines.size(); ++i)
        clipper.AddPath(ClipperZUtils::to_zpath(polylines[i]->points, coord_t(i + 1)), ClipperLib_Z::ptSubject, false);
    clipper.AddPaths(ClipperZUtils::expolygons_to_zpaths_with_same_z<false>(below, coord_t(-1)), ClipperLib_Z::ptClip, true);
    ClipperLib_Z::PolyTree tree;
    clipper.Execute(ClipperLib_Z::ctIntersection, tree, ClipperLib_Z::pftNonZero, ClipperLib_Z::pftNonZero);
    ClipperLib_Z::Paths pieces;
    ClipperLib_Z::PolyTreeToPaths(tree, pieces);
    for (const ClipperLib_Z::Path &piece : pieces) {
        // Un sommet de l'empreinte peut se glisser dans un morceau (Z = -1) : le numéro est pris sur un autre point.
        const auto it = std::find_if(piece.begin(), piece.end(), [](const ClipperLib_Z::IntPoint &pt) { return pt.z() > 0; });
        if (it != piece.end() && size_t(it->z()) <= polylines.size())
            out[size_t(it->z()) - 1].emplace_back(ClipperZUtils::from_zpath(piece));
    }
    return out;
}

// Part de ce qui est monté dessous le long d'un chemin, moyennée sur AVERAGE_WINDOW_MM et arrondie au quart, par
// morceaux d'au moins MIN_PIECE_MM, à partir des parties du chemin posées sur du monté (inside, clip_on_below()). Un
// chemin entièrement dans le même cas reste d'un seul morceau.
std::vector<BelowPiece> below_profile(const Polyline &polyline, const Polylines &inside_pieces)
{
    std::vector<BelowPiece> out;
    if (polyline.points.size() < 2)
        return out;
    const std::vector<double> lengths = cumulated_lengths(polyline);
    const double              length  = lengths.back();
    // Parties posées sur du monté, en abscisses curvilignes.
    std::vector<std::pair<double, double>> intervals;
    if (length > SCALED_EPSILON)
        for (const Polyline &inside : inside_pieces)
            if (inside.points.size() >= 2) {
                double t0 = param_on(polyline, lengths, inside.points.front());
                double t1 = param_on(polyline, lengths, inside.points.back());
                if (t0 > t1)
                    std::swap(t0, t1);
                if (t1 > t0)
                    intervals.emplace_back(t0, t1);
            }
    std::sort(intervals.begin(), intervals.end());
    std::vector<std::pair<double, double>> covered;
    for (const std::pair<double, double> &interval : intervals)
        if (!covered.empty() && interval.first <= covered.back().second)
            covered.back().second = std::max(covered.back().second, interval.second);
        else
            covered.emplace_back(interval);
    double covered_length = 0.;
    for (const std::pair<double, double> &interval : covered)
        covered_length += interval.second - interval.first;
    if (covered_length <= SCALED_EPSILON || covered_length >= length - SCALED_EPSILON) {
        out.push_back({ polyline, covered_length <= SCALED_EPSILON ? 0 : 4 });
        return out;
    }

    // Longueur posée sur du monté entre le début du chemin et t.
    std::vector<double> before(covered.size(), 0.);
    for (size_t i = 1; i < covered.size(); ++i)
        before[i] = before[i - 1] + covered[i - 1].second - covered[i - 1].first;
    auto covered_until = [&covered, &before](const double t) {
        const auto it = std::upper_bound(covered.begin(), covered.end(), t,
            [](const double value, const std::pair<double, double> &interval) { return value < interval.first; });
        if (it == covered.begin())
            return 0.;
        const size_t i = size_t(it - covered.begin()) - 1;
        return before[i] + std::min(t, covered[i].second) - covered[i].first;
    };
    // Moyenne glissante, arrondie au quart.
    const double     window = std::min(length, scale_(AVERAGE_WINDOW_MM));
    const int        nsteps = std::max(1, int(std::ceil(length / scale_(AVERAGE_STEP_MM))));
    const double     step   = length / nsteps;
    std::vector<int> quarters(nsteps, 0);
    for (int k = 0; k < nsteps; ++k) {
        const double a = std::clamp((k + 0.5) * step - 0.5 * window, 0., length - window);
        quarters[k]    = std::clamp(int(std::lround(4. * (covered_until(a + window) - covered_until(a)) / window)), 0, 4);
    }
    // Morceaux de même valeur ; un morceau trop court prend la valeur de son voisin.
    struct Run
    {
        double t0;
        double t1;
        int    quarters;
    };
    std::vector<Run> runs;
    for (int k = 0; k < nsteps; ++k)
        if (!runs.empty() && runs.back().quarters == quarters[k])
            runs.back().t1 = (k + 1) * step;
        else
            runs.push_back({ k * step, (k + 1) * step, quarters[k] });
    if (runs.size() > 1)
        for (size_t i = 0; i < runs.size(); ++i)
            if (runs[i].t1 - runs[i].t0 < scale_(MIN_PIECE_MM))
                runs[i].quarters = i > 0 ? runs[i - 1].quarters : runs[i + 1].quarters;
    std::vector<Run> merged;
    for (const Run &run : runs)
        if (!merged.empty() && merged.back().quarters == run.quarters)
            merged.back().t1 = run.t1;
        else
            merged.push_back(run);
    if (merged.size() == 1) {
        out.push_back({ polyline, merged.front().quarters });
        return out;
    }
    merged.back().t1 = length;
    for (const Run &run : merged)
        out.push_back({ sub_polyline(polyline, lengths, run.t0, run.t1), run.quarters });
    return out;
}

// Grille des lignes d'un remplissage rectiligne : sens des cordons, et position des lignes le long de la normale.
struct StrandGrid
{
    Vec2d     dir       = Vec2d(1., 0.);
    Vec2d     normal    = Vec2d(0., 1.);
    // Position d'une ligne le long de la normale, et espacement des lignes (mis à l'échelle).
    double    reference { 0. };
    double    spacing   { 0. };
    // Numéro de cette ligne compté depuis l'origine de l'objet : une ligne garde sa parité d'une couche à l'autre.
    long long first     { 0 };
};

// Parité de la ligne de la grille qui porte le segment a-b : 0 ou 1, -1 si ce n'est pas un cordon (liaison).
int strand_parity(const StrandGrid &grid, const Point &a, const Point &b)
{
    const Vec2d  v      = (b - a).cast<double>();
    const double length = v.norm();
    if (length <= SCALED_EPSILON || std::abs(cross2(v, grid.dir)) > STRAND_MAX_SIN * length)
        return -1;
    const double t    = (0.5 * (a.cast<double>() + b.cast<double>()).dot(grid.normal) - grid.reference) / grid.spacing;
    const double line = std::round(t);
    if (std::abs(t - line) > STRAND_GRID_TOL)
        return -1;
    return int(((grid.first + (long long)line) % 2 + 2) % 2);
}

// Grille d'un remplissage rectiligne (zigzag de cordons parallèles), si ces chemins en sont un : le plus long segment
// donne le sens des cordons, la largeur des chemins leur espacement.
bool detect_strand_grid(const std::vector<const ExtrusionPath*> &paths, StrandGrid &grid)
{
    double longest = 0.;
    double total   = 0.;
    Point  longest_a;
    Point  longest_b;
    for (const ExtrusionPath *path : paths)
        for (size_t i = 1; i < path->polyline.points.size(); ++i) {
            const double length = (path->polyline.points[i] - path->polyline.points[i - 1]).cast<double>().norm();
            total += length;
            if (length > longest) {
                longest   = length;
                longest_a = path->polyline.points[i - 1];
                longest_b = path->polyline.points[i];
            }
        }
    if (paths.empty() || longest <= SCALED_EPSILON)
        return false;
    grid.dir     = (longest_b - longest_a).cast<double>().normalized();
    grid.normal  = Vec2d(-grid.dir.y(), grid.dir.x());
    grid.spacing = extrusion_spacing(*paths.front());
    if (grid.spacing <= SCALED_EPSILON)
        return false;
    grid.reference = 0.5 * (longest_a.cast<double>() + longest_b.cast<double>()).dot(grid.normal);
    grid.first     = std::llround(grid.reference / grid.spacing);
    double strands = 0.;
    for (const ExtrusionPath *path : paths)
        for (size_t i = 1; i < path->polyline.points.size(); ++i)
            if (strand_parity(grid, path->polyline.points[i - 1], path->polyline.points[i]) >= 0)
                strands += (path->polyline.points[i] - path->polyline.points[i - 1]).cast<double>().norm();
    return strands >= STRAND_MIN_RATIO * total;
}

// Morceau d'un zigzag rectiligne : un ou plusieurs cordons de même parité, avec la moitié des liaisons qui les
// entourent (le zigzag est coupé au milieu de chaque liaison entre deux cordons de parités différentes).
struct BrickStrand
{
    Polyline polyline;
    bool     odd { false };
};

std::vector<BrickStrand> split_into_strands(const Polyline &polyline, const StrandGrid &grid)
{
    std::vector<BrickStrand> out;
    const Points &pts = polyline.points;
    if (pts.size() < 2)
        return out;
    const std::vector<double> lengths = cumulated_lengths(polyline);
    std::vector<double>       cuts;
    std::vector<int>          parities;
    int                       previous     = -1;
    double                    previous_end = 0.;
    for (size_t i = 1; i < pts.size(); ++i) {
        const int parity = strand_parity(grid, pts[i - 1], pts[i]);
        if (parity < 0)
            continue;
        if (previous < 0)
            parities.emplace_back(parity);
        else if (parity != previous) {
            cuts.emplace_back(0.5 * (previous_end + lengths[i - 1]));
            parities.emplace_back(parity);
        }
        previous     = parity;
        previous_end = lengths[i];
    }
    if (parities.size() < 2) {
        out.push_back({ polyline, parities.size() == 1 && parities.front() == 1 });
        return out;
    }
    double t0 = 0.;
    for (size_t i = 0; i < parities.size(); ++i) {
        const double t1 = i < cuts.size() ? cuts[i] : lengths.back();
        out.push_back({ sub_polyline(polyline, lengths, t0, t1), parities[i] == 1 });
        t0 = t1;
    }
    return out;
}

// Distance d'un point au bord du morceau de zone qui le contient, -1 s'il est dehors.
double distance_to_edge(const BrickArea &area, const Point &pt)
{
    for (size_t i = 0; i < area.expolygons.size(); ++i)
        if (area.bboxes[i].contains(pt) && area.expolygons[i].contains(pt)) {
            double distance2 = std::numeric_limits<double>::max();
            auto   visit     = [&distance2, &pt](const Polygon &polygon) {
                for (size_t j = 0; j < polygon.points.size(); ++j)
                    distance2 = std::min(distance2, Line(polygon.points[j], polygon.points[(j + 1) % polygon.points.size()]).distance_to_squared(pt));
            };
            visit(area.expolygons[i].contour);
            for (const Polygon &hole : area.expolygons[i].holes)
                visit(hole);
            return std::sqrt(distance2);
        }
    return -1.;
}

// Paroi la plus interne d'une région à remplissage en briques (zone : ses slices). Dans une île, un modificateur qui a
// plus ou moins de parois que la pièce a la sienne (parois continues : parois communes + parois en plus).
struct BrickInfillRegion
{
    BrickArea area;
    int       innermost { -1 };
};

struct BrickInfillLayer
{
    // Numéro de la paroi la plus interne de chaque île (lslices), -1 sans paroi ; et de chaque région à remplissage en
    // briques, qui le précise.
    std::vector<int>               innermost;
    std::vector<BrickInfillRegion> regions;
    // Régions dont la paroi la plus interne reste au Z de la couche (numéro pair : 1, 3, 5 parois...).
    ExPolygons       shifted_area;
    // Remplissage creux de ces régions, qui monte d'un bloc ; remplissage plein interne, où qu'il soit.
    ExPolygons       sparse_area;
    BrickArea        solid_area;
    // Zones où ils montent : le creux sous le creux de la couche du dessus, le plein sous tout remplissage concerné de
    // la couche du dessus (up_solid_block : partie de ces îles, pour un plein d'un autre motif qui monte d'un bloc).
    BrickArea        up_sparse;
    BrickArea        up_solid;
    BrickArea        up_solid_block;
    // Ce qui est monté dans la couche (parois, zone du creux, cordons du plein) : la couche du dessus s'y pose plus haut.
    BrickArea        below;
    // Hauteur de buse de ce qui est monté, au-dessus du print_z de la couche (mm), comme pour les parois.
    double           shift { 0. };
};

// Numéro de la paroi la plus interne là où est pt : celle de sa région, sinon celle de son île.
int innermost_at(const Layer &layer, const BrickInfillLayer &brick, const Point &pt)
{
    for (const BrickInfillRegion &region : brick.regions)
        if (region.area.contains(pt))
            return region.innermost;
    const int island = island_of(layer, pt);
    return island >= 0 && size_t(island) < brick.innermost.size() ? brick.innermost[island] : -1;
}

// Remplace un chemin par ses morceaux, chacun à sa hauteur de buse : monté (shift) ou au Z de la couche. Les morceaux
// montés du plein s'ajoutent à l'empreinte de ce qui est monté (raised).
void emit_raised_pieces(ExtrusionPath *path, std::vector<StatePiece> &&pieces, const float shift, const bool solid,
                        ExtrusionEntitiesPtr &out, Polygons &raised)
{
    if (pieces.size() < 2) {
        // Un seul état sur tout le chemin : il reste tel quel.
        if (!pieces.empty() && (pieces.front().state & RAISED)) {
            path->z_shift = shift;
            if (solid)
                path->polygons_covered_by_width(raised, float(SCALED_EPSILON));
        }
        out.emplace_back(path);
        return;
    }
    for (StatePiece &piece : pieces) {
        if (piece.polyline.points.size() < 2)
            continue;
        auto *sub    = new ExtrusionPath(std::move(piece.polyline), *path);
        sub->z_shift = (piece.state & RAISED) ? shift : 0.f;
        if (solid && sub->z_shift > 0.f)
            sub->polygons_covered_by_width(raised, float(SCALED_EPSILON));
        out.emplace_back(sub);
    }
    delete path;
}

// Remplissage qui monte d'un bloc (le creux, ou un plein sans cordons voisins) là où il est dans la zone montée (up). Un
// chemin est coupé là où il en sort ; une boucle ou une suite de chemins monte d'un seul tenant si presque toute sa
// longueur y est.
void raise_block(ExtrusionEntity *entity, const Coverage collection_coverage, const BrickArea &up, const float shift, const bool solid,
                 ExtrusionEntitiesPtr &out, Polygons &raised)
{
    if (auto *path = dynamic_cast<ExtrusionPath*>(entity)) {
        const Coverage coverage = collection_coverage == Coverage::Partial ? coverage_of({ path->polyline }, up.expolygons) : collection_coverage;
        if (coverage != Coverage::Partial) {
            if (coverage == Coverage::All) {
                path->z_shift = shift;
                if (solid)
                    path->polygons_covered_by_width(raised, float(SCALED_EPSILON));
            }
            out.emplace_back(path);
            return;
        }
        std::vector<StatePiece> pieces = split_by_state(path->polyline, [&up](const Point &pt) { return up.contains(pt) ? RAISED : 0; });
        merge_short_states(pieces, scale_(MIN_PIECE_MM));
        emit_raised_pieces(path, std::move(pieces), shift, solid, out, raised);
    } else if (ExtrusionPaths *paths = wall_paths(entity)) {
        const Polylines center  = entity->as_polylines();
        const double    length  = total_length(center);
        const double    covered = up.empty() ? 0. : total_length(intersection_pl(center, up.expolygons));
        if (length > 0. && length - covered <= std::min(MAX_UNCOVERED_RATIO * length, scale_(MAX_UNCOVERED_MM))) {
            for (ExtrusionPath &p : *paths)
                p.z_shift = shift;
            if (solid)
                entity->polygons_covered_by_width(raised, float(SCALED_EPSILON));
        }
        out.emplace_back(entity);
    } else
        out.emplace_back(entity);
}

// Plein rectiligne : un cordon sur deux (lignes impaires de la grille) monte dans la zone montée ; le reste du zigzag
// reste au Z de la couche, d'un seul tenant entre deux cordons montés.
void raise_strands(ExtrusionPath *path, const StrandGrid &grid, const BrickArea &up, const float shift, ExtrusionEntitiesPtr &out,
                   Polygons &raised)
{
    if (coverage_of({ path->polyline }, up.expolygons) == Coverage::None) {
        out.emplace_back(path);
        return;
    }
    std::vector<StatePiece> pieces;
    for (BrickStrand &strand : split_into_strands(path->polyline, grid)) {
        const Coverage coverage = strand.odd ? coverage_of({ strand.polyline }, up.expolygons) : Coverage::None;
        if (coverage != Coverage::Partial) {
            pieces.push_back({ std::move(strand.polyline), coverage == Coverage::All ? RAISED : 0 });
            continue;
        }
        for (StatePiece &piece : split_by_state(strand.polyline, [&up](const Point &pt) { return up.contains(pt) ? RAISED : 0; }))
            pieces.push_back(std::move(piece));
    }
    merge_short_states(pieces, scale_(MIN_PIECE_MM));
    emit_raised_pieces(path, std::move(pieces), shift, true, out, raised);
}

// Remplissage concentrique (Arachne) : chaînes de chemins bout à bout, ou boucles. Vrai si ces chaînes sont surtout
// des boucles presque fermées.
bool detect_loops(const ExtrusionEntityCollection &collection, const double layer_height, std::vector<std::vector<size_t>> &chains)
{
    chains.clear();
    const ExtrusionPath *previous = nullptr;
    for (size_t i = 0; i < collection.entities.size(); ++i) {
        const ExtrusionEntity *entity = collection.entities[i];
        if (!is_brick_infill(*entity, layer_height) || entity->role() != erSolidInfill) {
            previous = nullptr;
            continue;
        }
        const auto *path = dynamic_cast<const ExtrusionPath*>(entity);
        if (path != nullptr && previous != nullptr && previous->last_point() == path->first_point())
            chains.back().emplace_back(i);
        else
            chains.push_back({ i });
        previous = path;
    }
    double total = 0.;
    double loops = 0.;
    for (const std::vector<size_t> &chain : chains) {
        double length = 0.;
        for (size_t i : chain)
            length += collection.entities[i]->length();
        total += length;
        const ExtrusionEntity &front = *collection.entities[chain.front()];
        const ExtrusionPath   *first = first_path_of(front);
        const double           width = first == nullptr ? 0. : scale_(double(first->width));
        const bool             closed = dynamic_cast<const ExtrusionLoop*>(&front) != nullptr ||
            (width > 0. && length > 4. * width &&
             (front.first_point() - collection.entities[chain.back()]->last_point()).cast<double>().norm() <= LOOP_MAX_GAP_WIDTHS * width);
        if (closed)
            loops += length;
    }
    return total > 0. && loops >= LOOP_MIN_RATIO * total;
}

// Plein concentrique : les boucles continuent l'alternance des parois vers l'intérieur de l'île (la première boucle est
// à l'inverse de la paroi la plus interne). Une boucle monte d'un seul tenant si presque toute sa longueur est dans la
// zone montée (up).
void raise_loops(ExtrusionEntityCollection &collection, const std::vector<std::vector<size_t>> &chains, const Layer &layer,
                 const BrickInfillLayer &brick, const BrickArea &up, Polygons &raised)
{
    for (const std::vector<size_t> &chain : chains) {
        Polylines center;
        for (size_t i : chain)
            collection.entities[i]->collect_polylines(center);
        const double length = total_length(center);
        if (center.empty() || length <= 0.)
            continue;
        // Rang de la boucle depuis le bord de la zone pleine : médiane sur quelques points.
        Points samples;
        for (const Polyline &polyline : center)
            append(samples, polyline.points);
        std::vector<double> distances;
        const size_t        nsamples = std::min<size_t>(9, samples.size());
        for (size_t s = 0; s < nsamples; ++s)
            if (const double distance = distance_to_edge(brick.solid_area, samples[s * samples.size() / nsamples]); distance >= 0.)
                distances.emplace_back(distance);
        const ExtrusionPath *first = first_path_of(*collection.entities[chain.front()]);
        if (distances.empty() || first == nullptr)
            continue;
        std::nth_element(distances.begin(), distances.begin() + distances.size() / 2, distances.end());
        const int rank      = int(distances[distances.size() / 2] / extrusion_spacing(*first));
        const int innermost = innermost_at(layer, brick, center.front().first_point());
        if ((rank + (innermost >= 0 && innermost % 2 == 0 ? 0 : 1)) % 2 != 0)
            continue;
        const double covered = up.empty() ? 0. : total_length(intersection_pl(center, up.expolygons));
        if (length - covered > std::min(MAX_UNCOVERED_RATIO * length, scale_(MAX_UNCOVERED_MM)))
            continue;
        for (size_t i : chain) {
            ExtrusionEntity *entity = collection.entities[i];
            if (auto *path = dynamic_cast<ExtrusionPath*>(entity))
                path->z_shift = float(brick.shift);
            else if (ExtrusionPaths *paths = wall_paths(entity))
                for (ExtrusionPath &p : *paths)
                    p.z_shift = float(brick.shift);
            entity->polygons_covered_by_width(raised, float(SCALED_EPSILON));
        }
    }
}

// Hauteur de buse du remplissage d'une collection (le remplissage d'une surface) : le creux monte d'un bloc ; le plein
// alterne ses cordons (motifs rectilignes) ou ses boucles (concentrique), ou monte d'un bloc (autres motifs).
void raise_brick_infill(ExtrusionEntityCollection &collection, const Layer &layer, const BrickInfillLayer &brick, Polygons &raised)
{
    Polylines                         sparse_lines;
    Polylines                         solid_lines;
    std::vector<const ExtrusionPath*> solid_paths;
    bool                              solid_other = false;
    for (const ExtrusionEntity *entity : collection.entities) {
        if (!is_brick_infill(*entity, layer.height))
            continue;
        if (entity->role() == erSolidInfill) {
            entity->collect_polylines(solid_lines);
            if (const auto *path = dynamic_cast<const ExtrusionPath*>(entity))
                solid_paths.emplace_back(path);
            else
                solid_other = true;
        } else
            entity->collect_polylines(sparse_lines);
    }
    auto local = [](const BrickArea &area, const Polylines &lines) {
        if (area.empty() || lines.empty())
            return BrickArea();
        BoundingBox bbox = get_extents(lines);
        bbox.offset(SCALED_EPSILON);
        return area.local(bbox);
    };
    const BrickArea up_sparse       = local(brick.up_sparse, sparse_lines);
    const BrickArea up_solid        = local(brick.up_solid, solid_lines);
    const Coverage  sparse_coverage = coverage_of(sparse_lines, up_sparse.expolygons);
    const Coverage  solid_coverage  = coverage_of(solid_lines, up_solid.expolygons);
    if (sparse_coverage == Coverage::None && solid_coverage == Coverage::None)
        return;

    // Motif du plein : cordons rectilignes, boucles concentriques, ou autre (d'un bloc, comme le creux).
    enum class SolidMode { Strands, Loops, Block };
    SolidMode                        mode = SolidMode::Block;
    StrandGrid                       grid;
    std::vector<std::vector<size_t>> chains;
    if (solid_coverage != Coverage::None) {
        if (!solid_other && detect_strand_grid(solid_paths, grid))
            mode = SolidMode::Strands;
        else if (detect_loops(collection, layer.height, chains))
            mode = SolidMode::Loops;
    }
    const BrickArea up_block       = mode == SolidMode::Block ? local(brick.up_solid_block, solid_lines) : BrickArea();
    const Coverage  block_coverage = coverage_of(solid_lines, up_block.expolygons);
    const float     shift          = float(brick.shift);
    if (solid_coverage != Coverage::None && mode == SolidMode::Loops)
        raise_loops(collection, chains, layer, brick, up_solid, raised);

    ExtrusionEntitiesPtr out;
    out.reserve(collection.entities.size());
    for (ExtrusionEntity *entity : collection.entities) {
        if (!is_brick_infill(*entity, layer.height))
            out.emplace_back(entity);
        else if (entity->role() != erSolidInfill)
            raise_block(entity, sparse_coverage, up_sparse, shift, false, out, raised);
        else if (solid_coverage == Coverage::None || mode == SolidMode::Loops)
            out.emplace_back(entity);
        else if (mode == SolidMode::Strands)
            raise_strands(static_cast<ExtrusionPath*>(entity), grid, up_solid, shift, out, raised);
        else
            raise_block(entity, block_coverage, up_block, shift, true, out, raised);
    }
    collection.entities = std::move(out);
}

// Hauteurs de la couche et de ce qui est monté dans la couche du dessous.
struct InfillHeights
{
    double height      { 0. };
    double lower_shift { 0. };
};

// Hauteur réelle d'un morceau, du dessus de ce qui est dessous jusqu'à la buse, et débit en proportion.
void set_brick_height(ExtrusionPath &path, const int quarters, const InfillHeights &heights)
{
    const double real_height = heights.height + double(path.z_shift) - 0.25 * quarters * heights.lower_shift;
    if (real_height <= 0. || path.height <= 0.f || std::abs(real_height - double(path.height)) < EPSILON)
        return;
    path.mm3_per_mm *= real_height / double(path.height);
    path.height      = float(real_height);
}

// Hauteur réelle de chaque morceau du remplissage d'une collection, selon ce qui est monté dans la couche du dessous
// (below), moyenné le long des chemins.
void set_brick_infill_heights(ExtrusionEntityCollection &collection, const BrickArea &below, const InfillHeights &heights)
{
    Polylines lines;
    bool      raised = false;
    for (const ExtrusionEntity *entity : collection.entities)
        if (is_brick_infill(*entity, heights.height)) {
            entity->collect_polylines(lines);
            raised = raised || z_shift_of(*entity) > 0.f;
        }
    if (lines.empty())
        return;
    BoundingBox bbox = get_extents(lines);
    bbox.offset(SCALED_EPSILON);
    const BrickArea local_below = below.local(bbox);
    const bool      on_raised   = !local_below.empty() && coverage_of(lines, local_below.expolygons) != Coverage::None;
    if (!raised && !on_raised)
        return;
    // Chemins à profiler, dans l'ordre de la boucle ci-dessous, et leurs parties posées sur du monté : un seul découpage
    // pour toute la collection (clip_on_below()).
    std::vector<const Polyline*> polylines;
    if (on_raised)
        for (const ExtrusionEntity *entity : collection.entities)
            if (is_brick_infill(*entity, heights.height)) {
                if (const auto *path = dynamic_cast<const ExtrusionPath*>(entity))
                    polylines.emplace_back(&path->polyline);
                else if (const ExtrusionPaths *paths = const_wall_paths(*entity))
                    for (const ExtrusionPath &path : *paths)
                        if (!path.is_force_no_extrusion())
                            polylines.emplace_back(&path.polyline);
            }
    const std::vector<Polylines> inside = clip_on_below(polylines, local_below.expolygons);
    size_t next = 0;
    auto profile = [&polylines, &inside, &next](const Polyline &polyline) {
        if (next >= polylines.size())
            return std::vector<BelowPiece>();
        assert(polylines[next] == &polyline);
        const size_t i = next++;
        return below_profile(polyline, inside[i]);
    };

    ExtrusionEntitiesPtr out;
    out.reserve(collection.entities.size());
    for (ExtrusionEntity *entity : collection.entities) {
        if (!is_brick_infill(*entity, heights.height)) {
            out.emplace_back(entity);
            continue;
        }
        if (auto *path = dynamic_cast<ExtrusionPath*>(entity)) {
            std::vector<BelowPiece> pieces = profile(path->polyline);
            if (pieces.size() < 2) {
                set_brick_height(*path, pieces.empty() ? 0 : pieces.front().quarters, heights);
                out.emplace_back(path);
                continue;
            }
            // Une suite de chemins de débits différents, d'un seul tenant.
            auto *multi_path = new ExtrusionMultiPath();
            for (BelowPiece &piece : pieces) {
                multi_path->paths.emplace_back(std::move(piece.polyline), *path);
                set_brick_height(multi_path->paths.back(), piece.quarters, heights);
            }
            if (!path->can_reverse())
                multi_path->set_reverse();
            out.emplace_back(multi_path);
            delete path;
        } else if (ExtrusionPaths *paths = wall_paths(entity)) {
            ExtrusionPaths new_paths;
            new_paths.reserve(paths->size());
            for (ExtrusionPath &path : *paths) {
                std::vector<BelowPiece> pieces = path.is_force_no_extrusion() ? std::vector<BelowPiece>() : profile(path.polyline);
                if (pieces.size() < 2) {
                    if (!path.is_force_no_extrusion())
                        set_brick_height(path, pieces.empty() ? 0 : pieces.front().quarters, heights);
                    new_paths.emplace_back(std::move(path));
                    continue;
                }
                for (BelowPiece &piece : pieces) {
                    new_paths.emplace_back(std::move(piece.polyline), path);
                    set_brick_height(new_paths.back(), piece.quarters, heights);
                }
            }
            *paths = std::move(new_paths);
            out.emplace_back(entity);
        } else
            out.emplace_back(entity);
    }
    collection.entities = std::move(out);
}

} // namespace

void PrintObject::apply_brick_layers()
{
    bool enabled = false;
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id)
        if (this->printing_region(region_id).config().brick_layers.value)
            enabled = true;
    // Le mode vase imprime une seule paroi en spirale ; les sous-couches de couleur mélangée changent elles-mêmes de Z.
    if (!enabled || m_layers.size() < 2 || m_print->config().spiral_mode.value || m_print->config().enable_mixed_color_sublayer.value)
        return;

    BOOST_LOG_TRIVIAL(debug) << "Brick layers - start";
    std::vector<BrickLayer> bricks(m_layers.size());

    // 1. Parois et candidates de chaque couche (la 1re couche reste plate : elle n'a pas de candidates).
    tbb::parallel_for(tbb::blocked_range<size_t>(1, m_layers.size()), [this, &bricks](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_print->throw_if_canceled();
            BrickLayer &brick = bricks[layer_idx];
            Polygons    footprint;
            for (LayerRegion *layerm : m_layers[layer_idx]->regions()) {
                const PrintRegionConfig &config = layerm->region().config();
                if (!config.brick_layers.value)
                    continue;
                const double flow_ratio = config.brick_layers_flow.value / 100.;
                for (ExtrusionEntity *island : layerm->perimeters.entities) {
                    ExtrusionEntitiesPtr single { island };
                    const ExtrusionEntitiesPtr &entities = island->is_collection() ? static_cast<ExtrusionEntityCollection*>(island)->entities : single;
                    for (ExtrusionEntity *entity : entities) {
                        ExtrusionPaths *paths = wall_paths(entity);
                        if (paths == nullptr || paths->empty() || paths->front().wall_index < 0)
                            continue;
                        BrickWall wall;
                        wall.entity     = entity;
                        wall.paths      = paths;
                        wall.candidate  = is_candidate(*paths);
                        wall.flow_ratio = flow_ratio;
                        if (wall.candidate)
                            entity->polygons_covered_by_width(footprint, float(SCALED_EPSILON));
                        brick.walls.emplace_back(wall);
                    }
                }
            }
            if (!footprint.empty()) {
                brick.candidates        = union_ex(footprint);
                brick.candidates_bboxes = bboxes_of(brick.candidates);
            }
        }
    });
    m_print->throw_if_canceled();

    // 2. Hauteur de buse : une candidate monte si des candidates de la couche du dessus la recouvrent et si rien ne
    //    la laisse à l'air libre ; sinon elle reste au Z de sa couche.
    tbb::parallel_for(tbb::blocked_range<size_t>(1, m_layers.size()), [this, &bricks](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_print->throw_if_canceled();
            BrickLayer  &brick = bricks[layer_idx];
            const Layer &layer = *m_layers[layer_idx];
            const bool   top   = layer_idx + 1 >= m_layers.size();
            // La buse ne doit pas dépasser la moitié de la couche suivante, qui s'imprime à côté des parois montées.
            brick.shift = 0.5 * std::min(layer.height, top ? layer.height : m_layers[layer_idx + 1]->height);
            if (top || bricks[layer_idx + 1].candidates.empty())
                continue;
            const BrickLayer &upper       = bricks[layer_idx + 1];
            const Layer      &upper_layer = *m_layers[layer_idx + 1];

            Polygons footprint;
            for (BrickWall &wall : brick.walls) {
                if (!wall.candidate)
                    continue;
                const Polylines center = wall.entity->as_polylines();
                const double    length = total_length(center);
                if (length <= 0.)
                    continue;
                BoundingBox bbox = get_extents(center);
                bbox.offset(SCALED_EPSILON);

                const ExPolygons upper_candidates = select_by_bbox(upper.candidates, upper.candidates_bboxes, bbox);
                const double     covered          = upper_candidates.empty() ? 0. : total_length(intersection_pl(center, upper_candidates));
                if (length - covered > std::min(MAX_UNCOVERED_RATIO * length, scale_(MAX_UNCOVERED_MM)))
                    continue;
                const ExPolygons upper_slices = select_by_bbox(upper_layer.lslices, upper_layer.lslices_bboxes, bbox);
                if (total_length(diff_pl(center, upper_slices)) > scale_(MAX_EXPOSED_MM))
                    continue;

                for (ExtrusionPath &path : *wall.paths)
                    path.z_shift = float(brick.shift);
                wall.entity->polygons_covered_by_width(footprint, float(SCALED_EPSILON));
            }
            if (!footprint.empty()) {
                brick.raised        = union_ex(footprint);
                brick.raised_bboxes = bboxes_of(brick.raised);
            }
        }
    });
    m_print->throw_if_canceled();

    // 3. Débit : hauteur réelle de chaque morceau, entre le dessus de ce qui est dessous et la buse.
    tbb::parallel_for(tbb::blocked_range<size_t>(1, m_layers.size()), [this, &bricks](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_print->throw_if_canceled();
            BrickLayer       &brick  = bricks[layer_idx];
            const BrickLayer &lower  = bricks[layer_idx - 1];
            const double      height = m_layers[layer_idx]->height;

            for (BrickWall &wall : brick.walls) {
                const double top_offset = wall.paths->front().z_shift;
                const Polylines center  = wall.entity->as_polylines();
                BoundingBox     bbox    = get_extents(center);
                bbox.offset(SCALED_EPSILON);

                // Position par rapport aux parois montées de la couche du dessous.
                const ExPolygons below  = select_by_bbox(lower.raised, lower.raised_bboxes, bbox);
                bool             inside = false;
                bool             mixed  = false;
                if (!below.empty()) {
                    const double length         = total_length(center);
                    const double inside_length  = total_length(intersection_pl(center, below));
                    inside = length - inside_length < scale_(MIN_PIECE_MM);
                    mixed  = !inside && inside_length >= scale_(MIN_PIECE_MM);
                }
                if (top_offset <= 0. && !inside && !mixed)
                    continue; // paroi ordinaire, rien de décalé autour

                auto adjust = [&wall, top_offset, height, &lower](ExtrusionPath &path, bool on_raised) {
                    if (!is_adjustable(path))
                        return;
                    const double bottom_offset = on_raised ? lower.shift : 0.;
                    const double real_height   = height + top_offset - bottom_offset;
                    if (real_height <= 0.)
                        return;
                    double factor = real_height / path.height;
                    if (wall.candidate && (top_offset > 0. || on_raised))
                        factor *= wall.flow_ratio;
                    path.mm3_per_mm *= factor;
                    path.height      = float(real_height);
                };

                if (!mixed) {
                    for (ExtrusionPath &path : *wall.paths)
                        adjust(path, inside);
                    continue;
                }

                // Paroi en partie sur des parois montées : on la découpe là où ce qui est dessous change.
                const std::vector<BoundingBox> below_bboxes = bboxes_of(below);
                ExtrusionPaths out;
                out.reserve(wall.paths->size() + 4);
                for (ExtrusionPath &path : *wall.paths) {
                    std::vector<Piece> pieces;
                    if (is_adjustable(path))
                        pieces = split_by_area(path.polyline, below, below_bboxes);
                    merge_short_pieces(pieces, scale_(MIN_PIECE_MM));
                    if (pieces.size() < 2) {
                        // Un seul état sur tout le chemin.
                        adjust(path, !pieces.empty() && pieces.front().on_raised);
                        out.emplace_back(std::move(path));
                        continue;
                    }
                    for (Piece &piece : pieces) {
                        if (piece.polyline.points.size() < 2)
                            continue;
                        ExtrusionPath sub_path(std::move(piece.polyline), path);
                        adjust(sub_path, piece.on_raised);
                        out.emplace_back(std::move(sub_path));
                    }
                }
                *wall.paths = std::move(out);
            }
        }
    });
    m_print->throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Brick layers - end";
}

// Orio : « Remplissage en briques ». Le remplissage continue les couches en briques des parois :
//  - plein interne : un cordon sur deux monte d'une demi-couche, comme les parois, quel que soit le nombre de parois
//    (motifs rectilignes : lignes impaires de la grille du motif ; concentrique : une boucle sur deux, dans la
//    continuité des parois ; autres motifs : d'un bloc, comme le creux). Les motifs rectilignes sont empilés d'une
//    couche à l'autre (group_fills et make_fills, Fill.cpp) : même sens à chaque couche, et lignes sur une grille
//    commune, à un nombre entier d'espacements du centre de l'objet ; un cordon monté se pose sur un cordon monté ;
//  - creux : ses cordons sont trop espacés pour s'emboîter entre eux ; il monte d'un bloc dans les îles dont la paroi la
//    plus interne reste au Z de la couche (nombre impair de parois), pour s'emboîter avec elle.
// Cette passe s'exécute à la fin de PrintObject::infill(), quand le remplissage de toutes les couches existe :
//  1. Zones : creux de ces îles, plein interne partout (surfaces des régions concernées).
//  2. Zones montées : le creux monte sous le creux de la couche du dessus, le plein sous tout remplissage concerné de la
//     couche du dessus. Ailleurs (sous un dessus, un pont, une paroi, ou à la dernière couche) tout reste au Z de la
//     couche : les dessus restent plats, et aucune paroi n'est posée sur du remplissage monté.
//  3. Hauteur de buse de chaque morceau, et empreinte de tout ce qui est monté dans chaque couche.
//  4. Hauteur réelle de chaque morceau, du dessus de ce qui est monté dessous jusqu'à la buse : 150 % monté sur du plat,
//     100 % monté sur du monté, 50 % au Z de la couche sur du monté. Un cordon qui croise ceux de la couche du dessous
//     (cas des motifs qui ne s'empilent pas) prend la moyenne le long de son trajet : ~75 % au Z de la couche, ~125 %
//     monté.
// Les parois ont été traitées à l'étape des parois (apply_brick_layers) : cette passe les lit sans les modifier.
void PrintObject::apply_brick_infill()
{
    bool enabled = false;
    for (size_t region_id = 0; region_id < this->num_printing_regions(); ++region_id) {
        const PrintRegionConfig &config = this->printing_region(region_id).config();
        if (config.brick_layers.value && config.brick_layers_infill.value)
            enabled = true;
    }
    if (!enabled || m_layers.size() < 2 || m_print->config().spiral_mode.value || m_print->config().enable_mixed_color_sublayer.value)
        return;

    BOOST_LOG_TRIVIAL(debug) << "Brick infill - start";
    std::vector<BrickInfillLayer> bricks(m_layers.size());
    auto is_brick_region = [](const LayerRegion &layerm) {
        const PrintRegionConfig &config = layerm.region().config();
        return config.brick_layers.value && config.brick_layers_infill.value;
    };

    // 1. Zones de chaque couche (la 1re couche reste plate).
    tbb::parallel_for(tbb::blocked_range<size_t>(1, m_layers.size()), [this, &bricks, &is_brick_region](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_print->throw_if_canceled();
            const Layer      &layer = *m_layers[layer_idx];
            BrickInfillLayer &brick = bricks[layer_idx];

            // Numéro de la paroi la plus interne de chaque île...
            brick.innermost.assign(layer.lslices.size(), -1);
            std::vector<std::pair<int, Polylines>> walls;
            for_each_wall(layer, [&layer, &brick, &walls](const ExtrusionEntity &entity) {
                const int wall_index = wall_index_of(entity);
                if (wall_index < 0)
                    return;
                if (const int island = island_of(layer, entity.first_point()); island >= 0)
                    brick.innermost[island] = std::max(brick.innermost[island], wall_index);
                walls.emplace_back(wall_index, entity.as_polylines());
            });
            // ... et de chaque région à remplissage en briques : la plus haute des parois qui y passent. Orio
            // (parois continues) : un modificateur qui a plus ou moins de parois que la pièce a sa propre paroi la plus
            // interne, et son remplissage alterne avec elle, pas avec celle de la pièce.
            std::sort(walls.begin(), walls.end(), [](const std::pair<int, Polylines> &a, const std::pair<int, Polylines> &b) { return a.first > b.first; });
            for (const LayerRegion *layerm : layer.regions()) {
                if (!is_brick_region(*layerm) || layerm->slices.empty())
                    continue;
                BrickInfillRegion region;
                region.area.set(to_expolygons(layerm->slices.surfaces));
                const BoundingBox bbox = get_extents(region.area.expolygons);
                for (const std::pair<int, Polylines> &wall : walls)
                    if (get_extents(wall.second).overlap(bbox) &&
                        total_length(intersection_pl(wall.second, region.area.expolygons)) > scale_(MIN_PIECE_MM)) {
                        region.innermost = wall.first;
                        break;
                    }
                // Le creux monte là où la paroi la plus interne reste au Z de la couche.
                if (region.innermost >= 0 && region.innermost % 2 == 0)
                    append(brick.shifted_area, region.area.expolygons);
                brick.regions.emplace_back(std::move(region));
            }

            // Seuls les types de remplissage réellement imprimés à la hauteur de la couche comptent, là où ils passent : le
            // remplissage de régions aux mêmes réglages est tracé ensemble et rangé dans la première (Layer::make_fills),
            // une région peut donc ne pas en avoir à elle (modificateur qui ne change que les parois, par exemple).
            Points sparse_points;
            Points solid_points;
            for (const LayerRegion *layerm : layer.regions())
                if (is_brick_region(*layerm))
                    for (const ExtrusionEntity *fill : layerm->fills.entities)
                        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(fill))
                            for (const ExtrusionEntity *entity : collection->entities)
                                if (is_brick_infill(*entity, layer.height)) {
                                    Points   &points = entity->role() == erInternalInfill ? sparse_points : solid_points;
                                    Polylines lines;
                                    entity->collect_polylines(lines);
                                    for (const Polyline &line : lines)
                                        for (size_t i = 1; i < line.points.size(); ++i)
                                            points.emplace_back((line.points[i - 1] + line.points[i]) / 2);
                                }
            auto printed_in = [](const Points &points, const ExPolygon &expolygon) {
                const BoundingBox bbox = get_extents(expolygon);
                return std::any_of(points.begin(), points.end(), [&bbox, &expolygon](const Point &pt) { return bbox.contains(pt) && expolygon.contains(pt); });
            };
            ExPolygons sparse_surfaces;
            ExPolygons solid_surfaces;
            for (const LayerRegion *layerm : layer.regions()) {
                if (!is_brick_region(*layerm))
                    continue;
                for (const Surface &surface : layerm->fill_surfaces.surfaces)
                    if (surface.surface_type == stInternal && printed_in(sparse_points, surface.expolygon))
                        sparse_surfaces.emplace_back(surface.expolygon);
                    else if (surface.surface_type == stInternalSolid && printed_in(solid_points, surface.expolygon))
                        solid_surfaces.emplace_back(surface.expolygon);
            }
            if (!sparse_surfaces.empty() && !brick.shifted_area.empty())
                brick.sparse_area = intersection_ex(union_ex(sparse_surfaces), brick.shifted_area);
            if (!solid_surfaces.empty())
                brick.solid_area.set(union_ex(solid_surfaces));
        }
    });
    m_print->throw_if_canceled();

    // 2. Zones montées, et même hauteur de buse que les parois montées de la couche (apply_brick_layers).
    tbb::parallel_for(tbb::blocked_range<size_t>(1, m_layers.size()), [this, &bricks](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_print->throw_if_canceled();
            BrickInfillLayer &brick = bricks[layer_idx];
            const Layer      &layer = *m_layers[layer_idx];
            const bool        top   = layer_idx + 1 >= m_layers.size();
            brick.shift = 0.5 * std::min(layer.height, top ? layer.height : m_layers[layer_idx + 1]->height);
            if (top)
                continue;
            const BrickInfillLayer &upper     = bricks[layer_idx + 1];
            const float             tolerance = float(scale_(AREA_TOLERANCE_MM));
            if (!brick.sparse_area.empty() && !upper.sparse_area.empty())
                brick.up_sparse.set(intersection_ex(brick.sparse_area, offset_ex(upper.sparse_area, tolerance)));
            if (!brick.solid_area.empty() && !(upper.sparse_area.empty() && upper.solid_area.empty())) {
                ExPolygons upper_area = upper.sparse_area;
                append(upper_area, upper.solid_area.expolygons);
                brick.up_solid.set(intersection_ex(brick.solid_area.expolygons, offset_ex(upper_area, tolerance)));
                if (!brick.up_solid.empty() && !brick.shifted_area.empty())
                    brick.up_solid_block.set(intersection_ex(brick.up_solid.expolygons, brick.shifted_area));
            }
        }
    });
    m_print->throw_if_canceled();

    // 3. Hauteur de buse de chaque morceau de remplissage, et empreinte de tout ce qui est monté dans la couche (parois,
    //    zone du creux, cordons du plein) : la couche du dessus s'y pose plus haut.
    tbb::parallel_for(tbb::blocked_range<size_t>(1, m_layers.size()), [this, &bricks, &is_brick_region](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_print->throw_if_canceled();
            Layer            &layer = *m_layers[layer_idx];
            BrickInfillLayer &brick = bricks[layer_idx];
            Polygons          raised;
            if (!brick.up_sparse.empty() || !brick.up_solid.empty())
                for (LayerRegion *layerm : layer.regions())
                    if (is_brick_region(*layerm))
                        for (ExtrusionEntity *fill : layerm->fills.entities)
                            if (auto *collection = dynamic_cast<ExtrusionEntityCollection*>(fill))
                                raise_brick_infill(*collection, layer, brick, raised);
            for_each_wall(layer, [&raised](const ExtrusionEntity &entity) {
                if (z_shift_of(entity) > 0.f)
                    entity.polygons_covered_by_width(raised, float(SCALED_EPSILON));
            });
            append(raised, to_polygons(brick.up_sparse.expolygons));
            if (!raised.empty())
                brick.below.set(union_ex(raised));
        }
    });
    m_print->throw_if_canceled();

    // 4. Hauteur réelle (débit) de chaque morceau, selon ce qui est monté dans la couche du dessous.
    tbb::parallel_for(tbb::blocked_range<size_t>(1, m_layers.size()), [this, &bricks, &is_brick_region](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            m_print->throw_if_canceled();
            const BrickInfillLayer &brick = bricks[layer_idx];
            const BrickInfillLayer &lower = bricks[layer_idx - 1];
            if (lower.below.empty() && brick.up_sparse.empty() && brick.up_solid.empty())
                continue;
            Layer        &layer = *m_layers[layer_idx];
            InfillHeights heights;
            heights.height      = layer.height;
            heights.lower_shift = lower.shift;
            for (LayerRegion *layerm : layer.regions())
                if (is_brick_region(*layerm))
                    for (ExtrusionEntity *fill : layerm->fills.entities)
                        if (auto *collection = dynamic_cast<ExtrusionEntityCollection*>(fill))
                            set_brick_infill_heights(*collection, lower.below, heights);
        }
    });
    m_print->throw_if_canceled();
    BOOST_LOG_TRIVIAL(debug) << "Brick infill - end";
}

} // namespace Slic3r
