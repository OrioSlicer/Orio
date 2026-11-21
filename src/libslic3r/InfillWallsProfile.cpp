// Orio: "Variable infill and walls" tool, see InfillWallsProfile.hpp.

#include "InfillWallsProfile.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

#include "AABBTreeLines.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "PerimeterGenerator.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

bool InfillWallsProfile::has(InfillWallsTarget target) const
{
    return this->has(target, paZ);
}

bool InfillWallsProfile::has(InfillWallsTarget target, PaintAxis axis) const
{
    const auto it = this->curves.find(paint_axis_key((uint32_t)target, axis));
    return it != this->curves.end() && it->second.size() >= 4 && (it->second.size() & 1) == 0;
}

coordf_t InfillWallsProfile::value_at(InfillWallsTarget target, coordf_t z, coordf_t fallback) const
{
    const auto it = this->curves.find((uint32_t)target);
    return (it == this->curves.end()) ? fallback : infill_walls_curve_value_at(it->second, z, fallback);
}

coordf_t infill_walls_min_value(InfillWallsTarget target)
{
    return (target == iwtWallLoops) ? MIN_VARIABLE_WALL_LOOPS : MIN_VARIABLE_INFILL_DENSITY;
}

coordf_t infill_walls_max_value(InfillWallsTarget target)
{
    return (target == iwtWallLoops) ? MAX_VARIABLE_WALL_LOOPS : MAX_VARIABLE_INFILL_DENSITY;
}

coordf_t infill_walls_min_value(InfillWallsTarget target, PaintAxis axis)
{
    return (target == iwtInfillDensity && axis != paZ) ? MIN_VARIABLE_INFILL_DENSITY_XY : infill_walls_min_value(target);
}

coordf_t infill_walls_max_value(InfillWallsTarget target, PaintAxis axis)
{
    return (target == iwtInfillDensity && axis != paZ) ? MAX_VARIABLE_INFILL_DENSITY_XY : infill_walls_max_value(target);
}

// ------------------------------------------------------------------------------------------------
// Editing helpers. They mirror the variable speed ones (Slicing.cpp), which themselves mirror the
// variable layer height machinery, so that the three tools behave exactly the same under the mouse.
// ------------------------------------------------------------------------------------------------

std::vector<coordf_t> infill_walls_curve_flat(coordf_t object_height, coordf_t value)
{
    const coordf_t top = std::max(object_height, coordf_t(2. * EPSILON));
    return { 0., value, top, value };
}

coordf_t infill_walls_curve_value_at(const std::vector<coordf_t> &curve, coordf_t z, coordf_t fallback)
{
    if (curve.size() < 4 || (curve.size() & 1) != 0)
        return fallback;
    if (z <= curve.front())
        return curve[1];
    if (z >= curve[curve.size() - 2])
        return curve.back();
    // Binary search of the segment containing z: a curve painted over a tall object holds thousands of
    // points and the slicer queries it for every layer region. Invariant: z(lo) < z <= z(hi).
    size_t lo = 0;
    size_t hi = curve.size() / 2 - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (curve[2 * mid] < z)
            lo = mid;
        else
            hi = mid;
    }
    const coordf_t z1 = curve[2 * lo];
    const coordf_t v1 = curve[2 * lo + 1];
    const coordf_t z2 = curve[2 * hi];
    const coordf_t v2 = curve[2 * hi + 1];
    const coordf_t dz = z2 - z1;
    return (dz > EPSILON) ? lerp(v1, v2, (z - z1) / dz) : v2;
}

// Returns true when the curve is well formed and spans exactly [0, object_height].
static bool infill_walls_curve_matches(const std::vector<coordf_t> &curve, coordf_t object_height)
{
    return curve.size() >= 4 && (curve.size() & 1) == 0 &&
           std::abs(curve.front()) < EPSILON &&
           std::abs(curve[curve.size() - 2] - object_height) < 1e-3;
}

void adjust_infill_walls_curve(
    std::vector<coordf_t>       &curve,
    coordf_t                     object_height,
    coordf_t                     base_value,
    coordf_t                     min_value,
    coordf_t                     max_value,
    coordf_t                     z,
    coordf_t                     delta,
    coordf_t                     band_width,
    LayerHeightEditActionType    action,
    coordf_t                     paint_from,
    coordf_t                     paint_to)
{
    if (object_height < EPSILON || z < 0. || z > object_height)
        return;

    max_value  = std::max(max_value, min_value + EPSILON);
    base_value = std::clamp(base_value, min_value, max_value);

    // The curve may be empty or may have been generated for another object height.
    if (! infill_walls_curve_matches(curve, object_height))
        curve = infill_walls_curve_flat(object_height, base_value);

    // 1) Current value at z.
    const coordf_t current_value = infill_walls_curve_value_at(curve, z, base_value);

    // 2) Is it possible to apply the delta?
    switch (action) {
    case LAYER_HEIGHT_EDIT_ACTION_DECREASE:
        delta = - delta;
        // fallthrough
    case LAYER_HEIGHT_EDIT_ACTION_INCREASE:
        if (delta > 0) {
            if (current_value >= max_value - EPSILON)
                return;
            delta = std::min(delta, max_value - current_value);
        } else {
            if (current_value <= min_value + EPSILON)
                return;
            delta = std::max(delta, min_value - current_value);
        }
        break;
    case LAYER_HEIGHT_EDIT_ACTION_REDUCE:
    case LAYER_HEIGHT_EDIT_ACTION_SMOOTH:
        delta = std::abs(delta);
        delta = std::min(delta, std::abs(base_value - current_value));
        if (delta < EPSILON)
            return;
        break;
    default:
        assert(false);
        break;
    }

    // 3) Densify the curve inside z +- band_width / 2 and apply the delta with a cosine falloff.
    const coordf_t lo = std::max(coordf_t(0.), z - 0.5 * band_width);
    const coordf_t hi = z + 0.5 * band_width;
    // Sample the band finely enough to follow the cosine falloff without generating thousands of
    // points for a wide band.
    const coordf_t z_step = std::max(coordf_t(0.1), band_width / 40.);

    size_t idx = 0;
    while (idx < curve.size() && curve[idx] < lo)
        idx += 2;
    idx = (idx >= 2) ? idx - 2 : 0;

    std::vector<coordf_t> curve_new;
    curve_new.reserve(curve.size() + 128);
    curve_new.insert(curve_new.end(), curve.begin(), curve.begin() + idx + 2);

    coordf_t zz = lo;
    const size_t i_resampled_start = curve_new.size();
    while (zz < hi) {
        const size_t next = idx + 2;
        coordf_t value = curve[idx + 1];
        if (next < curve.size()) {
            const coordf_t z1 = curve[idx];
            const coordf_t z2 = curve[next];
            const coordf_t v2 = curve[next + 1];
            value = (z2 - z1 > EPSILON) ? lerp(value, v2, (zz - z1) / (z2 - z1)) : v2;
        }
        // Outside [paint_from, paint_to] the curve keeps its value.
        const coordf_t weight = (paint_from <= zz && zz <= paint_to && std::abs(zz - z) < 0.5 * band_width) ?
            (0.5 + 0.5 * cos(2. * M_PI * (zz - z) / band_width)) : 0.;
        switch (action) {
        case LAYER_HEIGHT_EDIT_ACTION_INCREASE:
        case LAYER_HEIGHT_EDIT_ACTION_DECREASE:
            value += weight * delta;
            break;
        case LAYER_HEIGHT_EDIT_ACTION_REDUCE:
        {
            const coordf_t diff = value - base_value;
            coordf_t step = weight * delta;
            step = (std::abs(diff) > step) ? ((diff > 0) ? -step : step) : -diff;
            value += step;
            break;
        }
        case LAYER_HEIGHT_EDIT_ACTION_SMOOTH:
            // Don't modify the curve during the resampling, it is done in the pass below.
            break;
        default:
            assert(false);
            break;
        }
        value = std::clamp(value, min_value, max_value);

        if (zz >= object_height - EPSILON) {
            // This is the last point of the curve.
            if (curve_new[curve_new.size() - 2] + EPSILON > object_height) {
                curve_new.pop_back();
                curve_new.pop_back();
            }
            curve_new.push_back(object_height);
            curve_new.push_back(value);
            idx = curve.size();
            break;
        }
        // Avoid entering a too short segment.
        if (curve_new[curve_new.size() - 2] + EPSILON < zz) {
            curve_new.push_back(zz);
            curve_new.push_back(value);
        }
        zz = std::min(zz + z_step, object_height);
        idx = next;
        while (idx < curve.size() && curve[idx] < zz)
            idx += 2;
        idx = (idx >= 2) ? idx - 2 : 0;
    }

    idx += 2;
    const size_t i_resampled_end = curve_new.size();
    if (idx < curve.size())
        curve_new.insert(curve_new.end(), curve.begin() + idx, curve.end());
    else if (curve_new[curve_new.size() - 2] + 0.5 * EPSILON < object_height)
        curve_new.insert(curve_new.end(), curve.end() - 2, curve.end());
    curve = std::move(curve_new);

    if (action == LAYER_HEIGHT_EDIT_ACTION_SMOOTH && curve.size() >= 6) {
        const size_t begin = (i_resampled_start < 2) ? 2 : i_resampled_start;
        const size_t end   = std::min(i_resampled_end, curve.size() - 2);
        for (size_t i_round = 0; i_round < 6; ++ i_round) {
            const std::vector<coordf_t> src = curve;
            for (size_t i = begin; i + 3 < src.size(); i += 2) {
                if (i >= end)
                    break;
                const coordf_t zi = src[i];
                const coordf_t t  = (paint_from <= zi && zi <= paint_to && std::abs(zi - z) < 0.5 * band_width) ?
                    (0.25 + 0.25 * cos(2. * M_PI * (zi - z) / band_width)) : 0.;
                curve[i + 1] = (1. - t) * src[i + 1] + 0.5 * t * (src[i - 1] + src[i + 3]);
            }
        }
    }

    // Make sure the curve stays sane whatever happened above: strictly increasing z, values in range,
    // spanning exactly [0, object_height]. The interpolation relies on all of it.
    std::vector<coordf_t> sane;
    sane.reserve(curve.size());
    for (size_t i = 0; i + 1 < curve.size(); i += 2) {
        const coordf_t value = std::clamp(curve[i + 1], min_value, max_value);
        if (! sane.empty() && curve[i] <= sane[sane.size() - 2] + EPSILON) {
            // Duplicated or decreasing z, keep the latest value.
            sane[sane.size() - 1] = value;
            continue;
        }
        sane.push_back(curve[i]);
        sane.push_back(value);
    }
    if (sane.size() < 4)
        sane = infill_walls_curve_flat(object_height, base_value);
    sane.front() = 0.;
    sane[sane.size() - 2] = object_height;
    curve = std::move(sane);

    assert(curve.size() >= 4);
    assert(curve.size() % 2 == 0);
}

int generate_infill_walls_texture(
    coordf_t                     object_height,
    coordf_t                     min_value,
    coordf_t                     max_value,
    const std::vector<coordf_t> &curve,
    bool                         round_values,
    void *data, int rows, int cols, bool level_of_detail_2nd_level)
{
    // Cool (sparse / few walls) to hot (dense / many walls), the palette of the variable speed tool.
    std::vector<Vec3crd> palette_raw;
    palette_raw.push_back(Vec3crd(0x02C, 0x07B, 0x0B6));
    palette_raw.push_back(Vec3crd(0x000, 0x0A6, 0x0CA));
    palette_raw.push_back(Vec3crd(0x07F, 0x0CD, 0x0BB));
    palette_raw.push_back(Vec3crd(0x0C7, 0x0E9, 0x0B4));
    palette_raw.push_back(Vec3crd(0x0FF, 0x0FF, 0x0BF));
    palette_raw.push_back(Vec3crd(0x0FE, 0x0E0, 0x090));
    palette_raw.push_back(Vec3crd(0x0FD, 0x0AE, 0x061));
    palette_raw.push_back(Vec3crd(0x0D7, 0x030, 0x027));

    object_height = std::max(object_height, coordf_t(EPSILON));
    max_value     = std::max(max_value, min_value + EPSILON);
    const coordf_t span = max_value - min_value;

    unsigned char *data1 = reinterpret_cast<unsigned char*>(data) + rows * cols * 4;
    const int cols1 = cols / 2;
    // Both levels of detail have to fit into their part of the texture.
    const int max_cells_lod0 = (cols - 1) * rows;
    const int max_cells_lod1 = (cols1 - 1) * (rows / 2);
    // 200 samples per millimeter is far more than the smooth curve needs.
    int ncells = std::max(2, int(ceil(200. * object_height)));
    ncells = std::min(ncells, max_cells_lod0);
    ncells = std::min(ncells, 2 * max_cells_lod1);
    const int ncells1 = std::max(2, ncells / 2);

    const coordf_t cell_to_z  = object_height / coordf_t(ncells - 1);
    const coordf_t cell_to_z1 = object_height / coordf_t(ncells1 - 1);

    auto write_cell = [&palette_raw](unsigned char *base, int cell, int cols_, coordf_t t01) {
        const coordf_t idxf = std::clamp(t01, coordf_t(0.), coordf_t(1.)) * coordf_t(palette_raw.size() - 1);
        const int idx1 = std::clamp(int(floor(idxf)), 0, int(palette_raw.size() - 1));
        const int idx2 = std::min(int(palette_raw.size() - 1), idx1 + 1);
        const coordf_t t = idxf - coordf_t(idx1);
        const Vec3crd &color1 = palette_raw[idx1];
        const Vec3crd &color2 = palette_raw[idx2];
        const int row = cell / (cols_ - 1);
        const int col = cell - row * (cols_ - 1);
        unsigned char *ptr = base + (row * cols_ + col) * 4;
        ptr[0] = (unsigned char)std::clamp(int(floor(lerp(coordf_t(color1(0)), coordf_t(color2(0)), t) + 0.5)), 0, 255);
        ptr[1] = (unsigned char)std::clamp(int(floor(lerp(coordf_t(color1(1)), coordf_t(color2(1)), t) + 0.5)), 0, 255);
        ptr[2] = (unsigned char)std::clamp(int(floor(lerp(coordf_t(color1(2)), coordf_t(color2(2)), t) + 0.5)), 0, 255);
        ptr[3] = 255;
        if (col == 0 && row > 0) {
            // Duplicate the first value in a row as a last value of the preceding row.
            ptr[-4] = ptr[0];
            ptr[-3] = ptr[1];
            ptr[-2] = ptr[2];
            ptr[-1] = ptr[3];
        }
    };
    auto value_at = [&curve, min_value, round_values](coordf_t z) {
        const coordf_t v = infill_walls_curve_value_at(curve, z, min_value);
        return round_values ? coordf_t(std::lround(v)) : v;
    };

    for (int cell = 0; cell < ncells; ++ cell)
        write_cell(reinterpret_cast<unsigned char*>(data), cell, cols, (value_at(cell_to_z * coordf_t(cell)) - min_value) / span);

    if (level_of_detail_2nd_level) {
        for (int cell = 0; cell < ncells1; ++ cell)
            write_cell(data1, cell, cols1, (value_at(cell_to_z1 * coordf_t(cell)) - min_value) / span);
    }

    // Returns number of cells of the 0th LOD level.
    return ncells;
}

// ------------------------------------------------------------------------------------------------
// Slicing.
// ------------------------------------------------------------------------------------------------

// The curve of a quantity painted along an axis for the object of this layer, or nullptr when it was not painted.
static const std::vector<coordf_t>* infill_walls_painted_curve(const Layer &layer, InfillWallsTarget target, PaintAxis axis = paZ)
{
    const PrintObject *object       = layer.object();
    const ModelObject *model_object = (object != nullptr) ? object->model_object() : nullptr;
    if (model_object == nullptr)
        return nullptr;
    const std::map<uint32_t, std::vector<coordf_t>> &curves = model_object->infill_walls_profile.curves;
    const auto it = curves.find(paint_axis_key((uint32_t)target, axis));
    if (it == curves.end() || it->second.size() < 4 || (it->second.size() & 1) != 0)
        return nullptr;
    return &it->second;
}

// Is this quantity painted at all, along any axis, on the object of this layer?
static bool infill_walls_is_painted(const Layer &layer, InfillWallsTarget target)
{
    for (PaintAxis axis : { paZ, paX, paY })
        if (infill_walls_painted_curve(layer, target, axis) != nullptr)
            return true;
    return false;
}

// z of a layer in the coordinates of a painted curve: top of the layer, relative to the bottom of the object
// (raft excluded) like the variable speed tool does. When the object height differs from the height the curve
// was painted for (the object was scaled afterwards), the curve is stretched, exactly as the tool displays it.
static coordf_t infill_walls_layer_z(const Layer &layer, const std::vector<coordf_t> &curve)
{
    const SlicingParameters &slicing_params = layer.object()->slicing_parameters();
    coordf_t z = layer.print_z - slicing_params.object_print_z_min;
    coordf_t object_height = slicing_params.object_print_z_height();
    //Orio: tilted layers, the curve is in real heights: the one of the layer at the middle of the object.
    // ponytail: one height per layer, split tilted layers into zones by real z if narrow Z bands get smeared.
    const TiltedLayersParams &tilted = layer.object()->tilted_params();
    if (tilted.enabled) {
        z = tilted.real_z_w(tilted.w_center, z);
        object_height -= tilted.warped_max_z - tilted.real_max_z;
    }
    const coordf_t curve_height  = curve[curve.size() - 2];
    if (object_height > EPSILON && curve_height > EPSILON && std::abs(object_height - curve_height) > 1e-3)
        z *= curve_height / object_height;
    return z;
}

// The object's own value of a region option (the one the tool shows as its base): the print profile value,
// overridden by the object settings if any, before any part, modifier or height range setting.
static const ConfigOption* infill_walls_object_option(const PrintObject &object, const char *key)
{
    const ModelObject *model_object = object.model_object();
    return (model_object != nullptr) ? model_object->config.option(key) : nullptr;
}

// Same clamping as region_config_from_model_volume().
static coordf_t infill_walls_sanitize_density(coordf_t density)
{
    return (density < 0.00011) ? 0. : std::min(density, MAX_VARIABLE_INFILL_DENSITY);
}

// Does this region carry the object's own value of a quantity, the one the painted curves replace? A part, a
// modifier or a height range with its own value keeps it.
static bool infill_walls_region_follows_object(const Layer &layer, const PrintRegionConfig &region_config, InfillWallsTarget target)
{
    const PrintObject &object = *layer.object();
    if (target == iwtWallLoops) {
        const ConfigOption *opt = infill_walls_object_option(object, "wall_loops");
        const int object_value = (opt != nullptr) ? opt->getInt() : object.print()->default_region_config().wall_loops.value;
        return region_config.wall_loops.value == object_value;
    }
    // These patterns are generated once per object from the density of the region (octree, lightning trees):
    // a painted density cannot change them, and a region without infill has no generator at all.
    const InfillPattern pattern = region_config.sparse_infill_pattern.value;
    if (pattern == ipAdaptiveCubic || pattern == ipSupportCubic || pattern == ipLightning)
        return false;
    const ConfigOption *opt = infill_walls_object_option(object, "sparse_infill_density");
    const coordf_t object_value = infill_walls_sanitize_density(
        (opt != nullptr) ? opt->getFloat() : object.print()->default_region_config().sparse_infill_density.value);
    return std::abs(region_config.sparse_infill_density.value - object_value) <= EPSILON;
}

// ------------------------------------------------------------------------------------------------
// Painting along X / Y.
// ------------------------------------------------------------------------------------------------

// The density painted along X / Y is grouped into bands of +-2.5 % at least 2 mm wide, the walls into bands of a
// whole number of walls at least 1 mm wide: a few infill areas and wall ends rather than countless slivers.
static constexpr double INFILL_WALLS_XY_DENSITY_TOLERANCE = 2.5;
static constexpr double INFILL_WALLS_XY_DENSITY_MIN_WIDTH = 2.;
static constexpr double INFILL_WALLS_XY_WALLS_MIN_WIDTH   = 1.;

namespace {

// A quantity of a layer painted along X and / or Y: the value painted along Z at this layer if any, and for each
// painted axis the bands of its curve with the plane giving their coordinate on the layer. The value at a point
// is the highest of them.
struct InfillWallsAxisValues
{
    struct Axis
    {
        PaintAxisFrame::Plane plane;
        PaintAxisBands        bands;
    };
    bool              has_z   { false };
    coordf_t          z_value { 0. };
    std::vector<Axis> axes;

    coordf_t value_at(const Point &p) const
    {
        coordf_t v = this->has_z ? this->z_value : std::numeric_limits<coordf_t>::lowest();
        for (const Axis &axis : this->axes)
            v = std::max(v, axis.bands[paint_axis_band_index(axis.bands, axis.plane(p))].value);
        return v;
    }

    // Lowest value over a bounding box. A lower bound, exact when the lowest bands of the axes cross over the box.
    coordf_t min_value(const BoundingBox &bbox) const
    {
        coordf_t v = this->has_z ? this->z_value : std::numeric_limits<coordf_t>::lowest();
        for (const Axis &axis : this->axes) {
            const std::pair<double, double> range  = axis.plane.range(bbox);
            coordf_t                        lowest = std::numeric_limits<coordf_t>::max();
            for (size_t i = paint_axis_band_index(axis.bands, range.first); i < axis.bands.size(); ++ i) {
                lowest = std::min(lowest, axis.bands[i].value);
                if (axis.bands[i].to >= range.second)
                    break;
            }
            v = std::max(v, lowest);
        }
        return v;
    }

    // The zones of constant value over a bounding box, by increasing value.
    InfillWallsZones zones(const BoundingBox &bbox) const
    {
        // The bands of each axis over the box, with the value they give (the Z value may top them).
        std::vector<std::pair<coordf_t, Polygon>> strips;
        std::vector<coordf_t>                     values;
        for (const Axis &axis : this->axes)
            for (const PaintAxisBand &band : axis.bands) {
                Polygon polygon = axis.plane.band(bbox, band.from, band.to);
                if (polygon.empty())
                    continue;
                const coordf_t v = this->has_z ? std::max(this->z_value, band.value) : band.value;
                values.push_back(v);
                strips.emplace_back(v, std::move(polygon));
            }
        sort_remove_duplicates(values);
        // The value is at least v where a band of any axis is at least v: the zone of value v is the area of at least
        // v minus the area of at least the next value.
        InfillWallsZones out;
        Polygons         above;
        for (size_t i = values.size(); i -- > 0;) {
            Polygons at_least;
            for (const std::pair<coordf_t, Polygon> &strip : strips)
                if (strip.first >= values[i])
                    at_least.push_back(strip.second);
            at_least = union_(at_least);
            ExPolygons area = above.empty() ? union_ex(at_least) : diff_ex(at_least, above);
            if (! area.empty())
                out.push_back({ values[i], std::move(area) });
            above = std::move(at_least);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }
};

} // namespace

// Bounding box of the islands of a layer.
static BoundingBox infill_walls_layer_bbox(const Layer &layer)
{
    BoundingBox bbox = get_extents(layer.lslices);
    if (! bbox.defined)
        // No island (yet): the whole object.
        bbox = layer.object()->bounding_box();
    return bbox;
}

// The quantity painted along X / Y on the object of this layer, nullopt when it is not painted along X nor Y.
static std::optional<InfillWallsAxisValues> infill_walls_axis_values(const Layer &layer, InfillWallsTarget target)
{
    std::optional<InfillWallsAxisValues> out;
    const bool walls = target == iwtWallLoops;
    for (PaintAxis axis : { paX, paY }) {
        const std::vector<coordf_t> *painted = infill_walls_painted_curve(layer, target, axis);
        if (painted == nullptr)
            continue;
        std::vector<coordf_t> curve = *painted;
        const coordf_t        lo    = infill_walls_min_value(target, axis);
        const coordf_t        hi    = infill_walls_max_value(target, axis);
        for (size_t i = 1; i < curve.size(); i += 2)
            curve[i] = std::clamp(curve[i], lo, hi);
        PaintAxisBands bands = paint_axis_bands(curve, walls, INFILL_WALLS_XY_DENSITY_TOLERANCE,
            walls ? INFILL_WALLS_XY_WALLS_MIN_WIDTH : INFILL_WALLS_XY_DENSITY_MIN_WIDTH);
        if (bands.empty())
            continue;
        if (! out)
            out.emplace();
        out->axes.push_back({ PaintAxisFrame(*layer.object(), axis).plane(layer.slice_z), std::move(bands) });
    }
    if (out) {
        if (const std::vector<coordf_t> *curve = infill_walls_painted_curve(layer, target, paZ); curve != nullptr) {
            const coordf_t v = std::clamp(infill_walls_curve_value_at(*curve, infill_walls_layer_z(layer, *curve), 0.),
                                          infill_walls_min_value(target), infill_walls_max_value(target));
            out->has_z   = true;
            out->z_value = walls ? std::round(v) : infill_walls_sanitize_density(v);
        }
    }
    return out;
}

int infill_walls_wall_loops(const Layer &layer, const PrintRegionConfig &region_config)
{
    const int region_value = region_config.wall_loops.value;
    if (! infill_walls_is_painted(layer, iwtWallLoops) || ! infill_walls_region_follows_object(layer, region_config, iwtWallLoops))
        // Not painted, or a part or a modifier with its own wall count, which keeps it.
        return region_value;
    if (const std::optional<InfillWallsAxisValues> values = infill_walls_axis_values(layer, iwtWallLoops); values)
        // Painted along X / Y: the fewest walls of the layer, infill_walls_make_zone_walls() adds the others.
        return std::clamp(int(std::lround(values->min_value(infill_walls_layer_bbox(layer)))), int(MIN_VARIABLE_WALL_LOOPS), int(MAX_VARIABLE_WALL_LOOPS));
    const std::vector<coordf_t> &curve = *infill_walls_painted_curve(layer, iwtWallLoops, paZ);
    const coordf_t v = infill_walls_curve_value_at(curve, infill_walls_layer_z(layer, curve), coordf_t(region_value));
    return std::clamp(int(std::lround(v)), int(MIN_VARIABLE_WALL_LOOPS), int(MAX_VARIABLE_WALL_LOOPS));
}

coordf_t infill_walls_density(const Layer &layer, const PrintRegionConfig &region_config)
{
    const coordf_t region_value = region_config.sparse_infill_density.value;
    if (! infill_walls_is_painted(layer, iwtInfillDensity) || ! infill_walls_region_follows_object(layer, region_config, iwtInfillDensity))
        // Not painted, a part or a modifier with its own infill density (which keeps it), or a pattern ignoring it.
        return region_value;
    if (const std::optional<InfillWallsAxisValues> values = infill_walls_axis_values(layer, iwtInfillDensity); values)
        // Painted along X / Y: the lowest density of the layer, for the decisions taken for the whole layer. The
        // sparse infill gets the density of each zone (infill_walls_density_zones()).
        return infill_walls_sanitize_density(values->min_value(infill_walls_layer_bbox(layer)));
    const std::vector<coordf_t> &curve = *infill_walls_painted_curve(layer, iwtInfillDensity, paZ);
    return infill_walls_sanitize_density(infill_walls_curve_value_at(curve, infill_walls_layer_z(layer, curve), region_value));
}

InfillWallsZones infill_walls_density_zones(const Layer &layer, const PrintRegionConfig &region_config)
{
    // The locked zag infill shares its densities between the surfaces of the whole layer: it keeps a single one.
    if (! infill_walls_is_painted(layer, iwtInfillDensity) || region_config.sparse_infill_pattern.value == ipLockedZag ||
        ! infill_walls_region_follows_object(layer, region_config, iwtInfillDensity))
        return {};
    const std::optional<InfillWallsAxisValues> values = infill_walls_axis_values(layer, iwtInfillDensity);
    if (! values)
        return {};
    BoundingBox bbox = infill_walls_layer_bbox(layer);
    bbox.offset(scale_(1.));
    InfillWallsZones zones = values->zones(bbox);
    if (zones.size() < 2)
        return {};
    for (InfillWallsZone &zone : zones)
        zone.value = infill_walls_sanitize_density(zone.value);
    return zones;
}

// The walls generated inside the walls of a region for the zones painted with more walls.
struct InfillWallsZoneWalls
{
    ExtrusionEntityCollection perimeters;
    ExtrusionEntityCollection thin_fills;
    SurfaceCollection         fill_surfaces;
    ExPolygons                fill_no_overlap;
};

// The walls generated by infill_walls_generate_walls() are inner walls (the generator made their outermost loops
// external), numbered after the walls the region already has (wall_index, used by the brick layers). Same as
// set_inner_walls() of LayerRegion.cpp.
static void infill_walls_set_inner_walls(ExtrusionEntity &entity, int wall_index_offset)
{
    auto set_path = [wall_index_offset](ExtrusionPath &path) {
        if (path.role() == erExternalPerimeter)
            path.set_extrusion_role(erPerimeter);
        if (path.wall_index >= 0)
            path.wall_index += wall_index_offset;
    };
    if (auto *collection = dynamic_cast<ExtrusionEntityCollection*>(&entity)) {
        for (ExtrusionEntity *child : collection->entities)
            infill_walls_set_inner_walls(*child, wall_index_offset);
    } else if (auto *loop = dynamic_cast<ExtrusionLoop*>(&entity)) {
        for (ExtrusionPath &path : loop->paths)
            set_path(path);
    } else if (auto *multi_path = dynamic_cast<ExtrusionMultiPath*>(&entity)) {
        for (ExtrusionPath &path : multi_path->paths)
            set_path(path);
    } else if (auto *path = dynamic_cast<ExtrusionPath*>(&entity))
        set_path(*path);
}

// wall_loops walls inside the shared_wall_loops walls the region has, over islands of the area left to the infill. Same
// as LayerRegion::make_extra_perimeters(), the results being returned instead of applied: they are cut to the zone.
static InfillWallsZoneWalls infill_walls_generate_walls(const LayerRegion &layerm, int wall_loops, int shared_wall_loops, const ExPolygons &islands,
                                                        std::vector<LoopNode> &loop_nodes)
{
    const Layer             &layer         = *layerm.layer();
    const PrintConfig       &print_config  = layer.object()->print()->config();
    const PrintObjectConfig &object_config = layer.object()->config();

    // Inner walls, inside the walls of the region. Without any (0 walls painted), all the walls of the zone, outer
    // wall included, as make_perimeters() would generate them.
    const bool               inner_walls        = shared_wall_loops > 0;
    const Flow               perimeter_flow     = layerm.flow(frPerimeter);
    const PrintRegionConfig &region_config_base = layerm.region().config();
    PrintRegionConfig        region_config      = region_config_base;
    region_config.wall_loops.value            = wall_loops;
    region_config.sparse_infill_density.value = infill_walls_density(layer, region_config_base);
    if (inner_walls) {
        region_config.precise_outer_wall.value = false;
        if (region_config.fuzzy_skin.value == FuzzySkinType::External || region_config.fuzzy_skin.value == FuzzySkinType::All)
            region_config.fuzzy_skin.value = FuzzySkinType::None;
    }

    // The area left to the infill starts half a wall spacing inside the last wall and the generator puts the first
    // wall half a wall width inside it: grown by the difference, for the first new wall to overlap the last one as usual.
    SurfaceCollection slices;
    slices.append(inner_walls ? offset_ex(islands, float(scale_(0.5 * (perimeter_flow.width() - perimeter_flow.spacing())))) : islands,
                  stInternal);
    InfillWallsZoneWalls   out;
    const PerimeterRegions perimeter_regions;
    PerimeterGenerator g(
        // input:
        &slices,
        layer.height,
        perimeter_flow,
        &region_config,
        &object_config,
        &print_config,
        false,
        // output:
        &out.perimeters,
        &out.thin_fills,
        &out.fill_surfaces,
        &out.fill_no_overlap,
        // The layer's own list: the generated collections index it (Layer::recrod_cooling_node_for_each_extrusion()).
        &loop_nodes
    );
    if (layer.lower_layer != nullptr)
        g.lower_slices = &layer.lower_layer->lslices;
    if (layer.upper_layer != nullptr)
        g.upper_slices = &layer.upper_layer->lslices;
    g.layer_id           = (int)layer.id();
    g.slice_z            = layer.slice_z;
    g.ext_perimeter_flow = inner_walls ? perimeter_flow : layerm.flow(frExternalPerimeter);
    g.overhang_flow      = layerm.bridging_flow(frPerimeter, object_config.thick_bridges);
    g.solid_infill_flow  = layerm.flow(frSolidInfill);
    g.perimeter_regions  = &perimeter_regions;
    if (object_config.wall_generator.value == PerimeterGeneratorType::Arachne)
        g.process_arachne();
    else
        g.process_classic();

    if (inner_walls)
        infill_walls_set_inner_walls(out.perimeters, shared_wall_loops);
    return out;
}

namespace {
// Keeps the parts of extrusions inside a zone, cutting them where they cross its boundary. Same as the
// ExtrusionZoneClipper of LayerRegion.cpp (walls in addition of a modifier, kept in it), every run of pieces being a
// multi-path.
class InfillWallsZoneClipper
{
public:
    explicit InfillWallsZoneClipper(const ExPolygons &zone) : m_bbox(get_extents(zone)), m_boundary(to_lines(zone)) {}

    // In place. The collections are kept (those of the perimeter generator are indexed by the loop nodes of the layer,
    // see Layer::recrod_cooling_node_for_each_extrusion()), the emptied ones removed. A loop or a path crossing the
    // boundary is replaced by its pieces in the zone.
    void clip(ExtrusionEntityCollection &collection) const
    {
        ExtrusionEntitiesPtr out;
        out.reserve(collection.entities.size());
        for (ExtrusionEntity *entity : collection.entities) {
            bool keep = false;
            if (auto *child = dynamic_cast<ExtrusionEntityCollection*>(entity)) {
                this->clip(*child);
                keep = ! child->entities.empty();
            } else if (auto *loop = dynamic_cast<ExtrusionLoop*>(entity))
                keep = this->clip_paths(loop->paths, true, out);
            else if (auto *multi_path = dynamic_cast<ExtrusionMultiPath*>(entity))
                keep = this->clip_paths(multi_path->paths, false, out);
            else if (auto *path = dynamic_cast<ExtrusionPath*>(entity))
                keep = this->clip_paths(ExtrusionPaths{ *path }, false, out);
            else
                keep = this->inside(entity->first_point());
            if (keep)
                out.push_back(entity);
            else
                delete entity;
        }
        collection.entities = std::move(out);
    }

private:
    bool inside(const Point &point) const { return m_bbox.contains(point) && m_boundary.outside(point) <= 0; }

    // The points of a polyline, with the points where it crosses the boundary of the zone.
    Points cut(const Points &points) const
    {
        Points out;
        out.reserve(points.size());
        for (size_t i = 0; i < points.size(); ++ i) {
            if (i > 0)
                for (const auto &crossing : m_boundary.intersections_with_line<true>(Line(points[i - 1], points[i]))) {
                    const Point point(crossing.first);
                    if (point != out.back() && point != points[i])
                        out.push_back(point);
                }
            if (out.empty() || points[i] != out.back())
                out.push_back(points[i]);
        }
        return out;
    }

    // Cuts consecutive paths (a loop when closed) where they cross the boundary of the zone. Returns true when they are
    // all in it, else adds to out their pieces in it: runs of pieces continuing each other, the run through the start of
    // a loop staying in one piece. Each run is a multi-path, even a single piece: the brick layers only take loops and
    // multi-paths for walls. A run shorter than its width, a blob at the boundary, is dropped.
    bool clip_paths(const ExtrusionPaths &paths, bool closed, ExtrusionEntitiesPtr &out) const
    {
        std::vector<ExtrusionPaths> runs;
        bool in_run       = false;
        bool all_inside   = true;
        bool first_known  = false;
        bool first_inside = false;
        bool last_inside  = false;
        for (const ExtrusionPath &path : paths) {
            const Points points = this->cut(path.polyline.points);
            if (points.size() < 2)
                continue;
            // In the zone or not, segment by segment (the crossing points end the segments).
            std::vector<char> kept(points.size() - 1);
            for (size_t i = 0; i + 1 < points.size(); ++ i)
                kept[i] = this->inside(Point((points[i].x() + points[i + 1].x()) / 2, (points[i].y() + points[i + 1].y()) / 2));
            if (! first_known) {
                first_known  = true;
                first_inside = kept.front() != 0;
            }
            last_inside = kept.back() != 0;
            for (size_t i = 0; i < kept.size();) {
                size_t j = i + 1;
                while (j < kept.size() && kept[j] == kept[i])
                    ++ j;
                // Segments [i, j), points [i, j], on the same side of the boundary.
                if (kept[i]) {
                    if (! in_run) {
                        runs.emplace_back();
                        in_run = true;
                    }
                    runs.back().emplace_back(Polyline(Points(points.begin() + i, points.begin() + j + 1)), path);
                } else {
                    all_inside = false;
                    in_run     = false;
                }
                i = j;
            }
        }
        if (all_inside)
            return true;
        if (closed && first_inside && last_inside && runs.size() > 1) {
            // The run through the start of the loop: its end goes first.
            ExtrusionPaths &first = runs.front();
            first.insert(first.begin(), std::make_move_iterator(runs.back().begin()), std::make_move_iterator(runs.back().end()));
            runs.pop_back();
        }
        for (ExtrusionPaths &run : runs) {
            double length = 0.;
            double width  = 0.;
            for (const ExtrusionPath &path : run) {
                length += path.length();
                width   = std::max(width, double(path.width));
            }
            if (! run.empty() && length >= scale_(width))
                out.push_back(new ExtrusionMultiPath(std::move(run)));
        }
        return false;
    }

    BoundingBox                         m_bbox;
    AABBTreeLines::LinesDistancer<Line> m_boundary;
};
} // namespace

void infill_walls_make_zone_walls(LayerRegion &layerm, const ExPolygons &area, std::vector<LoopNode> &loop_nodes)
{
    const Layer             &layer         = *layerm.layer();
    const PrintObject       &object        = *layer.object();
    const PrintRegionConfig &region_config = layerm.region().config();
    const PrintObjectConfig &object_config = object.config();
    if (layerm.fill_no_overlap_expolygons.empty() || object.print()->config().spiral_mode.value ||
        ! infill_walls_is_painted(layer, iwtWallLoops) || ! infill_walls_region_follows_object(layer, region_config, iwtWallLoops))
        return;
    // A single wall everywhere on the first layer or on the topmost one, as LayerRegion::make_extra_perimeters().
    if ((object_config.only_one_wall_first_layer && layer.id() == 0) ||
        (object_config.top_one_wall_type != TopOneWallType::None && layer.upper_layer == nullptr))
        return;
    const std::optional<InfillWallsAxisValues> values = infill_walls_axis_values(layer, iwtWallLoops);
    if (! values)
        return;

    // The region has the fewest walls painted on the layer (infill_walls_wall_loops()), plus the alternate extra wall.
    BoundingBox bbox = infill_walls_layer_bbox(layer);
    const int   painted_min  = std::clamp(int(std::lround(values->min_value(bbox))), int(MIN_VARIABLE_WALL_LOOPS), int(MAX_VARIABLE_WALL_LOOPS));
    const int   region_walls = layer_wall_loops(layer, region_config);
    bbox.offset(scale_(1.));
    const InfillWallsZones zones = values->zones(bbox);
    if (zones.size() < 2)
        return;

    // area may be the fill_no_overlap_expolygons of the region, changed at the end: not read after the loop below.
    const ExPolygons     region    = to_expolygons(layerm.slices.surfaces);
    const BoundingBox    fill_bbox = get_extents(layerm.fill_no_overlap_expolygons);
    ExtrusionEntitiesPtr zone_perimeters;
    ExtrusionEntitiesPtr zone_thin_fills;
    ExPolygons           zone_fill;
    ExPolygons           zone_fill_no_overlap;
    Polygons             zones_with_walls;
    for (const InfillWallsZone &zone : zones) {
        const int extra_wall_loops = std::clamp(int(std::lround(zone.value)), int(MIN_VARIABLE_WALL_LOOPS), int(MAX_VARIABLE_WALL_LOOPS)) - painted_min;
        if (extra_wall_loops <= 0 || ! get_extents(zone.area).overlap(fill_bbox))
            continue;
        // The part of the zone in this region, and the islands of the area around it.
        const ExPolygons zone_region = intersection_ex(zone.area, region);
        if (zone_region.empty())
            continue;
        const BoundingBox zone_bbox = get_extents(zone_region);
        ExPolygons        islands;
        for (const ExPolygon &island : area)
            if (get_extents(island).overlap(zone_bbox))
                islands.emplace_back(island);
        if (islands.empty())
            continue;
        // The walls over the whole area left to the infill, then only their parts in the zone and in the region: they
        // follow the contour of the object and stop at the boundary of the zone, rather than closing along it across the
        // part or following the boundary of a modifier, whose region only cuts them.
        InfillWallsZoneWalls         walls = infill_walls_generate_walls(layerm, extra_wall_loops, region_walls, islands, loop_nodes);
        const InfillWallsZoneClipper clipper(zone_region);
        clipper.clip(walls.perimeters);
        clipper.clip(walls.thin_fills);
        append(zone_perimeters, std::move(walls.perimeters.entities));
        walls.perimeters.entities.clear();
        append(zone_thin_fills, std::move(walls.thin_fills.entities));
        walls.thin_fills.entities.clear();
        // The infill of the region, where these walls leave room for it.
        append(zone_fill, intersection_ex(walls.fill_surfaces.surfaces, intersection_ex(layerm.fill_expolygons, zone_region)));
        append(zone_fill_no_overlap, intersection_ex(walls.fill_no_overlap, intersection_ex(layerm.fill_no_overlap_expolygons, zone_region)));
        polygons_append(zones_with_walls, to_polygons(zone_region));
    }
    if (zones_with_walls.empty())
        return;

    // Inside the walls of the region: printed before them (inner walls first), unless the outer wall is printed first.
    layerm.perimeters.entities.insert(region_config.wall_sequence.value == WallSequence::OuterInner ? layerm.perimeters.entities.end() :
                                                                                                     layerm.perimeters.entities.begin(),
                                      zone_perimeters.begin(), zone_perimeters.end());
    layerm.thin_fills.append(std::move(zone_thin_fills));
    // The infill keeps its area outside the zones with more walls, and gets the area left inside their walls.
    const Surface surface_template = layerm.fill_surfaces.surfaces.empty() ? Surface(stInternal) : layerm.fill_surfaces.surfaces.front();
    ExPolygons    fill             = diff_ex(layerm.fill_expolygons, zones_with_walls);
    append(fill, std::move(zone_fill));
    fill = union_ex(fill);
    ExPolygons fill_no_overlap = diff_ex(layerm.fill_no_overlap_expolygons, zones_with_walls);
    append(fill_no_overlap, std::move(zone_fill_no_overlap));
    layerm.fill_no_overlap_expolygons = union_ex(fill_no_overlap);
    layerm.fill_expolygons            = fill;
    layerm.fill_surfaces.set(std::move(fill), surface_template);
}

bool infill_walls_has_zone_walls(const Layer &layer)
{
    return infill_walls_painted_curve(layer, iwtWallLoops, paX) != nullptr || infill_walls_painted_curve(layer, iwtWallLoops, paY) != nullptr;
}

bool infill_walls_has_void_density(const PrintObject &object)
{
    const ModelObject *model_object = object.model_object();
    if (model_object == nullptr)
        return false;
    // Only the curve painted along Z goes down to 0 %, see MIN_VARIABLE_INFILL_DENSITY_XY.
    const auto it = model_object->infill_walls_profile.curves.find((uint32_t)iwtInfillDensity);
    if (it == model_object->infill_walls_profile.curves.end())
        return false;
    const std::vector<coordf_t> &curve = it->second;
    for (size_t i = 1; i < curve.size(); i += 2)
        if (infill_walls_sanitize_density(curve[i]) == 0.)
            return true;
    return false;
}

bool infill_walls_profile_changes_perimeters(const InfillWallsProfile &old_profile, const InfillWallsProfile &new_profile)
{
    auto curve_of = [](const InfillWallsProfile &profile, uint32_t key) -> const std::vector<coordf_t>* {
        const auto it = profile.curves.find(key);
        return (it == profile.curves.end()) ? nullptr : &it->second;
    };
    auto same_curve = [](const std::vector<coordf_t> *a, const std::vector<coordf_t> *b) {
        return (a == nullptr || b == nullptr) ? a == b : *a == *b;
    };
    // The perimeter generator only looks at the infill density to know whether there is any infill at all
    // (0 %), and a solid infill (100 %) changes the classification of the surfaces.
    auto reaches_extremes = [](const std::vector<coordf_t> *curve) {
        if (curve != nullptr)
            for (size_t i = 1; i < curve->size(); i += 2) {
                const coordf_t density = infill_walls_sanitize_density((*curve)[i]);
                if (density == 0. || density >= MAX_VARIABLE_INFILL_DENSITY - EPSILON)
                    return true;
            }
        return false;
    };

    for (PaintAxis axis : { paZ, paX, paY }) {
        const uint32_t walls = paint_axis_key(iwtWallLoops, axis);
        if (! same_curve(curve_of(old_profile, walls), curve_of(new_profile, walls)))
            return true;
    }
    // A density painted along X / Y sets the density of the layers it covers within 1..99 %: painting the first one or
    // removing the last one may turn layers void at 0 % (along Z or in the print profile) into infilled ones and back.
    auto has_xy_density = [](const InfillWallsProfile &profile) { return profile.has(iwtInfillDensity, paX) || profile.has(iwtInfillDensity, paY); };
    if (has_xy_density(old_profile) != has_xy_density(new_profile))
        return true;
    const std::vector<coordf_t> *old_density = curve_of(old_profile, iwtInfillDensity);
    const std::vector<coordf_t> *new_density = curve_of(new_profile, iwtInfillDensity);
    return ! same_curve(old_density, new_density) && (reaches_extremes(old_density) || reaches_extremes(new_density));
}

} // namespace Slic3r
