#include "FaceAnchors.hpp"

#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/libslic3r.h"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <string>

namespace Slic3r {
namespace GUI {
namespace FaceAnchors {

// Fractions (numérateur, dénominateur) proposées comme ancrages : grille longueur × largeur du
// rectangle de l'îlot, et subdivisions plus fines le long de ses médianes et de ses diagonales.
static const Vec2i GRID_FRACTIONS[] = {{1, 4}, {1, 3}, {1, 2}, {2, 3}, {3, 4}};
static const Vec2i LINE_FRACTIONS[] = {{1, 10}, {1, 8}, {1, 6}, {1, 5}, {1, 4}, {3, 10}, {1, 3}, {3, 8}, {2, 5}, {1, 2},
                                       {3, 5},  {5, 8}, {2, 3}, {7, 10}, {3, 4}, {4, 5}, {5, 6}, {7, 8}, {9, 10}};

static double      fraction_value(const Vec2i &f) { return double(f.x()) / double(f.y()); }
static std::string fraction_text(const Vec2i &f) { return std::to_string(f.x()) + "/" + std::to_string(f.y()); }

static double island_edge_distance(const Island &isl, const Vec2d &point)
{
    double d2 = std::numeric_limits<double>::max();
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

int island_at(const Face &face, const Vec2d &point)
{
    const Point p = Point::new_scale(point);
    for (int i = 0; i < int(face.islands.size()); ++i)
        if (face.islands[i].expoly.contains(p))
            return i;
    return -1;
}

double edge_distance(const Face &face, int island, const Vec2d &point)
{
    if (island < 0 || island >= int(face.islands.size()))
        return -1.;
    return island_edge_distance(face.islands[island], point);
}

Face compute(const ExPolygons &expolys)
{
    Face face;
    for (const ExPolygon &expoly : expolys) {
        Island island;
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
        // côté). À aire presque égale (cercle, polygone régulier), le plus proche de l'axe X.
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
        island.axis_u  = u;
        island.axis_v  = Vec2d(-u.y(), u.x());
        island.box_min = Vec2d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
        island.box_max = -island.box_min;
        for (const Vec2d &p : hull) {
            const Vec2d q((p - island.centroid).dot(island.axis_u), (p - island.centroid).dot(island.axis_v));
            island.box_min = island.box_min.cwiseMin(q);
            island.box_max = island.box_max.cwiseMax(q);
        }
        face.islands.push_back(std::move(island));
    }
    // le plus grand îlot d'abord
    std::sort(face.islands.begin(), face.islands.end(), [](const Island &a, const Island &b) { return a.area > b.area; });

    for (int idx = 0; idx < int(face.islands.size()); ++idx) {
        Island &island = face.islands[idx];

        // point le plus éloigné des bords (pôle d'inaccessibilité, recherche par cellules)
        {
            struct Cell { Vec2d c; double h, d, max; };
            auto make_cell = [&island](const Vec2d &c, double h) {
                const double d = island_edge_distance(island, c);
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

        // ancrages, sans doublon (moins de 0,05 mm d'écart) et seulement dans la matière (sauf les
        // centres de trous)
        auto add_anchor = [&face, &island, idx](const Vec2d &p, AnchorKind kind, const Vec2i &frac_a, const Vec2i &frac_b, bool need_material) {
            if (need_material && island_edge_distance(island, p) <= 0.)
                return;
            for (const Anchor &a : face.anchors)
                if ((a.pos - p).squaredNorm() < 0.05 * 0.05)
                    return;
            face.anchors.push_back({p, kind, idx, frac_a, frac_b});
        };
        const Vec2i none(0, 1);
        // point du rectangle de l'îlot : fraction fu de la longueur, fv de la largeur
        auto at_box = [&island](double fu, double fv) {
            const double u = island.box_min.x() + fu * (island.box_max.x() - island.box_min.x());
            const double v = island.box_min.y() + fv * (island.box_max.y() - island.box_min.y());
            return Vec2d(island.centroid + u * island.axis_u + v * island.axis_v);
        };
        add_anchor(island.centroid, AnchorKind::Centroid, none, none, true);
        if (island.pole_radius > 0.)
            add_anchor(island.pole, AnchorKind::Pole, none, none, true);
        add_anchor(at_box(0.5, 0.5), AnchorKind::BoxCenter, none, none, true);

        // foyers, quand le contour est une ellipse : ellipse de mêmes moments d'ordre 2 que le
        // contour plein (demi-axes 2·√λ), gardée si chaque sommet du contour en est à moins de 8 %
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
                        add_anchor(c0 + focal * axis_major, AnchorKind::Focus, none, none, true);
                        add_anchor(c0 - focal * axis_major, AnchorKind::Focus, none, none, true);
                    }
                }
            }
        }
        for (const Vec2d &c : island.hole_centers)
            add_anchor(c, AnchorKind::HoleCenter, none, none, false);

        // subdivisions des médianes (longueur, largeur) et des diagonales du rectangle de l'îlot
        for (const Vec2i &f : LINE_FRACTIONS) {
            const double t = fraction_value(f);
            add_anchor(at_box(t, 0.5), AnchorKind::LengthMedian, f, none, true);
            add_anchor(at_box(0.5, t), AnchorKind::WidthMedian, f, none, true);
        }
        for (const Vec2i &f : LINE_FRACTIONS) {
            const double t = fraction_value(f);
            add_anchor(at_box(t, t), AnchorKind::Diagonal, f, none, true);
            add_anchor(at_box(t, 1. - t), AnchorKind::Diagonal, f, none, true);
        }
        // grille des quarts, tiers et moitiés de la longueur × largeur
        for (const Vec2i &fu : GRID_FRACTIONS)
            for (const Vec2i &fv : GRID_FRACTIONS)
                add_anchor(at_box(fraction_value(fu), fraction_value(fv)), AnchorKind::Fraction, fu, fv, true);
    }
    return face;
}

int snap(const Face &face, const Vec2d &point, double tolerance)
{
    int    best      = -1;
    int    best_rank = std::numeric_limits<int>::max();
    double best_dist = std::numeric_limits<double>::max();
    for (int i = 0; i < int(face.anchors.size()); ++i) {
        const Anchor &anchor = face.anchors[i];
        const double  dist   = (anchor.pos - point).norm();
        const int     rank   = anchor.kind >= AnchorKind::Fraction ? 1 : 0; // subdivisions après les points remarquables
        if (dist > tolerance || rank > best_rank || (rank == best_rank && dist >= best_dist))
            continue;
        best      = i;
        best_rank = rank;
        best_dist = dist;
    }
    return best;
}

wxString anchor_name(const Anchor &anchor)
{
    switch (anchor.kind) {
    case AnchorKind::Centroid: return _L("Centroid of the face");
    case AnchorKind::Pole: return _L("Farthest point from the edges");
    case AnchorKind::BoxCenter: return _L("Center of the face box");
    case AnchorKind::HoleCenter: return _L("Center of a hole");
    case AnchorKind::Focus: return _L("Focus of the ellipse");
    case AnchorKind::LengthMedian: return format_wxstr(_L("%1% of the length median"), fraction_text(anchor.frac_a));
    case AnchorKind::WidthMedian: return format_wxstr(_L("%1% of the width median"), fraction_text(anchor.frac_a));
    case AnchorKind::Diagonal: return format_wxstr(_L("%1% of a diagonal"), fraction_text(anchor.frac_a));
    default:
        return format_wxstr(_L("%1% of the length, %2% of the width"), fraction_text(anchor.frac_a), fraction_text(anchor.frac_b));
    }
}

} // namespace FaceAnchors
} // namespace GUI
} // namespace Slic3r
