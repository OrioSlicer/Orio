#include "ArrangePrusa.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>

#include <boost/log/trivial.hpp>
#include <boost/variant.hpp>

#include <arrange/ArrangeFirstFit.hpp>
#include <arrange/Beds.hpp>
#include <arrange/NFP/PackStrategyNFP.hpp>
#include <arrange/NFP/RectangleOverfitPackingStrategy.hpp>
#include <arrange/NFP/Kernels/GravityKernel.hpp>
#include <arrange/NFP/Kernels/TMArrangeKernel.hpp>
#include <arrange-wrapper/Items/ArrangeItem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/MinAreaBoundingBox.hpp"
#include "libslic3r/Print.hpp"

namespace Slic3r {

// Additions to the arrange library of PrusaSlicer, for the rules of Bambu Studio
namespace arr2 {

// Packing strategy that applies the rules of Bambu Studio on top of a strategy of PrusaSlicer:
// - can_pack() refuses an item on a plate where the filaments of the objects already there forbid it;
// - an item that does not fit on an empty plate is tried again without the extrusion calibration region
//   (fallback_bed), as Bambu Studio does.
template<class BaseStrategy> struct OrioPackStrategy
{
    BaseStrategy base;
    std::function<bool(const ArrangeItem &, const DefaultPackingContext<ArrangeItem> &)> can_pack;
    const ArrangeBed *fallback_bed = nullptr;
};

struct OrioPackStrategyTag {};

template<class Base> struct PackStrategyTag_<OrioPackStrategy<Base>>
{
    using Tag = OrioPackStrategyTag;
};

template<class Base> struct PackStrategyTraits_<OrioPackStrategy<Base>>
{
    template<class ArrItem> using Context = PackStrategyContext<Base, ArrItem>;

    template<class ArrItem, class Bed>
    static Context<ArrItem> create_context(OrioPackStrategy<Base> &ps, const Bed &bed, int bed_index)
    {
        return arr2::create_context<ArrItem>(ps.base, bed, bed_index);
    }
};

template<class... Args> PackStrategyNFP<Args...> &orio_nfp_strategy(PackStrategyNFP<Args...> &s) { return s; }
template<class... Args> PackStrategyNFP<Args...> &orio_nfp_strategy(RectangleOverfitPackingStrategy<Args...> &s)
{
    return s.base_strategy;
}

template<class Strategy, class ArrItem, class Bed, class RemIt>
bool pack(Strategy                                     &strategy,
          const Bed                                    &bed,
          ArrItem                                      &item,
          const PackStrategyContext<Strategy, ArrItem> &packing_context,
          const Range<RemIt>                           &remaining_items,
          const OrioPackStrategyTag &)
{
    const ArrangeItem &itm = item;
    if (strategy.can_pack && !strategy.can_pack(itm, packing_context))
        return false;

    // A failed packing of PackStrategyNFP leaves the item with the last rotation tried. The rotations allowed are
    // relative to the current one: without restoring it, they would add up from one plate to the next.
    const double  rotation0    = get_rotation(item);
    const Vec2crd translation0 = get_translation(item);
    auto restore = [&]() {
        set_rotation(item, rotation0);
        set_translation(item, translation0);
    };

    bool packed = pack(strategy.base, bed, item, packing_context, remaining_items);

    if (!packed && strategy.fallback_bed != nullptr && packing_context.packed_items_range().empty()) {
        restore();
        auto &nfp_strategy = orio_nfp_strategy(strategy.base);
        boost::apply_visitor([&](const auto &fallback_bed) {
            packed = pack(nfp_strategy, fallback_bed, item, packing_context, remaining_items);
        }, *strategy.fallback_bed);
    }

    if (!packed)
        restore();

    return packed;
}

// TMArrangeKernel of PrusaSlicer, whose pile grows from the alignment pivot. On an empty plate packed by the
// RectangleOverfitPackingStrategy (infinite plate, pile aligned afterwards), it keeps the center of the plate as
// PrusaSlicer does: an initial position away from the center may not fit, and that strategy has no other one.
class OrioTMArrangeKernel : public TMArrangeKernel
{
    Point m_pivot;
    bool  m_overfit;

public:
    OrioTMArrangeKernel(const Point &pivot, bool overfit, size_t itm_cnt, double bedarea)
        : TMArrangeKernel(itm_cnt, bedarea), m_pivot(pivot), m_overfit(overfit)
    {}

    template<class ArrItem, class Bed, class Context, class RemIt>
    bool on_start_packing(ArrItem &itm, const Bed &bed, const Context &packing_context, const Range<RemIt> &remaining_items)
    {
        sink = (m_overfit && fixed_items_range(packing_context).empty()) ? bounding_box(bed).center() : m_pivot;
        return TMArrangeKernel::on_start_packing(itm, bed, packing_context, remaining_items);
    }
};

} // namespace arr2

namespace GUI {

using arrangement::ArrangePolygon;
using arrangement::ArrangePolygons;
using arrangement::ArrangeParams;
using arr2::ArrangeItem;

static const std::string ArrangeSection = "arrange";

ArrangePrusaSettings ArrangePrusaSettings::load(const AppConfig &cfg, const std::string &postfix)
{
    ArrangePrusaSettings settings;

    const std::string dist_str = cfg.get(ArrangeSection, "min_bed_distance" + postfix);
    if (!dist_str.empty()) {
        // Saved with float_to_string_decimal_point(): independent of the locale.
        size_t       pos  = 0;
        const double dist = string_to_double_decimal_point(dist_str, &pos);
        if (pos > 0 && dist >= 0.)
            settings.bed_distance = float(dist);
    }

    auto load_choice = [&cfg](const std::string &key, int count, int &value) {
        const std::string str = cfg.get(ArrangeSection, key);
        if (!str.empty()) {
            const int v = std::atoi(str.c_str());
            if (v >= 0 && v < count)
                value = v;
        }
    };
    load_choice("geometry_handling", ghCount, settings.geometry);
    load_choice("alignment", alCount, settings.alignment);

    return settings;
}

void ArrangePrusaSettings::save(AppConfig &cfg, const std::string &postfix) const
{
    cfg.set(ArrangeSection, "min_bed_distance" + postfix, float_to_string_decimal_point(bed_distance));
    cfg.set(ArrangeSection, "geometry_handling", std::to_string(geometry));
    cfg.set(ArrangeSection, "alignment", std::to_string(alignment));
}

namespace {

using PackingContext = arr2::DefaultPackingContext<ArrangeItem>;

// Data attached to the arrange items
const std::string KeyBambu     = "orio_arrange_polygon"; // const ArrangePolygon*: the item in Bambu Studio
const std::string KeyWipeTower = "is_wipe_tower";          // the key tested by arr2::is_wipe_tower()

constexpr coord_t MinSeparation = scaled<coord_t>(0.5); // minimum inflation of the items, MIN_SEPARATION of libnest2d
constexpr coord_t TowerSeparation = scaled<coord_t>(1.); // minimum inflation of a wipe tower, as a fixed one gets
                                                         // (exclusion_gap of update_unselected_items_inflation())
constexpr coord_t SimplifyTol   = scaled<coord_t>(0.2); // simplification of the real outlines, as in PrusaSlicer
constexpr coord_t FitSlack      = scaled<coord_t>(0.5); // room kept when the spacing of a large object is reduced
constexpr double  Accuracy      = 1.;                   // as DefaultArranger of PrusaSlicer

const ArrangePolygon *bambu_polygon(const ArrangeItem &itm)
{
    const ArrangePolygon *const *ptr = arr2::get_data<const ArrangePolygon *>(itm, KeyBambu);
    return ptr != nullptr ? *ptr : nullptr;
}

// Point of the plate the pile goes to: its center (moved by the best_object_pos of the printer) or a corner.
Point alignment_pivot(const BoundingBox &bedbb, int alignment, const Vec2d &center_rel)
{
    switch (alignment) {
    case ArrangePrusaSettings::alRearLeft: return Point(bedbb.min.x(), bedbb.max.y());
    case ArrangePrusaSettings::alFrontLeft: return bedbb.min;
    case ArrangePrusaSettings::alFrontRight: return Point(bedbb.max.x(), bedbb.min.y());
    case ArrangePrusaSettings::alRearRight: return bedbb.max;
    default: {
        const Vec2d size = bedbb.size().cast<double>();
        return Point(bedbb.min.x() + coord_t(size.x() * center_rel.x()), bedbb.min.y() + coord_t(size.y() * center_rel.y()));
    }
    }
}

// Translation that moves a pile to the alignment pivot, keeping it on the plate.
Vec2crd pile_alignment(const BoundingBox &bedbb, const BoundingBox &pilebb, int alignment, const Vec2d &center_rel)
{
    const Point pivot = alignment_pivot(bedbb, alignment, center_rel);

    Vec2crd d;
    switch (alignment) {
    case ArrangePrusaSettings::alRearLeft: d = Vec2crd(pivot.x() - pilebb.min.x(), pivot.y() - pilebb.max.y()); break;
    case ArrangePrusaSettings::alFrontLeft: d = pivot - pilebb.min; break;
    case ArrangePrusaSettings::alFrontRight: d = Vec2crd(pivot.x() - pilebb.max.x(), pivot.y() - pilebb.min.y()); break;
    case ArrangePrusaSettings::alRearRight: d = pivot - pilebb.max; break;
    default: d = pivot - pilebb.center(); break;
    }

    // As SegmentedRectangleBed of PrusaSlicer: no part of the pile outside the plate.
    BoundingBox moved = pilebb;
    translate_bb(moved, d);
    d.x() += -std::min<coord_t>(0, moved.min.x() - bedbb.min.x()) - std::max<coord_t>(0, moved.max.x() - bedbb.max.x());
    d.y() += -std::min<coord_t>(0, moved.min.y() - bedbb.min.y()) - std::max<coord_t>(0, moved.max.y() - bedbb.max.y());

    return d;
}

// Arrange item of an ArrangePolygon of Bambu Studio, false if it has no usable outline.
bool make_item(ArrangeItem &itm, const ArrangePolygon &ap, bool fixed, coord_t inflation, const ArrangePrusaParams &pparams)
{
    const int geometry  = pparams.settings.geometry;
    bool      has_shape = false;

    // As the converters of PrusaSlicer (ArrangeImpl.hpp): real outline inflated and simplified, its convex hull
    // as envelope (the shape of the moving item) for the balanced geometry.
    if (geometry != ArrangePrusaSettings::ghFast && !ap.is_virt_object && pparams.outline) {
        ExPolygons outline = pparams.outline(ap, fixed);
        if (!outline.empty()) {
            if (inflation != 0)
                outline = offset_ex(outline, float(inflation));
            outline = expolygons_simplify(outline, double(SimplifyTol));
            if (!outline.empty()) {
                arr2::set_shape(itm, outline);
                if (geometry == ArrangePrusaSettings::ghBalanced)
                    arr2::set_convex_envelope(itm, arr2::envelope_convex_hull(itm));
                has_shape = true;
            }
        }
    }

    if (!has_shape) {
        Polygon contour = ap.poly.contour;
        if (contour.size() < 3)
            return false;
        if (contour.is_clockwise())
            contour.reverse();
        Polygon hull = inflation != 0 ? Geometry::convex_hull(offset(contour, float(inflation))) : Geometry::convex_hull(contour.points);
        if (hull.size() < 3)
            return false;
        arr2::set_convex_shape(itm, hull);
    }

    // The transformation after the shape: setting the shape resets it.
    arr2::set_translation(itm, ap.translation);
    arr2::set_rotation(itm, ap.rotation);
    arr2::set_priority(itm, ap.priority);
    arr2::set_bed_index(itm, fixed ? ap.bed_idx : arr2::Unarranged);
    arr2::set_data(itm, KeyBambu, &ap);
    if (ap.is_wipe_tower)
        arr2::set_data(itm, KeyWipeTower, true);

    return true;
}

// Rotations tried for an item (relative to its current rotation), empty: only the current one.
std::vector<double> allowed_rotations_of(const ArrangeItem &itm, const ArrangePolygon &ap, const BoundingBox &bedbb, const ArrangeParams &params)
{
    std::vector<double> rotations;

    // Rotating the wipe tower is not supported (PrusaSlicer does not rotate it either).
    if (ap.is_wipe_tower)
        return rotations;

    const Polygon &chull = arr2::envelope_convex_hull(itm);
    if (chull.size() < 3)
        return rotations;

    const Vec2crd bedsz = bedbb.size();
    auto fits = [&bedsz](const Polygon &hull, double rot) {
        Polygon rotated = hull;
        rotated.rotate(rot);
        const Vec2crd sz = get_extents(rotated).size();
        return sz.x() <= bedsz.x() && sz.y() <= bedsz.y();
    };

    double minbbr = 0.;
    try {
        minbbr = MinAreaBoundigBox{chull, MinAreaBoundigBox::pcConvex}.angle_to_X();
    } catch (const std::exception &e) {
        // May divide by zero on a shape already aligned with X (seen by Bambu Studio).
        BOOST_LOG_TRIVIAL(warning) << "arrange: minimum area bounding box failed for " << ap.name << ": " << e.what();
    }

    if (params.align_to_y_axis) {
        // Bambu Studio: the long side of an elongated object (more than 10 %) goes along Y.
        Polygon rotated = chull;
        rotated.rotate(minbbr);
        const Vec2crd sz  = get_extents(rotated).size();
        double        rot = 0.;
        if (sz.x() > 1.1 * sz.y())
            rot = minbbr + PI / 2.;
        else if (sz.y() > 1.1 * sz.x())
            rot = minbbr;
        rotations.emplace_back(rot);
        if (rot != 0. && !fits(chull, rot))
            rotations.emplace_back(0.);
    } else if (params.allow_rotations) {
        // As fill_rotations() of PrusaSlicer.
        rotations = {minbbr, minbbr + PI / 4., minbbr + PI / 2., minbbr + PI, minbbr + 3 * PI / 4.};
        if (std::abs(minbbr) > 0.)
            rotations.emplace_back(0.);
    }

    // An object larger than the plate may fit once rotated. Bambu Studio tries it even without auto rotation.
    if (!fits(chull, 0.)) {
        double fitrot = 0.;
        try {
            fitrot = fit_into_box_rotation(chull, bedbb);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "arrange: fit into bed rotation failed for " << ap.name << ": " << e.what();
        }
        if (std::abs(fitrot) > 0.) {
            if (rotations.empty())
                rotations.emplace_back(0.);
            rotations.emplace_back(fitrot);
        }
    }

    return rotations;
}

// Rules of Bambu Studio for the objects sharing a plate (printing by layer): compatible filament temperatures,
// at most one TPU extruder and, when several materials are not allowed on a plate, no extruder the plate does not
// use yet. Taken from the object function of Arrange.cpp, where they are scores that reject the plate.
bool filaments_allow(const ArrangeItem &itm, const PackingContext &packing_context, const ArrangeParams &params)
{
    const ArrangePolygon *ap = bambu_polygon(itm);
    if (ap == nullptr || ap->is_virt_object)
        return true;

    std::map<int, std::string> plate_extruders;
    std::set<int>              tpu_extruders;
    auto add_tpu = [&tpu_extruders](const ArrangePolygon &p) {
        for (const auto &[id, type] : p.extrude_id_filament_types)
            if (type == "TPU")
                tpu_extruders.insert(id);
    };

    for (const ArrangeItem &other : packing_context.all_items_range()) {
        const ArrangePolygon *oap = bambu_polygon(other);
        if (oap == nullptr || oap->is_virt_object)
            continue;
        if (!Print::is_filaments_compatible({ap->filament_temp_type, oap->filament_temp_type}))
            return false;
        plate_extruders.insert(oap->extrude_id_filament_types.begin(), oap->extrude_id_filament_types.end());
        add_tpu(*oap);
    }

    add_tpu(*ap);
    if (tpu_extruders.size() > 1)
        return false;

    if (!params.allow_multi_materials_on_same_plate && !plate_extruders.empty() &&
        !std::includes(plate_extruders.begin(), plate_extruders.end(), ap->extrude_id_filament_types.begin(),
                       ap->extrude_id_filament_types.end()))
        return false;

    return true;
}

// After the packing, moves the pile of each empty plate (packed around its center by the
// RectangleOverfitPackingStrategy) to the alignment, as PrusaSlicer does for the segmented bed of the XL.
// The plates with fixed objects are left as packed. A fixed wipe tower is an obstacle and, with bed_shape (plate
// with excluded regions), the pile must stay inside that shape: the pile is then moved only where it is free, the
// whole move, else along one axis, else a part of it. A wipe tower of the pile moves with it, staying in tower_area.
void align_piles(std::vector<ArrangeItem>       &items,
                 const std::vector<ArrangeItem> &fixed,
                 const BoundingBox              &bedbb,
                 int                             alignment,
                 const Vec2d                    &center_rel,
                 const ExPolygons               *bed_shape  = nullptr,
                 const Polygons                 *tower_area = nullptr)
{
    std::set<int>           occupied;
    std::map<int, Polygons> towers;
    for (const ArrangeItem &itm : fixed) {
        if (arr2::is_wipe_tower(itm))
            Slic3r::append(towers[arr2::get_bed_index(itm)], arr2::fixed_outline(itm));
        else
            occupied.insert(arr2::get_bed_index(itm));
    }

    std::map<int, BoundingBox> piles;
    std::set<int>              with_tower;
    for (const ArrangeItem &itm : items) {
        const int bed_idx = arr2::get_bed_index(itm);
        if (bed_idx < 0)
            continue;
        if (arr2::is_wipe_tower(itm) && tower_area != nullptr)
            with_tower.insert(bed_idx);
        piles[bed_idx].merge(arr2::fixed_bounding_box(itm));
    }

    for (const auto &[bed_idx, pilebb] : piles) {
        if (occupied.count(bed_idx) > 0 || !pilebb.defined)
            continue;
        const Vec2crd full = pile_alignment(bedbb, pilebb, alignment, center_rel);
        if (full.x() == 0 && full.y() == 0)
            continue;

        const auto      towers_it = towers.find(bed_idx);
        const Polygons *obstacles = towers_it != towers.end() ? &towers_it->second : nullptr;

        Vec2crd d = full;
        const bool tower_check = with_tower.count(bed_idx) > 0;
        if (bed_shape != nullptr || obstacles != nullptr || tower_check) {
            // The hulls checked by the packing (inner-fit polygon of the envelope).
            Polygons hulls, tower_hulls;
            for (const ArrangeItem &itm : items)
                if (arr2::get_bed_index(itm) == bed_idx) {
                    hulls.emplace_back(arr2::envelope_convex_hull(itm));
                    if (tower_check && arr2::is_wipe_tower(itm))
                        tower_hulls.emplace_back(hulls.back());
                }
            auto fits = [&hulls, &tower_hulls, bed_shape, obstacles, tower_area](const Vec2crd &v) {
                auto moved = [&v](Polygons polys) {
                    for (Polygon &poly : polys)
                        poly.translate(Point(v));
                    return polys;
                };
                const Polygons moved_hulls = moved(hulls);
                return (bed_shape == nullptr || Slic3r::diff(moved_hulls, *bed_shape).empty()) &&
                       (obstacles == nullptr || Slic3r::intersection(moved_hulls, *obstacles).empty()) &&
                       (tower_hulls.empty() || Slic3r::diff(moved(tower_hulls), *tower_area).empty());
            };
            const Vec2crd candidates[] = {full,
                                          Vec2crd(full.x(), 0),
                                          Vec2crd(0, full.y()),
                                          Vec2crd(full.x() / 2, full.y() / 2),
                                          Vec2crd(full.x() / 2, 0),
                                          Vec2crd(0, full.y() / 2),
                                          Vec2crd(full.x() / 4, full.y() / 4)};
            d = Vec2crd::Zero();
            for (const Vec2crd &candidate : candidates)
                if ((candidate.x() != 0 || candidate.y() != 0) && fits(candidate)) {
                    d = candidate;
                    break;
                }
            if (d.x() == 0 && d.y() == 0)
                continue;
        }

        for (ArrangeItem &itm : items)
            if (arr2::get_bed_index(itm) == bed_idx)
                arr2::translate(itm, d);
    }
}

// Rectangles of bedbb overlapping none of the bounding boxes of the regions, whose sides lie on the borders of the
// plate or of the regions. Made for the excluded regions of the Bambu printers (a few rectangles along the borders of
// the plate).
std::vector<BoundingBox> free_rectangles(const BoundingBox &bedbb, const Polygons &regions)
{
    std::vector<BoundingBox> boxes;
    std::vector<coord_t>     xs{bedbb.min.x(), bedbb.max.x()}, ys{bedbb.min.y(), bedbb.max.y()};
    for (const Polygon &region : regions) {
        const BoundingBox bb = get_extents(region);
        boxes.emplace_back(bb);
        for (coord_t x : {bb.min.x(), bb.max.x()})
            if (x > bedbb.min.x() && x < bedbb.max.x())
                xs.emplace_back(x);
        for (coord_t y : {bb.min.y(), bb.max.y()})
            if (y > bedbb.min.y() && y < bedbb.max.y())
                ys.emplace_back(y);
    }
    sort_remove_duplicates(xs);
    sort_remove_duplicates(ys);

    std::vector<BoundingBox> rects;
    for (size_t i = 0; i < xs.size(); ++i)
        for (size_t j = i + 1; j < xs.size(); ++j)
            for (size_t k = 0; k < ys.size(); ++k)
                for (size_t l = k + 1; l < ys.size(); ++l) {
                    const BoundingBox rect(Point(xs[i], ys[k]), Point(xs[j], ys[l]));
                    if (std::none_of(boxes.begin(), boxes.end(), [&rect](const BoundingBox &b) {
                            return b.min.x() < rect.max.x() && b.max.x() > rect.min.x() && b.min.y() < rect.max.y() &&
                                   b.max.y() > rect.min.y();
                        }))
                        rects.emplace_back(rect);
                }
    return rects;
}

// The largest of the free rectangles, nothing if it is less than half of the plate.
std::optional<BoundingBox> largest_free_rectangle(const BoundingBox &bedbb, const std::vector<BoundingBox> &rects)
{
    std::optional<BoundingBox> best;
    double                     best_area = 0.5 * double(bedbb.size().x()) * double(bedbb.size().y());
    for (const BoundingBox &rect : rects)
        if (const double rect_area = double(rect.size().x()) * double(rect.size().y()); rect_area > best_area) {
            best      = rect;
            best_area = rect_area;
        }
    return best;
}

// Largest inflation of an object of bounding box size `sz` that still fits into a space of size `room`, straight or
// turned by 90 degrees (negative if it does not fit).
coord_t max_inflation_in(const Vec2crd &room, const Vec2crd &sz)
{
    return std::max(std::min(room.x() - sz.x(), room.y() - sz.y()), std::min(room.x() - sz.y(), room.y() - sz.x())) / 2;
}

// Packing as DefaultArranger::arrange_() of PrusaSlicer (ArrangeImpl.hpp), with the rules of Bambu Studio.
// plate_bb: the whole plate, for the alignment; plate_shape: the plate without its excluded regions when bed is only
// the free rectangle of it (see arrange_prusa()), the piles are then aligned as far as they stay inside.
template<class Bed>
void arrange_on_bed(std::vector<ArrangeItem>       &items,
                    const std::vector<ArrangeItem> &fixed,
                    const Bed                      &bed,
                    const arr2::ArrangeBed         *fallback_bed,
                    int                             alignment,
                    const ArrangeParams            &params,
                    int                             max_bed_count,
                    std::vector<int>               &packing_order,
                    const BoundingBox              &plate_bb,
                    const ExPolygons               *plate_shape)
{
    auto cmpfn = [](const ArrangeItem &a, const ArrangeItem &b) {
        const int pa = arr2::get_priority(a);
        const int pb = arr2::get_priority(b);
        return pa == pb ? arr2::envelope_convex_hull(a).area() > arr2::envelope_convex_hull(b).area() : pa > pb;
    };

    int finished = 0, packed = 0;
    auto on_arranged = [&](auto &itm, auto & /*bed*/, auto & /*packed_items*/, auto & /*remaining*/) {
        const ArrangeItem &ai  = itm;
        const size_t       idx = size_t(&ai - items.data());
        if (arr2::is_arranged(ai) && idx < packing_order.size())
            packing_order[idx] = packed++;
        if (params.progressind) {
            const ArrangePolygon *ap = bambu_polygon(ai);
            params.progressind(unsigned(++finished), ap != nullptr ? ap->name : std::string());
        }
    };

    auto stop_cond = [&params]() { return params.stopcondition && params.stopcondition(); };

    arr2::firstfit::SelectionStrategy sel{cmpfn, on_arranged, stop_cond};
    sel.max_bed_count = max_bed_count;

    std::function<bool(const ArrangeItem &, const PackingContext &)> can_pack =
        [&params](const ArrangeItem &itm, const PackingContext &packing_context) {
            return filaments_allow(itm, packing_context, params);
        };

    const Point       pivot    = alignment_pivot(plate_bb, alignment, params.align_center);
    const double      bed_area = arr2::area(bed);

    auto run = [&](auto &&strategy) {
        using Base = std::decay_t<decltype(strategy)>;
        arr2::OrioPackStrategy<Base> orio_strategy{std::move(strategy), can_pack, fallback_bed};
        arr2::arrange(sel, orio_strategy, Slic3r::range(items), Slic3r::crange(fixed), bed);
    };

    if constexpr (std::is_convertible_v<Bed, arr2::CircleBed>) {
        // As PrusaSlicer for circular beds.
        arr2::PackStrategyNFP strategy{arr2::GravityKernel{pivot}, ex_tbb, Accuracy, stop_cond};
        run(std::move(strategy));
    } else {
        const bool with_wipe_tower = std::any_of(items.begin(), items.end(), [](const ArrangeItem &itm) { return arr2::is_wipe_tower(itm); });

        // As PrusaSlicer: with a rectangular plate and no wipe tower to place, the empty plates are packed on an
        // infinite plate whose pile must fit into the rectangle, then centered; it gives better results than the
        // inner-fit polygon of the rectangle.
        if (!with_wipe_tower && arr2::IsRectangular<Bed>) {
            arr2::PackStrategyNFP base{arr2::OrioTMArrangeKernel{pivot, true, items.size(), bed_area}, ex_tbb, Accuracy, stop_cond};
            arr2::RectangleOverfitPackingStrategy strategy{std::move(base)};
            run(std::move(strategy));
            align_piles(items, fixed, plate_bb, alignment, params.align_center, plate_shape);
        } else {
            arr2::PackStrategyNFP strategy{arr2::OrioTMArrangeKernel{pivot, false, items.size(), bed_area}, ex_tbb, Accuracy, stop_cond};
            run(std::move(strategy));
            // Plate with excluded regions (X1, P1...): the pile grew from the border of the plate nearest to the
            // pivot (no initial position at the pivot, see find_initial_position()); aligned afterwards as far as
            // it stays on the plate.
            if constexpr (std::is_convertible_v<Bed, arr2::IrregularBed>)
                align_piles(items, fixed, plate_bb, alignment, params.align_center, &bed.poly);
        }
    }
}

} // namespace

void arrange_prusa(ArrangePolygons       &selected,
                   const ArrangePolygons &fixed,
                   const Points          &bedpts,
                   const ArrangeParams   &params,
                   const ArrangePrusaParams &pparams)
{
    const ArrangePrusaSettings &settings = pparams.settings;

    // Excluded regions (bed_exclude_area, wrapping detection) and extrusion calibration regions: Bambu Studio adds
    // them as fixed virtual objects on each plate. They are the same on every plate and are removed from it here,
    // so that they do not count as a pile the objects should gather around.
    Polygons hard_regions, cali_regions;
    for (const ArrangePolygon &ap : fixed) {
        if (!ap.is_virt_object || ap.is_wipe_tower)
            continue;
        Polygon contour = ap.transformed_poly().contour;
        if (contour.size() < 3)
            continue;
        if (contour.is_clockwise())
            contour.reverse();
        append(ap.is_extrusion_cali_object ? cali_regions : hard_regions, offset(contour, float(std::max(ap.inflation, MinSeparation))));
    }

    // "Spacing from bed": the plate already lost the margin of Bambu Studio, and an object keeps at least its
    // inflation (half the spacing) from the edges.
    coord_t extra_shrink = 0;
    if (settings.bed_distance > 0.f) {
        coord_t min_inflation = std::numeric_limits<coord_t>::max();
        for (const ArrangePolygon &ap : selected)
            if (!ap.is_virt_object)
                min_inflation = std::min(min_inflation, std::max(ap.inflation, MinSeparation));
        if (min_inflation == std::numeric_limits<coord_t>::max())
            min_inflation = MinSeparation;
        extra_shrink = std::max<coord_t>(0, scaled<coord_t>(settings.bed_distance) - pparams.bed_margin - min_inflation);
    }
    // As get_corrected_bed() of PrusaSlicer: the simplified outlines may be a little larger than the real ones.
    const coord_t simplify_tol = settings.geometry != ArrangePrusaSettings::ghFast ? SimplifyTol : 0;

    arr2::ArrangeBed rawbed = arr2::to_arrange_bed(bedpts, Vec2crd::Zero());
    if (extra_shrink + simplify_tol > 0)
        rawbed = arr2::offset(rawbed, -(extra_shrink + simplify_tol));

    arr2::ArrangeBed                bed = rawbed;
    std::optional<arr2::ArrangeBed> fallback_bed;
    ExPolygons                      plate; // the plate without its excluded regions, if it has some
    if (!hard_regions.empty() || !cali_regions.empty()) {
        plate = arr2::to_expolygons(rawbed);
        if (!hard_regions.empty())
            plate = diff_ex(plate, union_(hard_regions));
        if (!cali_regions.empty()) {
            fallback_bed = hard_regions.empty() ? rawbed : arr2::ArrangeBed{arr2::IrregularBed{plate, Vec2crd::Zero()}};
            plate        = diff_ex(plate, union_(cali_regions));
        }
        bed = arr2::IrregularBed{plate, Vec2crd::Zero()};
    }

    const BoundingBox bedbb = arr2::bounding_box(bed);

    // Rectangular plate with excluded regions (X1, P1...): its largest rectangle free of them. The objects are first
    // packed into it as PrusaSlicer packs a rectangular plate (see below).
    std::optional<BoundingBox> free_rect;
    std::vector<BoundingBox>   free_rects; // with and without the calibration regions (fallback_bed)
    if (!plate.empty() && boost::get<arr2::RectangleBed>(&rawbed) != nullptr) {
        const BoundingBox rawbb   = arr2::bounding_box(rawbed);
        Polygons          regions = hard_regions;
        append(regions, cali_regions);
        free_rects = free_rectangles(rawbb, regions);
        free_rect  = largest_free_rectangle(rawbb, free_rects);
        if (!cali_regions.empty())
            append(free_rects, free_rectangles(rawbb, hard_regions));
    }

    int alignment = settings.alignment;
    if (alignment == ArrangePrusaSettings::alRandom) {
        std::random_device                 rd;
        std::mt19937                       rng(rd());
        std::uniform_int_distribution<int> dist(0, ArrangePrusaSettings::alRandom - 1);
        alignment = dist(rng);
    }

    // Items to arrange: the objects. The wipe towers among `selected` are moved after them (see below).
    std::vector<ArrangeItem> items;
    std::vector<size_t>      item_src; // index in selected
    {
        std::vector<std::optional<ArrangeItem>> built(selected.size());
        execution::for_each(ex_tbb, size_t(0), selected.size(), [&](size_t i) {
            const ArrangePolygon &ap = selected[i];
            if (ap.is_wipe_tower)
                return;

            coord_t inflation = std::max(ap.inflation, MinSeparation);
            // Bambu Studio: an object that fits on the plate only without its spacing gets a smaller spacing. With
            // excluded regions, the room is the free rectangle where the object fits best (225 x 230 mm with the
            // automatic spacing of the supports, 6 mm or more: no room beside the excluded region of a X1C, the object
            // was left out of the plates). Minus FitSlack: the inner-fit polygon of an irregular plate needs some room.
            const Vec2crd sz            = get_extents(ap.poly.contour).size();
            coord_t       max_inflation = free_rects.empty() ? max_inflation_in(bedbb.size(), sz) : std::numeric_limits<coord_t>::min();
            for (const BoundingBox &rect : free_rects)
                max_inflation = std::max(max_inflation, max_inflation_in(rect.size(), sz) - FitSlack);
            if (max_inflation >= 0 && max_inflation < inflation)
                inflation = max_inflation;

            ArrangeItem itm;
            if (!make_item(itm, ap, false, inflation, pparams))
                return;

            std::vector<double> rotations = allowed_rotations_of(itm, ap, bedbb, params);
            if (!rotations.empty())
                arr2::set_allowed_rotations(itm, rotations);

            built[i] = std::move(itm);
        });

        for (size_t i = 0; i < built.size(); ++i) {
            if (built[i]) {
                items.emplace_back(std::move(*built[i]));
                item_src.emplace_back(i);
            } else if (!selected[i].is_wipe_tower) {
                selected[i].bed_idx = arrangement::UNARRANGED;
                BOOST_LOG_TRIVIAL(warning) << "arrange: no outline for " << selected[i].name;
            }
        }
    }

    // Items that stay in place: objects and wipe towers of the plates
    std::vector<ArrangeItem> fixed_items;
    {
        std::vector<const ArrangePolygon *> fixed_src;
        for (const ArrangePolygon &ap : fixed)
            if (!ap.is_virt_object || ap.is_wipe_tower)
                fixed_src.emplace_back(&ap);

        std::vector<std::optional<ArrangeItem>> built(fixed_src.size());
        execution::for_each(ex_tbb, size_t(0), fixed_src.size(), [&](size_t i) {
            const ArrangePolygon &ap = *fixed_src[i];
            if (ap.bed_idx < 0 || ap.bed_idx >= pparams.max_bed_count)
                return;
            ArrangeItem itm;
            if (make_item(itm, ap, true, std::max(ap.inflation, MinSeparation), pparams))
                built[i] = std::move(itm);
        });

        for (std::optional<ArrangeItem> &itm : built)
            if (itm)
                fixed_items.emplace_back(std::move(*itm));
    }

    // Wipe towers to move with the objects of their plate (bed_idx), built as fixed items at their place. One stays
    // there when its plate has fixed objects (arranging the objects outside the plates) or when no part of the plate
    // is in tower_area. A tower not moved gets UNARRANGED: ArrangeJob leaves it as it is.
    arr2::ArrangeBed tower_bed       = bed;
    bool             towers_can_move = true;
    if (!pparams.tower_area.empty()) {
        ExPolygons area = intersection_ex(arr2::to_expolygons(bed), pparams.tower_area);
        towers_can_move = !area.empty();
        tower_bed       = arr2::IrregularBed{std::move(area), Vec2crd::Zero()};
    }
    std::set<int> beds_with_fixed;
    for (const ArrangePolygon &ap : fixed)
        if (!ap.is_virt_object && ap.bed_idx >= 0)
            beds_with_fixed.insert(ap.bed_idx);
    std::vector<ArrangeItem> towers;
    std::vector<size_t>      tower_src;   // index in selected
    std::vector<bool>        tower_fixed; // stays where it is
    for (size_t i = 0; i < selected.size(); ++i) {
        ArrangePolygon &ap = selected[i];
        if (!ap.is_wipe_tower)
            continue;
        ArrangeItem itm;
        if (ap.bed_idx >= 0 && ap.bed_idx < pparams.max_bed_count &&
            make_item(itm, ap, true, std::max(ap.inflation, TowerSeparation), pparams)) {
            arr2::set_bed_constraint(itm, ap.bed_idx);
            towers.emplace_back(std::move(itm));
            tower_src.emplace_back(i);
            tower_fixed.emplace_back(!towers_can_move || beds_with_fixed.count(ap.bed_idx) > 0);
        }
        ap.bed_idx = arrangement::UNARRANGED;
    }

    BOOST_LOG_TRIVIAL(info) << "arrange (PrusaSlicer engine): " << items.size() << " items, " << fixed_items.size()
                            << " fixed items, " << towers.size() << " wipe towers, geometry " << settings.geometry
                            << ", alignment " << alignment << ", spacing from bed " << settings.bed_distance
                            << " mm, excluded regions " << hard_regions.size() << ", calibration regions "
                            << cali_regions.size();

    auto arrange_on_plate = [&](std::vector<ArrangeItem> &its, const std::vector<ArrangeItem> &fxd, std::vector<int> &order) {
        boost::apply_visitor([&](const auto &b) {
            arrange_on_bed(its, fxd, b, fallback_bed ? &*fallback_bed : nullptr, alignment, params,
                           pparams.max_bed_count, order, bedbb, nullptr);
        }, bed);
    };
    auto cancelled = [&params]() { return params.stopcondition && params.stopcondition(); };

    // The objects, around fxd.
    std::vector<int> packing_order;
    auto arrange_objects = [&](const std::vector<ArrangeItem> &fxd) {
        if (!free_rect) {
            arrange_on_plate(items, fxd, packing_order);
            return;
        }
        // Pass 1, as PrusaSlicer packs a rectangular plate (pile on an infinite plate, kept inside the rectangle, then
        // centered), into the free rectangle; the piles are then aligned on the whole plate as far as they stay out
        // of the excluded regions. The NFP of the irregular plate alone puts the first object against its border,
        // as near to the pivot as possible: turned by 45 degrees, its bounding box is the largest and it gets the
        // nearest, leaving no room for the next objects (two objects of 150 x 97 mm: one per plate on a X1C).
        BOOST_LOG_TRIVIAL(info) << "arrange (PrusaSlicer engine): free rectangle " << unscaled(free_rect->size().x())
                                << " x " << unscaled(free_rect->size().y()) << " mm";
        arrange_on_bed(items, fxd, arr2::RectangleBed{*free_rect, Vec2crd::Zero()}, nullptr, alignment, params,
                       pparams.max_bed_count, packing_order, bedbb, &plate);

        // Pass 2: the objects that do not fit into the free rectangle (large objects needing the strips along the
        // excluded regions), on the irregular plate, around the objects of pass 1.
        std::vector<size_t>      left;
        std::vector<ArrangeItem> rest;
        std::vector<ArrangeItem> fixed_rest = fxd;
        for (size_t k = 0; k < items.size(); ++k) {
            if (arr2::is_arranged(items[k]))
                fixed_rest.emplace_back(items[k]);
            else {
                left.emplace_back(k);
                rest.emplace_back(items[k]);
            }
        }
        if (!rest.empty() && !cancelled()) {
            BOOST_LOG_TRIVIAL(info) << "arrange (PrusaSlicer engine): " << rest.size() << " items arranged on the whole plate";
            std::vector<int> order_rest(rest.size(), -1);
            arrange_on_plate(rest, fixed_rest, order_rest);
            const int packed = int(std::count_if(packing_order.begin(), packing_order.end(), [](int o) { return o >= 0; }));
            for (size_t j = 0; j < rest.size(); ++j) {
                items[left[j]] = std::move(rest[j]);
                if (order_rest[j] >= 0)
                    packing_order[left[j]] = packed + order_rest[j];
            }
        }
    };

    // The objects, then each wipe tower against the objects of its plate, as near to the pivot as possible (the
    // kernel scores a wipe tower by its distance to the sink), then the whole is aligned again. A wipe tower with no
    // room left stays where it is and the objects are arranged again around it.
    // ponytail: all the objects arranged again for each set of towers without room; per plate if it gets slow.
    const std::vector<ArrangeItem> items0 = items;
    for (;;) {
        items = items0;
        packing_order.assign(items.size(), -1);
        std::vector<ArrangeItem> fixed_now = fixed_items;
        for (size_t t = 0; t < towers.size(); ++t)
            if (tower_fixed[t])
                fixed_now.emplace_back(towers[t]);

        arrange_objects(fixed_now);
        if (cancelled())
            break;

        std::set<int> beds_with_objects;
        for (const ArrangeItem &itm : items)
            if (arr2::is_arranged(itm))
                beds_with_objects.insert(arr2::get_bed_index(itm));
        std::vector<size_t>      moving; // index in towers
        std::vector<ArrangeItem> moved;
        for (size_t t = 0; t < towers.size(); ++t)
            if (!tower_fixed[t] && beds_with_objects.count(*arr2::get_bed_constraint(towers[t])) > 0) {
                moving.emplace_back(t);
                moved.emplace_back(towers[t]);
            }
        if (moved.empty())
            break;

        std::vector<ArrangeItem> fixed_towers = fixed_now;
        for (const ArrangeItem &itm : items)
            if (arr2::is_arranged(itm))
                fixed_towers.emplace_back(itm);
        std::vector<int> order_towers(moved.size(), -1);
        boost::apply_visitor([&](const auto &b) {
            arrange_on_bed(moved, fixed_towers, b, nullptr, alignment, params, pparams.max_bed_count, order_towers, bedbb, nullptr);
        }, tower_bed);
        if (cancelled())
            break;

        bool no_room = false;
        for (size_t j = 0; j < moved.size(); ++j)
            if (!arr2::is_arranged(moved[j])) {
                tower_fixed[moving[j]] = true;
                no_room                = true;
                BOOST_LOG_TRIVIAL(info) << "arrange (PrusaSlicer engine): no room for " << selected[tower_src[moving[j]]].name
                                        << " beside the objects of its plate, kept in place";
            }
        if (no_room)
            continue;

        // The whole aligned again, as the objects alone were (not on a circular plate: kept as packed there).
        if (boost::get<arr2::CircleBed>(&bed) == nullptr) {
            const size_t object_count = items.size();
            items.insert(items.end(), moved.begin(), moved.end());
            const auto *irregular = boost::get<arr2::IrregularBed>(&bed);
            align_piles(items, fixed_now, bedbb, alignment, params.align_center, irregular != nullptr ? &irregular->poly : nullptr,
                        pparams.tower_area.empty() ? nullptr : &pparams.tower_area);
            moved.assign(items.begin() + object_count, items.end());
            items.erase(items.begin() + object_count, items.end());
        }
        for (size_t j = 0; j < moved.size(); ++j) {
            ArrangePolygon &ap = selected[tower_src[moving[j]]];
            ap.bed_idx         = arr2::get_bed_index(moved[j]);
            ap.translation     = arr2::get_translation(moved[j]);
        }
        BOOST_LOG_TRIVIAL(info) << "arrange (PrusaSlicer engine): " << moved.size() << " wipe towers moved beside the objects";
        break;
    }

    // Result, as arrangement::arrange() gives it. An item left unarranged keeps its position: ArrangeJob puts it
    // beside the plates.
    size_t unarranged = 0;
    for (size_t k = 0; k < items.size(); ++k) {
        ArrangePolygon    &ap  = selected[item_src[k]];
        const ArrangeItem &itm = items[k];
        ap.bed_idx             = arr2::get_bed_index(itm);
        if (ap.bed_idx >= 0) {
            ap.translation = arr2::get_translation(itm);
            ap.rotation    = arr2::get_rotation(itm);
            ap.itemid      = packing_order[k];
        } else {
            ap.bed_idx = arrangement::UNARRANGED;
            ++unarranged;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "arrange (PrusaSlicer engine): done, " << unarranged << " items unarranged";
}

}} // namespace Slic3r::GUI
