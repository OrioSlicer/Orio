#ifndef slic3r_InfillWallsProfile_hpp_
#define slic3r_InfillWallsProfile_hpp_

// Orio: "Variable infill and walls" tool.
// The user paints, along the Z axis of a single object, the sparse infill density or the number of walls,
// with the same vertical bar as the variable layer height and variable speed tools. Contrary to the
// variable speed (applied at G-code export), these values change the geometry of each layer: they are
// applied layer by layer while generating the perimeters and the infill.
// The same quantities can be painted along the X and Y axes of the object as well (PaintAxis.hpp), with the
// bar laid along the bottom of the 3D scene: the highest of the painted values applies at each point.

#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include "PaintAxis.hpp"
#include "Slicing.hpp"

namespace Slic3r {

class Layer;
class LayerRegion;
class PrintObject;
class PrintRegionConfig;
struct LoopNode;

// Quantities that can be painted. Each one has its own independent <z, value> curve.
enum InfillWallsTarget : uint32_t {
    // Sparse infill density, in percent.
    iwtInfillDensity = 1 << 0,
    // Number of walls (wall_loops).
    iwtWallLoops     = 1 << 1,
    iwtAll           = iwtInfillDensity | iwtWallLoops,
};

// Limits accepted by the interactive editor and by the slicer.
static constexpr coordf_t MIN_VARIABLE_INFILL_DENSITY = 0.;
static constexpr coordf_t MAX_VARIABLE_INFILL_DENSITY = 100.;
static constexpr coordf_t MIN_VARIABLE_WALL_LOOPS     = 0.;
static constexpr coordf_t MAX_VARIABLE_WALL_LOOPS     = 50.;
// Along X / Y the density stays within 1..99 %: 0 % (no infill) and 100 % (solid infill) change how a whole
// layer is processed, so a layer is void or solid only through the curve painted along Z.
static constexpr coordf_t MIN_VARIABLE_INFILL_DENSITY_XY = 1.;
static constexpr coordf_t MAX_VARIABLE_INFILL_DENSITY_XY = 99.;

// Painted curves of one object. Each curve packs <z, value> pairs into a 1D array, z being relative to the
// bottom of the object (raft excluded) like the layer height and the variable speed profiles. A quantity
// without a curve keeps the value of the print profile. An empty map = no override at all.
// The curves painted along X / Y pack <coordinate, value> pairs, the coordinate being in the frame of the
// object (PaintAxis.hpp).
struct InfillWallsProfile
{
    // Key = paint_axis_key(InfillWallsTarget bit, axis): the bare bit for a curve painted along Z.
    // Value = the <z, value> (or <coordinate, value>) curve of that quantity.
    std::map<uint32_t, std::vector<coordf_t>> curves;
    // Bounds the painting does not cross (11/10), per curve (same key), sorted, in the coordinates of the curve. They
    // only guide the editor: the slicer ignores them.
    std::map<uint32_t, std::vector<coordf_t>> bounds;

    bool empty() const throw() { return curves.empty(); }
    void clear() { curves.clear(); bounds.clear(); }

    // Has this quantity been painted (along Z)?
    bool     has(InfillWallsTarget target) const;
    // Has this quantity been painted along this axis?
    bool     has(InfillWallsTarget target, PaintAxis axis) const;
    // Interpolated value of a quantity at z, or fallback when it has not been painted.
    coordf_t value_at(InfillWallsTarget target, coordf_t z, coordf_t fallback) const;

    bool operator==(const InfillWallsProfile &rhs) const { return this->curves == rhs.curves && this->bounds == rhs.bounds; }
    bool operator!=(const InfillWallsProfile &rhs) const { return ! (*this == rhs); }

    template<class Archive> void serialize(Archive &ar) { ar(curves, bounds); }
};

// Limits of a quantity, see MIN_ / MAX_VARIABLE_* above.
extern coordf_t infill_walls_min_value(InfillWallsTarget target);
extern coordf_t infill_walls_max_value(InfillWallsTarget target);
// Limits of a quantity painted along an axis.
extern coordf_t infill_walls_min_value(InfillWallsTarget target, PaintAxis axis);
extern coordf_t infill_walls_max_value(InfillWallsTarget target, PaintAxis axis);

// Editing helpers used by the interactive tool. Same behaviour as their variable speed counterparts
// (adjust_variable_speed_profile() etc.), without the speed limits: a value may go down to 0. The smoothing and the
// bounds use the variable speed ones, which take any <z, value> curve: smooth_variable_speed_profile_bounded(),
// variable_speed_clip_stroke().

// A flat curve spanning the whole object.
extern std::vector<coordf_t> infill_walls_curve_flat(coordf_t object_height, coordf_t value);
// Linear interpolation of a raw <z, value> curve, returns fallback when the curve is unusable.
extern coordf_t infill_walls_curve_value_at(const std::vector<coordf_t> &curve, coordf_t z, coordf_t fallback);
// Same semantics as adjust_layer_height_profile(): increase / decrease by delta with a cosine falloff over
// band_width, or bring back to base_value (REDUCE / SMOOTH). Values are clamped to [min_value, max_value]. Only
// the curve within [paint_from, paint_to] changes: along X / Y, the walls at both ends of the object, which hold
// no infill, are not painted.
extern void adjust_infill_walls_curve(
    std::vector<coordf_t>       &curve,
    coordf_t                     object_height,
    coordf_t                     base_value,
    coordf_t                     min_value,
    coordf_t                     max_value,
    coordf_t                     z,
    coordf_t                     delta,
    coordf_t                     band_width,
    LayerHeightEditActionType    action,
    coordf_t                     paint_from = 0.,
    coordf_t                     paint_to   = std::numeric_limits<coordf_t>::max());
// Produce a 1D texture packed into a 2D texture coloring the object by the painted value, using the very
// same layout as generate_layer_height_texture() so that the variable_layer_height shader can be reused.
// round_values: color by the rounded value (the wall count is an integer).
// Returns number of cells used by the texture of the 0th LOD level.
extern int generate_infill_walls_texture(
    coordf_t                     object_height,
    coordf_t                     min_value,
    coordf_t                     max_value,
    const std::vector<coordf_t> &curve,
    bool                         round_values,
    void *data, int rows, int cols, bool level_of_detail_2nd_level);

// Slicing: values to use for one layer region. When the object has no painted curve for that quantity, or
// when the region carries its own value (a part or a modifier whose setting differs from the object's
// value), the region value is returned untouched: the painted curve only replaces the object's value.
// When the quantity is painted along X / Y, the value varies over the layer: these return the lowest one,
// infill_walls_density_zones() and infill_walls_make_zone_walls() below apply the others.
extern int      infill_walls_wall_loops(const Layer &layer, const PrintRegionConfig &region_config);
extern coordf_t infill_walls_density(const Layer &layer, const PrintRegionConfig &region_config);

// Painting along X / Y. A zone of a layer where a painted quantity takes a single value: that value and the
// zone, in the frame of the slices of the layer (to be clipped with them).
struct InfillWallsZone
{
    coordf_t   value;
    ExPolygons area;
};
using InfillWallsZones = std::vector<InfillWallsZone>;
// The sparse infill density zones of a layer region, by increasing density. Empty when the density is the same
// all over the layer, infill_walls_density() then gives it.
extern InfillWallsZones infill_walls_density_zones(const Layer &layer, const PrintRegionConfig &region_config);
// The walls of a layer region are generated with the fewest walls painted on the layer (infill_walls_wall_loops()).
// This adds inside them the walls of the zones painted with more. Generated over area, the area left to the infill
// around all the regions sharing their walls (the region's own one when it is alone), then kept in the zone and in
// the region: they follow the contour of the object, across the boundaries of the modifiers, and stop at the zone
// boundaries; the area left to the infill shrinks accordingly. Nothing to do unless walls are painted along X / Y.
extern void     infill_walls_make_zone_walls(LayerRegion &layerm, const ExPolygons &area, std::vector<LoopNode> &loop_nodes);
// Are walls painted along X / Y on the object of this layer? infill_walls_make_zone_walls() has nothing to do otherwise.
extern bool     infill_walls_has_zone_walls(const Layer &layer);
// Does the painted infill density of this object go down to 0 % somewhere?
extern bool     infill_walls_has_void_density(const PrintObject &object);
// Does switching from one profile to the other require regenerating the perimeters? True when the wall
// curve changed, or when a changed density curve reaches 0 % or 100 % (like a sparse_infill_density change
// in the print profile). Otherwise only the infill has to be prepared again.
extern bool     infill_walls_profile_changes_perimeters(const InfillWallsProfile &old_profile, const InfillWallsProfile &new_profile);

} // namespace Slic3r

#endif /* slic3r_InfillWallsProfile_hpp_ */
