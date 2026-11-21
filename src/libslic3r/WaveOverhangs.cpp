///|/ Wave overhang generation.
///|/
///|/ Wave overhangs algorithm: Janis A. Andersons (andersonsjanis).
///|/ Builds on arc-overhang algorithm by Steven McCulloch (stmcculloch).
///|/ PrusaSlicer integration: Steven McCulloch.
///|/ Port to OrcaSlicer: Dennis Klappe (dennisklappe).
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/ Ported to Orio from Unlayered Slicer v0.17.
///|/
#include "WaveOverhangs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

//Orio: RegionExpansion lives flat in src/libslic3r in Orio (Algorithm/RegionExpansion.hpp upstream).
#include "RegionExpansion.hpp"
#include "BoundingBox.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExPolygon.hpp"
#include "Geometry/ConvexHull.hpp"
#include "Line.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "libslic3r.h"

namespace Slic3r { namespace WaveOverhangs {
namespace {

Polylines reconnect_polylines(const Polylines &polylines, double limit_distance)
{
    if (polylines.empty())
        return polylines;

    std::unordered_map<size_t, Polyline> connected;
    connected.reserve(polylines.size());
    for (size_t i = 0; i < polylines.size(); ++i) {
        if (! polylines[i].empty())
            connected.emplace(i, polylines[i]);
    }

    for (size_t a = 0; a < polylines.size(); ++a) {
        auto base_it = connected.find(a);
        if (base_it == connected.end())
            continue;

        Polyline &base = base_it->second;
        for (size_t b = a + 1; b < polylines.size(); ++b) {
            auto next_it = connected.find(b);
            if (next_it == connected.end())
                continue;

            Polyline &next = next_it->second;
            if ((base.last_point() - next.first_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.append(std::move(next));
                connected.erase(next_it);
            } else if ((base.last_point() - next.last_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.points.insert(base.points.end(), next.points.rbegin(), next.points.rend());
                connected.erase(next_it);
            } else if ((base.first_point() - next.last_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                next.append(std::move(base));
                base = std::move(next);
                base.reverse();
                connected.erase(next_it);
            } else if ((base.first_point() - next.first_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.reverse();
                base.append(std::move(next));
                base.reverse();
                connected.erase(next_it);
            }
        }
    }

    Polylines result;
    result.reserve(connected.size());
    for (auto &entry : connected)
        result.push_back(std::move(entry.second));
    return result;
}

template <class Fn>
void for_each_boundary_point(const ExPolygon &expoly, Fn &&fn)
{
    for (const Point &pt : expoly.contour.points)
        fn(pt);
    for (const Polygon &hole : expoly.holes)
        for (const Point &pt : hole.points)
            fn(pt);
}

//Orio: corner taper (detect_overhang_corners, build_corner_influence) not ported, its options are dropped.

struct ClosestBoundaryPair {
    Point  a;
    Point  b;
    double distance_sq{ std::numeric_limits<double>::infinity() };
    bool   valid{ false };
};

struct NarrowSplitCandidate {
    Point   a;
    Point   b;
    Point   midpoint;
    double  distance_sq{ std::numeric_limits<double>::infinity() };
    Polygon slit;
};

ClosestBoundaryPair find_closest_boundary_pair(const ExPolygon &a, const ExPolygon &b, const ExPolygon &container)
{
    ClosestBoundaryPair best;

    auto try_pair = [&](const Point &src, const ExPolygon &other, bool src_is_a) {
        Point projected = other.point_projection(src);
        const double distance_sq = (projected - src).cast<double>().squaredNorm();
        if (distance_sq >= best.distance_sq)
            return;

        const Point midpoint = (0.5 * (src.cast<double>() + projected.cast<double>())).cast<coord_t>();
        if (! container.contains(midpoint))
            return;

        best.distance_sq = distance_sq;
        best.valid = true;
        if (src_is_a) {
            best.a = src;
            best.b = projected;
        } else {
            best.a = projected;
            best.b = src;
        }
    };

    for_each_boundary_point(a, [&](const Point &pt) { try_pair(pt, b, true); });
    for_each_boundary_point(b, [&](const Point &pt) { try_pair(pt, a, false); });

    return best;
}

Polygon make_split_slit(const Point &a, const Point &b, coord_t extension, coord_t half_width)
{
    const Vec2d start = a.cast<double>();
    const Vec2d end   = b.cast<double>();
    const Vec2d delta = end - start;
    const double length = delta.norm();
    if (length <= 0.)
        return {};

    const Vec2d dir = delta / length;
    const Vec2d normal(-dir.y(), dir.x());
    const Vec2d extended_start = start - dir * double(extension);
    const Vec2d extended_end   = end + dir * double(extension);
    const Vec2d offset         = normal * double(std::max<coord_t>(1, half_width));

    Polygon slit;
    slit.points = {
        Point((extended_start + offset).cast<coord_t>()),
        Point((extended_end   + offset).cast<coord_t>()),
        Point((extended_end   - offset).cast<coord_t>()),
        Point((extended_start - offset).cast<coord_t>())
    };
    return slit;
}

size_t total_hole_count(const ExPolygons &expolygons)
{
    size_t count = 0;
    for (const ExPolygon &expolygon : expolygons)
        count += expolygon.holes.size();
    return count;
}

bool slit_changes_topology(const ExPolygon &wave_cover, const Polygon &slit)
{
    if (! slit.is_valid())
        return false;

    const ExPolygons split_result = union_ex(diff_ex(ExPolygons{ wave_cover }, Polygons{ slit }));
    return split_result.size() != 1 || total_hole_count(split_result) != wave_cover.holes.size();
}

Polygon make_effective_split_slit(const ExPolygon &wave_cover, const Point &a, const Point &b, coord_t extension, coord_t initial_half_width, coord_t wave_spacing)
{
    coord_t half_width = std::max<coord_t>(std::max<coord_t>(1, initial_half_width), wave_spacing / 2 + 1);
    for (int attempt = 0; attempt < 6; ++attempt) {
        Polygon slit = make_split_slit(a, b, extension, half_width);
        if (slit_changes_topology(wave_cover, slit))
            return slit;

        half_width = std::max<coord_t>(half_width + 1, half_width * 2);
    }

    return {};
}

Polygons generate_narrow_split_slits(const ExPolygon &wave_cover, coord_t wave_spacing, coord_t minimum_wave_width)
{
    const coord_t effective_minimum_width = std::max<coord_t>(0, minimum_wave_width);
    if (effective_minimum_width <= 0)
        return {};

    const double max_gap_sq = std::pow(double(effective_minimum_width), 2);
    const coord_t slit_half_width = std::max<coord_t>(1, wave_spacing / 20);
    const coord_t slit_extension  = std::max<coord_t>(slit_half_width, effective_minimum_width);
    const std::array<double, 4> inset_fractions{{ 0.25, 0.5, 0.75, 1.0 }};
    const double duplicate_radius_sq = std::pow(0.5 * double(wave_spacing), 2);
    const size_t original_hole_count = wave_cover.holes.size();

    std::vector<NarrowSplitCandidate> candidates;
    auto append_candidate = [&](const ClosestBoundaryPair &pair) {
        if (! pair.valid || pair.distance_sq > max_gap_sq)
            return;

        NarrowSplitCandidate candidate;
        candidate.a = pair.a;
        candidate.b = pair.b;
        candidate.distance_sq = pair.distance_sq;
        candidate.midpoint = (0.5 * (pair.a.cast<double>() + pair.b.cast<double>())).cast<coord_t>();
        candidate.slit = make_effective_split_slit(wave_cover, pair.a, pair.b, wave_spacing + slit_extension, slit_half_width, wave_spacing);
        if (! candidate.slit.is_valid())
            return;

        candidates.push_back(std::move(candidate));
    };

    for (double inset_fraction : inset_fractions) {
        const coord_t inset_depth = std::max<coord_t>(1, coord_t(std::round(inset_fraction * double(wave_spacing))));
        const ExPolygons inset_components = offset_ex(wave_cover, -float(inset_depth), jtRound, 0.);
        const bool component_count_changed = inset_components.size() > 1;
        const bool hole_count_changed = total_hole_count(inset_components) != original_hole_count;
        if (! component_count_changed && ! hole_count_changed)
            continue;

        if (component_count_changed) {
            for (size_t i = 0; i < inset_components.size(); ++i) {
                for (size_t j = i + 1; j < inset_components.size(); ++j) {
                    ClosestBoundaryPair pair = find_closest_boundary_pair(inset_components[i], inset_components[j], wave_cover);
                    append_candidate(pair);
                }
            }
        }

        if (hole_count_changed && ! wave_cover.holes.empty()) {
            ExPolygon outer_boundary;
            outer_boundary.contour = wave_cover.contour;

            for (size_t hole_idx = 0; hole_idx < wave_cover.holes.size(); ++hole_idx) {
                ExPolygon hole_boundary;
                hole_boundary.contour = wave_cover.holes[hole_idx];
                append_candidate(find_closest_boundary_pair(outer_boundary, hole_boundary, wave_cover));

                for (size_t other_hole_idx = hole_idx + 1; other_hole_idx < wave_cover.holes.size(); ++other_hole_idx) {
                    ExPolygon other_hole_boundary;
                    other_hole_boundary.contour = wave_cover.holes[other_hole_idx];
                    append_candidate(find_closest_boundary_pair(hole_boundary, other_hole_boundary, wave_cover));
                }
            }
        }
    }

    if (candidates.empty())
        return {};

    std::sort(candidates.begin(), candidates.end(), [](const NarrowSplitCandidate &lhs, const NarrowSplitCandidate &rhs) {
        return lhs.distance_sq < rhs.distance_sq;
    });

    Polygons slits;
    std::vector<Point> kept_midpoints;
    for (NarrowSplitCandidate &candidate : candidates) {
        bool duplicate = false;
        for (const Point &kept_midpoint : kept_midpoints) {
            if ((candidate.midpoint - kept_midpoint).cast<double>().squaredNorm() <= duplicate_radius_sq) {
                duplicate = true;
                break;
            }
        }

        if (duplicate)
            continue;

        kept_midpoints.push_back(candidate.midpoint);
        slits.push_back(std::move(candidate.slit));
    }

    return slits.empty() ? Polygons{} : union_(slits);
}

Polylines generate_wave_overhang_seeds(const ExPolygon &boundary, const Polygons &anchoring, const coord_t seed_expansion)
{
    if (anchoring.empty())
        return {};

    Polylines seeds;
    for (const Algorithm::WaveSeed &seed : Algorithm::wave_seeds(to_expolygons(anchoring), ExPolygons{ boundary }, float(seed_expansion), true)) {
        if (seed.boundary == 0 && seed.path.size() >= 2)
            seeds.emplace_back(seed.path);
    }

    if (seeds.empty())
        seeds = intersection_pl(to_polylines(boundary), offset(anchoring, float(seed_expansion), jtRound, 0.));

    return seeds;
}

// When false (default), simple bridge-friendly spans are left alone so the
// slicer's normal bridge pipeline handles them. When true, every detected
// overhang gets wave treatment regardless of bridgeability.
bool should_generate_waves_for_region(const Polygons  &overhang_to_cover,
                                      const ExPolygon &overhang_region,
                                      const Polygons  &real_overhang,
                                      const Polygons  &anchors,
                                      const Polygons  &inset_anchors,
                                      const Flow      &overhang_flow,
                                      bool             use_instead_of_bridges)
{
    if (real_overhang.empty())
        return false;

    if (use_instead_of_bridges)
        return true;

    if (! overhang_region.holes.empty())
        return true;

    const Polygons anchoring = intersection(expand(overhang_to_cover, 1.1 * overhang_flow.scaled_spacing(), jtRound, 0.), inset_anchors);
    const Polygon  anchoring_convex_hull = Geometry::convex_hull(anchoring);
    const double   unbridgeable_area = area(diff(real_overhang, Polygons{ anchoring_convex_hull }));
    const double   unsupported_dist = std::get<1>(detect_bridging_direction(real_overhang, anchors));

    // Prefer the slicer's regular bridge handling when a viable bridge
    // direction exists and the span is mostly convex/covered by anchors.
    return unbridgeable_area >= 0.2 * area(real_overhang) ||
           unsupported_dist >= total_length(real_overhang) * 0.2;
}

void tag_wave_overhang_paths(std::vector<ExtrusionPaths> &wave_paths)
{
    for (ExtrusionPaths &region : wave_paths)
        for (ExtrusionPath &path : region)
            path.wave_overhang = true;
}

void append_shell_perimeters(ExtrusionPaths &overhang_region,
                             const Polygons &overhang_to_cover,
                             int             outer_perimeter_count,
                             coord_t         perimeter_spacing,
                             const Flow     &perimeter_flow,
                             double          scaled_resolution)
{
    if (outer_perimeter_count <= 0)
        return;

    Polygons shell_centerline = shrink(overhang_to_cover, std::max<coord_t>(1, perimeter_flow.scaled_width() / 2), jtRound, 0.);
    for (int i = 0; i < outer_perimeter_count && ! shell_centerline.empty(); ++i) {
        Polylines shell_loops = to_polylines(shell_centerline);
        for (Polyline &loop : shell_loops)
            loop.simplify(std::min(0.05 * perimeter_spacing, scaled_resolution));
        shell_loops.erase(
            std::remove_if(shell_loops.begin(), shell_loops.end(), [](const Polyline &loop) { return loop.points.size() < 2; }),
            shell_loops.end());

        if (! shell_loops.empty())
            extrusion_paths_append(overhang_region, shell_loops, erOverhangPerimeter,
                                   perimeter_flow.mm3_per_mm(), perimeter_flow.width(), perimeter_flow.height());

        shell_centerline = shrink(shell_centerline, perimeter_spacing, jtRound, 0.);
    }
}

//Orio: ExtrusionPath::polyline is a 2D Polyline in Orio (Polyline3 in Unlayered): one overload is enough.
ExtrusionPath make_wave_path(Polyline polyline, const Flow &flow)
{
    ExtrusionPath path(erOverhangPerimeter, flow.mm3_per_mm(), flow.width(), flow.height());
    path.polyline = std::move(polyline);
    return path;
}

void append_wave_fronts(ExtrusionPaths     &overhang_region,
                        const Polylines    &fronts,
                        const Flow         &wave_flow,
                        coord_t             connector_limit,
                        WaveOverhangPattern wave_pattern)
{
    if (fronts.empty())
        return;

    if (wave_pattern == WaveOverhangPattern::Monotonic) {
        Polylines monotonic_fronts = fronts;
        extrusion_paths_append(overhang_region, monotonic_fronts, erOverhangPerimeter,
                               wave_flow.mm3_per_mm(), wave_flow.width(), wave_flow.height());
        return;
    }

    if (wave_pattern == WaveOverhangPattern::ZigZag) {
        Polylines merged;
        merged.reserve(fronts.size());
        for (const Polyline &source_front : fronts) {
            Polyline front = source_front;
            if (front.points.size() < 2)
                continue;

            if (merged.empty()) {
                merged.emplace_back(std::move(front));
                continue;
            }

            Polyline &current = merged.back();
            const double d_keep = (current.last_point() - front.first_point()).cast<double>().norm();
            const double d_flip = (current.last_point() - front.last_point()).cast<double>().norm();
            const double best_d = std::min(d_keep, d_flip);

            if (best_d > connector_limit) {
                merged.emplace_back(std::move(front));
                continue;
            }

            if (d_flip < d_keep)
                front.reverse();
            //Orio: Orio's Polyline::append() skips a first point equal to the current last point.
            current.append(std::move(front));
        }

        extrusion_paths_append(overhang_region, merged, erOverhangPerimeter,
                               wave_flow.mm3_per_mm(), wave_flow.width(), wave_flow.height());
        return;
    }

    auto point_at_distance = [](const Polyline &line, double distance) {
        if (line.points.empty())
            return Point{};
        if (distance <= 0. || line.points.size() == 1)
            return line.first_point();

        double walked = 0.;
        for (size_t i = 1; i < line.points.size(); ++i) {
            const Vec2d a = line.points[i - 1].cast<double>();
            const Vec2d b = line.points[i].cast<double>();
            const Vec2d segment = b - a;
            const double segment_length = segment.norm();
            if (segment_length <= 0.)
                continue;
            if (walked + segment_length >= distance) {
                const double t = (distance - walked) / segment_length;
                return Point((a + t * segment).cast<coord_t>());
            }
            walked += segment_length;
        }
        return line.last_point();
    };

    auto support_score = [&point_at_distance](const Polyline &candidate, const Polylines &support_paths, coord_t support_reach, coord_t prefix_length) {
        if (support_paths.empty() || candidate.points.size() < 2)
            return -1.;

        const double candidate_length = candidate.length();
        if (candidate_length <= 0.)
            return -1.;

        const double sample_length = std::min(candidate_length, double(std::max<coord_t>(1, prefix_length)));
        const std::array<std::pair<double, double>, 3> samples = {{
            { 0.0,                 3.0 },
            { 0.5 * sample_length, 2.0 },
            { sample_length,       1.0 }
        }};

        double best_score = -1.;
        for (auto it = support_paths.rbegin(); it != support_paths.rend(); ++it) {
            if (it->points.size() < 2)
                continue;

            double score = 0.;
            for (const auto &[distance_along, weight] : samples) {
                Point sample = point_at_distance(candidate, distance_along);
                std::pair<int, Point> foot = foot_pt(it->points, sample);
                int seg_idx = foot.first;
                if (seg_idx < 0 || size_t(seg_idx + 1) >= it->points.size())
                    continue;

                const Point &a = it->points[size_t(seg_idx)];
                const Point &b = it->points[size_t(seg_idx + 1)];
                const bool interior_projection = foot.second != a && foot.second != b;
                const double distance_to_support = (sample - foot.second).cast<double>().norm();
                const double normalized_support = std::max(0.0, 1.0 - distance_to_support / double(std::max<coord_t>(1, support_reach)));

                score += weight * (3.0 * normalized_support + (interior_projection ? 1.5 : 0.2));
            }

            best_score = std::max(best_score, score);
        }

        return best_score;
    };

    Polylines support_paths;
    support_paths.reserve(overhang_region.size() + fronts.size());
    for (const ExtrusionPath &path : overhang_region)
        support_paths.emplace_back(path.polyline);
    const coord_t support_reach = std::max<coord_t>(wave_flow.scaled_width(), connector_limit);
    const coord_t prefix_length = std::max<coord_t>(wave_flow.scaled_width(), connector_limit / 2);

    for (const Polyline &source_front : fronts) {
        Polyline front = source_front;
        if (front.points.size() < 2)
            continue;

        Polyline reversed = front;
        reversed.reverse();
        const double forward_score = support_score(front, support_paths, support_reach, prefix_length);
        const double reverse_score = support_score(reversed, support_paths, support_reach, prefix_length);
        if (reverse_score > forward_score)
            front.reverse();

        overhang_region.push_back(make_wave_path(front, wave_flow));
        support_paths.push_back(std::move(front));
    }
}

void append_zig_zag_front_levels(ExtrusionPaths               &overhang_region,
                                 const std::vector<Polylines> &front_levels,
                                 const Flow                   &wave_flow,
                                 coord_t                       connector_limit)
{
    if (front_levels.empty())
        return;

    std::vector<std::vector<bool>> used;
    used.reserve(front_levels.size());
    for (const Polylines &level : front_levels)
        used.emplace_back(level.size(), false);

    const double max_connector_distance_sq = double(connector_limit) * double(connector_limit);

    auto append_or_start = [&](Polyline &&front) {
        if (overhang_region.empty()) {
            overhang_region.push_back(make_wave_path(std::move(front), wave_flow));
            return;
        }

        ExtrusionPath &current = overhang_region.back();
        const double d_keep = (current.last_point() - front.first_point()).cast<double>().squaredNorm();
        const double d_flip = (current.last_point() - front.last_point()).cast<double>().squaredNorm();
        const double best_d = std::min(d_keep, d_flip);

        if (best_d > max_connector_distance_sq) {
            overhang_region.push_back(make_wave_path(std::move(front), wave_flow));
            return;
        }

        if (d_flip < d_keep)
            front.reverse();
        //Orio: Orio's Polyline::append() skips a first point equal to the current last point.
        current.polyline.append(std::move(front));
    };

    std::function<void(size_t, size_t, bool)> follow_branch = [&](size_t level_idx, size_t front_idx, bool reverse_front) {
        used[level_idx][front_idx] = true;
        Polyline current = front_levels[level_idx][front_idx];
        if (current.points.size() < 2)
            return;
        if (reverse_front)
            current.reverse();

        append_or_start(std::move(current));

        for (size_t next_level = level_idx + 1; next_level < front_levels.size(); ++next_level) {
            size_t best_idx = size_t(-1);
            bool   reverse_child = false;
            double best_d = max_connector_distance_sq;

            const Point anchor = overhang_region.back().last_point();
            for (size_t candidate_idx = 0; candidate_idx < front_levels[next_level].size(); ++candidate_idx) {
                if (used[next_level][candidate_idx])
                    continue;

                const Polyline &candidate = front_levels[next_level][candidate_idx];
                if (candidate.points.size() < 2)
                    continue;

                const double d_keep = (anchor - candidate.first_point()).cast<double>().squaredNorm();
                if (d_keep <= best_d) {
                    best_d = d_keep;
                    best_idx = candidate_idx;
                    reverse_child = false;
                }

                const double d_flip = (anchor - candidate.last_point()).cast<double>().squaredNorm();
                if (d_flip <= best_d) {
                    best_d = d_flip;
                    best_idx = candidate_idx;
                    reverse_child = true;
                }
            }

            if (best_idx == size_t(-1) || best_d > max_connector_distance_sq)
                break;

            follow_branch(next_level, best_idx, reverse_child);
            return;
        }
    };

    for (size_t level_idx = 0; level_idx < front_levels.size(); ++level_idx) {
        for (size_t front_idx = 0; front_idx < front_levels[level_idx].size(); ++front_idx) {
            if (! used[level_idx][front_idx])
                follow_branch(level_idx, front_idx, false);
        }
    }
}

//Orio: arc overhangs helpers.
Polygon make_disc(const Point &center, coord_t radius)
{
    constexpr int segments = 128;
    Polygon disc;
    disc.points.reserve(segments);
    for (int i = 0; i < segments; ++i) {
        const double angle = 2. * PI * double(i) / double(segments);
        disc.points.emplace_back(double(center.x()) + double(radius) * std::cos(angle), double(center.y()) + double(radius) * std::sin(angle));
    }
    return disc;
}

Point point_at_half_length(const Polyline &polyline)
{
    double remaining = 0.5 * polyline.length();
    for (size_t i = 1; i < polyline.points.size(); ++i) {
        const Vec2d  a   = polyline.points[i - 1].cast<double>();
        const Vec2d  b   = polyline.points[i].cast<double>();
        const double len = (b - a).norm();
        if (len > 0. && len >= remaining)
            return Point(a.x() + (b.x() - a.x()) * remaining / len, a.y() + (b.y() - a.y()) * remaining / len);
        remaining -= len;
    }
    return polyline.points.back();
}

} // namespace

std::tuple<std::vector<ExtrusionPaths>, Polygons> generate(
    ExPolygons          infill_area,
    const Polygons     &lower_slices_polygons,
    int                 perimeter_count,
    int                 additional_shell_count,
    double              wave_perimeter_overlap,
    double              minimum_wave_width,
    WaveOverhangPattern wave_pattern,
    double              wave_line_spacing,
    double              wave_line_width,
    const Flow         &overhang_flow,
    double              scaled_resolution,
    int                 max_iterations,
    double              min_new_area_mm2,
    bool                use_instead_of_bridges,
    double              min_length_mm,
    WaveOverhangShape   wave_shape,
    double              arc_max_radius_mm)
{
    //Orio: min_length filter of Unlayered's AndersonsGenerator, folded in (applied first).
    if (min_length_mm > 0.) {
        const double min_length = scale_(min_length_mm);
        infill_area.erase(std::remove_if(infill_area.begin(), infill_area.end(),
                                         [min_length](const ExPolygon &ex) { return ex.contour.length() < min_length; }),
                          infill_area.end());
    }
    if (infill_area.empty())
        return {};

    const coord_t base_spacing       = overhang_flow.scaled_spacing();
    const Flow    wave_flow          = wave_line_width > 0. ? overhang_flow.with_width(float(wave_line_width)) : overhang_flow;
    const coord_t perimeter_overlap  = std::max<coord_t>(0, wave_perimeter_overlap > 0. ? coord_t(scale_(wave_perimeter_overlap)) : 0);
    const coord_t wave_spacing       = std::max<coord_t>(1, wave_line_spacing > 0. ? coord_t(scale_(wave_line_spacing)) : base_spacing);
    const coord_t min_wave_width     = std::max<coord_t>(0, minimum_wave_width > 0. ? coord_t(scale_(minimum_wave_width)) : 0);
    const coord_t anchors_size       = std::min(coord_t(scale_(EXTERNAL_INFILL_MARGIN)), base_spacing * (perimeter_count + 1));
    const coord_t seed_expansion     = std::max<coord_t>(1, base_spacing / 10);
    const coord_t shell_inner_edge   = additional_shell_count > 0 ? overhang_flow.scaled_width() + (additional_shell_count - 1) * base_spacing : 0;
    const coord_t filled_area_regularization = std::max<coord_t>(1, base_spacing / 2);
    const coord_t zig_zag_connector_limit = std::max<coord_t>(wave_spacing, wave_flow.scaled_width()) + perimeter_overlap;
    // Map min_new_area (mm^2) into Clipper's scaled area units. Fall back to the
    // legacy 0.05 * spacing^2 heuristic when the user leaves it at 0.
    const double  min_area_growth    = min_new_area_mm2 > 0.
                                       ? scale_(1.) * scale_(1.) * min_new_area_mm2
                                       : 0.05 * double(wave_spacing) * double(wave_spacing);

    BoundingBox infill_area_bb       = get_extents(infill_area).inflated(SCALED_EPSILON);
    Polygons    optimized_lower      = ClipperUtils::clip_clipper_polygons_with_subject_bbox(lower_slices_polygons, infill_area_bb);
    Polygons    overhangs            = diff(infill_area, optimized_lower);

    if (overhangs.empty())
        return {};

    Polygons anchors             = intersection(infill_area, optimized_lower);
    Polygons inset_anchors       = diff(anchors, expand(overhangs, anchors_size + 0.1 * overhang_flow.scaled_width(), ClipperLib::jtSquare, 0.));
    Polygons inset_overhang_area = diff(infill_area, inset_anchors);
    //Orio: the overlap only reaches over walls standing on the layer below; past the outermost walls it would
    // hang out of the part.
    const Polygons overlap_limit = union_(to_polygons(infill_area), optimized_lower);

    //Orio: arcs only on the path without additional shells (the only one Orio uses).
    const bool    arcs           = wave_shape == WaveOverhangShape::Arcs && additional_shell_count == 0;
    const coord_t arc_max_radius = arc_max_radius_mm > 0. ? coord_t(scale_(arc_max_radius_mm)) : 0;

    auto trim_of = [&](const Polygons &cover) {
        Polygons trim = shrink(cover, std::max<coord_t>(1, wave_flow.scaled_width() / 2), jtRound, 0.);
        if (trim.empty())
            trim = shrink(cover, 0.1 * base_spacing);
        return trim.empty() ? cover : trim;
    };

    // Grows `region` inside `cover` by geodesic offsets of wave_spacing, one level of fronts per step.
    auto grow_fronts = [&](Polygons &region, const Polygons &cover, const Polygons &trim) {
        std::vector<Polylines> front_levels;
        double region_area = area(region);
        //Orio: corner-taper sub-steps (intercalated corner fronts) not ported.
        for (int iteration = 0; max_iterations <= 0 || iteration < max_iterations; ++iteration) {
            Polygons next_region = intersection(offset(region, float(wave_spacing), jtRound, 0.), cover);
            if (next_region.empty())
                break;

            double next_area = area(next_region);
            if (next_area <= region_area + min_area_growth)
                break;

            Polylines fronts = intersection_pl(to_polylines(next_region), trim);
            for (Polyline &front : fronts)
                front.simplify(std::min(0.05 * wave_spacing, scaled_resolution));
            fronts.erase(
                std::remove_if(fronts.begin(), fronts.end(), [](const Polyline &front) { return front.points.size() < 2; }),
                fronts.end());
            fronts = reconnect_polylines(fronts, wave_spacing);

            if (! fronts.empty())
                front_levels.emplace_back(std::move(fronts));

            region      = std::move(next_region);
            region_area = next_area;
        }
        return front_levels;
    };

    std::vector<ExtrusionPaths> wave_paths;
    Polygons                    filled_area;

    for (const ExPolygon &overhang : union_ex(to_expolygons(inset_overhang_area))) {
        Polygons overhang_to_cover = to_polygons(overhang);
        Polygons wave_cover_area   = additional_shell_count > 0 ?
            shrink(overhang_to_cover, std::max<coord_t>(0, shell_inner_edge - perimeter_overlap), jtRound, 0.) :
            intersection(expand(overhang_to_cover, perimeter_overlap, jtRound, 0.), overlap_limit);

        Polygons real_overhang     = intersection(wave_cover_area, overhangs);
        if (real_overhang.empty())
            wave_cover_area.clear();
        else if (! should_generate_waves_for_region(wave_cover_area, overhang, real_overhang, anchors, inset_anchors, overhang_flow, use_instead_of_bridges))
            wave_cover_area.clear();

        ExtrusionPaths &overhang_region = wave_paths.emplace_back();
        Polygons        filled_overhang_region;

        // Turns the front levels grown into `region` into paths; false when nothing is printed.
        auto append_fronts = [&](const std::vector<Polylines> &front_levels, const Polygons &region) {
            if (front_levels.empty())
                return false;
            ExtrusionPaths split_region_paths;
            if (wave_pattern == WaveOverhangPattern::ZigZag) {
                append_zig_zag_front_levels(split_region_paths, front_levels, wave_flow, zig_zag_connector_limit);
            } else {
                Polylines collected_fronts;
                for (const Polylines &level : front_levels)
                    collected_fronts.insert(collected_fronts.end(), level.begin(), level.end());
                append_wave_fronts(split_region_paths, collected_fronts, wave_flow, zig_zag_connector_limit, wave_pattern);
            }
            if (split_region_paths.empty())
                return false;
            append(overhang_region, split_region_paths);
            append(
                filled_overhang_region,
                additional_shell_count > 0 ?
                    intersection(
                        expand(region, std::max<coord_t>(0, shell_inner_edge - perimeter_overlap), jtRound, 0.),
                        overhang_to_cover) :
                    region);
            return true;
        };

        for (const ExPolygon &wave_cover : union_ex(to_expolygons(wave_cover_area))) {
            ExPolygons split_wave_covers = { wave_cover };
            if (Polygons split_slits = generate_narrow_split_slits(wave_cover, wave_spacing, min_wave_width); ! split_slits.empty())
                split_wave_covers = union_ex(diff_ex(ExPolygons{ wave_cover }, split_slits));

            const Polygons &full_seed_cover_polygons = additional_shell_count > 0 ? overhang_to_cover : to_polygons(wave_cover);
            const ExPolygon &full_seed_boundary = additional_shell_count > 0 ? overhang : wave_cover;
            const Polygons full_anchoring = intersection(expand(full_seed_cover_polygons, 1.1 * base_spacing, jtRound, 0.), inset_anchors);
            const Polylines base_seeds = generate_wave_overhang_seeds(full_seed_boundary, full_anchoring, seed_expansion);

            for (const ExPolygon &split_wave_cover : split_wave_covers) {
                Polygons wave_cover_polygons = to_polygons(split_wave_cover);
                Polylines seeds = base_seeds.empty() ? Polylines{} : intersection_pl(base_seeds, wave_cover_polygons);
                if (seeds.empty())
                    continue;

                const Polygons trim_boundary = trim_of(wave_cover_polygons);

                const coord_t seed_offset = additional_shell_count > 0 ? shell_inner_edge + seed_expansion : seed_expansion;
                Polygons accumulated_region = intersection(offset(seeds, float(seed_offset), jtRound, 0., ClipperLib::etOpenRound), wave_cover_polygons);
                if (accumulated_region.empty())
                    continue;

                if (! arcs) {
                    append_fronts(grow_fronts(accumulated_region, wave_cover_polygons, trim_boundary), accumulated_region);
                    continue;
                }

                //Orio: arc overhangs (idea of Steven McCulloch, Orio implementation). Instead of one front grown
                // from the whole anchored edge, a fan of concentric arcs grows from one point of the supported edge,
                // bounded by a disc of arc_max_radius. The next fan starts from the middle of the longest edge resting
                // on the anchors or on the fans already printed, until the cover is full.
                const Polygons support        = intersection(expand(wave_cover_polygons, 1.1 * base_spacing, jtRound, 0.), inset_anchors);
                const double   min_piece_area = 4. * double(wave_spacing) * double(wave_spacing);
                const double   min_frontier   = 2. * double(wave_spacing);
                // Lines along the middle of the pieces of `empty` too narrow for a wave (corridors between fans, cusps),
                // one width per line, wide enough to overlap both sides like two neighbouring arcs. Not along the free
                // edge nor over the support.
                const Polygons gap_area = diff(shrink(wave_cover_polygons, float(wave_spacing / 2), jtRound, 0.), support);
                const double   min_gap  = 0.25 * wave_flow.scaled_width();
                auto gap_fill = [&](const Polygons &empty, Polylines &lines) {
                    ThickPolylines axes;
                    for (const ExPolygon &piece : union_ex(opening(union_ex(intersection(empty, gap_area)), float(min_gap / 2))))
                        piece.medial_axis(min_gap, 2. * wave_spacing, &axes);
                    ExtrusionPaths paths;
                    for (const ThickPolyline &axis : axes) {
                        if (axis.width.empty() || axis.length() < wave_flow.scaled_width())
                            continue;
                        const double gap   = unscale<double>(std::accumulate(axis.width.begin(), axis.width.end(), 0.) / axis.width.size());
                        const double width = std::clamp(gap + 2. * (wave_flow.width() - unscale<double>(wave_spacing)),
                                                        0.5 * wave_flow.width(), double(wave_flow.width()));
                        ExtrusionPath path(erOverhangPerimeter, wave_flow.mm3_per_mm() * width / wave_flow.width(), float(width), wave_flow.height());
                        path.polyline = axis;
                        lines.push_back(path.polyline);
                        paths.push_back(std::move(path));
                    }
                    return paths;
                };
                Polygons       filled_by_arcs;
                // Footprint of the lines printed by the fans (arcs and gap lines).
                Polygons       line_footprint;
                // Filled or given up (around failed fans, left to the waves below): every fan shrinks what remains.
                Polygons       used;
                // Edges a fan may start from, kept up to date instead of expanding all the fans at every step.
                Polygons       frontier_mask = expand(support, float(2 * seed_expansion));
                Points         failed_starts;
                // ponytail: at most 200 fans per cover, the remainder goes to the waves below.
                for (int fan = 0; fan < 200; ++fan) {
                    ExPolygons remaining = diff_ex(wave_cover_polygons, used);
                    remaining.erase(
                        std::remove_if(remaining.begin(), remaining.end(), [min_piece_area](const ExPolygon &piece) { return piece.area() < min_piece_area; }),
                        remaining.end());
                    if (remaining.empty())
                        break;
                    const Polygons remaining_polygons = to_polygons(remaining);

                    Polylines frontier = intersection_pl(to_polylines(remaining_polygons), frontier_mask);
                    std::sort(frontier.begin(), frontier.end(), [](const Polyline &a, const Polyline &b) { return a.length() > b.length(); });
                    bool  found = false;
                    Point start;
                    for (const Polyline &edge : frontier) {
                        if (edge.length() < min_frontier)
                            break;
                        const Point candidate = point_at_half_length(edge);
                        if (std::any_of(failed_starts.begin(), failed_starts.end(), [&](const Point &failed) {
                                return (failed.cast<double>() - candidate.cast<double>()).norm() < min_frontier; }))
                            continue;
                        start = candidate;
                        found = true;
                        break;
                    }
                    if (! found)
                        break;

                    const Polygons fan_cover = arc_max_radius > 0 ? intersection(remaining_polygons, Polygons{ make_disc(start, arc_max_radius) }) : remaining_polygons;
                    Polygons       fan_region = intersection(Polygons{ make_disc(start, std::max<coord_t>(seed_expansion, wave_spacing / 2)) }, fan_cover);
                    if (fan_region.empty()) {
                        failed_starts.push_back(start);
                        used = union_(used, Polygons{ make_disc(start, coord_t(min_frontier)) });
                        continue;
                    }
                    const Polygons               seed_region = fan_region;
                    // Toward the fans already printed the arcs are trimmed less: their ends lie one wave_spacing from the last
                    // arc and overlap it like two neighbouring arcs (half a line width would leave a gap at the joint).
                    Polygons fan_trim = trim_of(fan_cover);
                    if (! filled_by_arcs.empty())
                        fan_trim = union_(fan_trim, intersection(intersection(
                            shrink(fan_cover, float(std::max<coord_t>(1, wave_spacing / 2 - seed_expansion)), jtRound, 0.),
                            expand(ClipperUtils::clip_clipper_polygons_with_subject_bbox(filled_by_arcs, get_extents(fan_cover).inflated(wave_flow.scaled_width())),
                                   float(wave_flow.scaled_width() / 2 + seed_expansion))), trim_boundary));
                    const std::vector<Polylines> fan_levels  = grow_fronts(fan_region, fan_cover, fan_trim);
                    Polylines                    printed;
                    for (const Polylines &level : fan_levels)
                        append(printed, level);
                    // Corridors left between these arcs and the fans already printed, too narrow for an arc: a gap line
                    // along their middle, printed before the arcs of this fan lying further out, which may rest on it.
                    const coord_t  half_width = wave_flow.scaled_width() / 2;
                    Polylines      gap_lines;
                    ExtrusionPaths gap_paths;
                    if (! printed.empty() && ! line_footprint.empty()) {
                        const Polygons near_fan = offset(printed, float(wave_spacing + half_width), jtRound, 0., ClipperLib::etOpenRound);
                        const Polygons previous = ClipperUtils::clip_clipper_polygons_with_subject_bbox(
                            line_footprint, get_extents(near_fan).inflated(wave_spacing + wave_flow.scaled_width()));
                        gap_paths = gap_fill(diff(intersection(near_fan, expand(previous, float(wave_spacing), jtRound, 0.)),
                                                  union_(offset(printed, float(half_width), jtRound, 0., ClipperLib::etOpenRound), previous)),
                                             gap_lines);
                    }
                    append(printed, gap_lines);
                    // Only the area under the printed arcs counts as filled: the strip between the last arc and the edge of
                    // the disc would otherwise stay empty and show as a gap between fans. Clipped by the whole cover, not
                    // fan_cover, so it closes up with the previous fans.
                    const double   simplify_tolerance = std::min(0.05 * wave_spacing, scaled_resolution);
                    const Polygons covered = polygons_simplify(intersection(
                        union_(offset(seed_region, float(wave_spacing / 2), jtRound, 0.),
                               offset(printed, float(wave_spacing / 2 + seed_expansion), jtRound, 0., ClipperLib::etOpenRound)),
                        wave_cover_polygons), simplify_tolerance);
                    // A fan of a single arc is left to the waves below (its strip would vanish in arc_lines).
                    const size_t first_path = overhang_region.size();
                    if (fan_levels.size() < 2 || covered.empty() || ! append_fronts(fan_levels, covered)) {
                        failed_starts.push_back(start);
                        used = union_(used, intersection(expand(fan_region, float(min_frontier)), fan_cover));
                        continue;
                    }
                    // Each gap line goes before the first arc of this fan lying further from the start than itself.
                    auto radius = [&start](const Polyline &polyline) {
                        double r = std::numeric_limits<double>::max();
                        for (const Point &point : polyline.points)
                            r = std::min(r, (point.cast<double>() - start.cast<double>()).norm());
                        return r;
                    };
                    for (ExtrusionPath &gap_path : gap_paths) {
                        const double r = radius(gap_path.polyline);
                        overhang_region.insert(std::find_if(overhang_region.begin() + first_path, overhang_region.end(),
                                                            [&](const ExtrusionPath &path) { return radius(path.polyline) > r; }),
                                               std::move(gap_path));
                    }
                    filled_by_arcs = union_(filled_by_arcs, covered);
                    line_footprint = union_(line_footprint, offset(printed, float(half_width), jtRound, 0., ClipperLib::etOpenRound));
                    used           = union_(used, covered);
                    frontier_mask  = union_(frontier_mask, expand(covered, float(2 * seed_expansion)));
                }

                // What the fans left (cusps between them, pieces too small for a fan) is filled with ordinary waves grown
                // from the anchors and from the centerlines of the arcs around it.
                // arc_lines ends seed_expansion short of the outer arcs and the seed band brings it back onto their centerline,
                // so the first wave lies one wave_spacing away from the last arc.
                const Polygons arc_lines       = shrink(filled_by_arcs, float(wave_spacing / 2 + 2 * seed_expansion));
                const Polygons leftover        = diff(wave_cover_polygons, arc_lines);
                Polygons       leftover_region = intersection(expand(union_(support, arc_lines), float(seed_expansion)), leftover);
                Polylines      filler_lines;
                if (! leftover_region.empty()) {
                    const std::vector<Polylines> filler_levels = grow_fronts(leftover_region, leftover, trim_of(leftover));
                    for (const Polylines &level : filler_levels)
                        append(filler_lines, level);
                    append_fronts(filler_levels, leftover_region);
                }
                // Gaps still left between the lines (cusps between fans): printed last, nothing rests on them.
                if (! line_footprint.empty()) {
                    Polylines last_gap_lines;
                    append(overhang_region, gap_fill(diff(wave_cover_polygons, union_(line_footprint,
                        offset(filler_lines, float(wave_flow.scaled_width() / 2), jtRound, 0., ClipperLib::etOpenRound))), last_gap_lines));
                }
            }
        }

        overhang_region.erase(
            std::remove_if(overhang_region.begin(), overhang_region.end(), [](const ExtrusionPath &path) { return path.empty(); }),
            overhang_region.end());
        filled_overhang_region = union_(filled_overhang_region);
        append_shell_perimeters(overhang_region, filled_overhang_region, additional_shell_count, base_spacing, overhang_flow, scaled_resolution);
        if (! filled_overhang_region.empty())
            append(filled_area, filled_overhang_region);
        if (overhang_region.empty())
            wave_paths.pop_back();
    }

    tag_wave_overhang_paths(wave_paths);
    return { std::move(wave_paths), union_safety_offset(closing_ex(filled_area, float(filled_area_regularization), jtRound, 0.)) };
}

} } // namespace Slic3r::WaveOverhangs
