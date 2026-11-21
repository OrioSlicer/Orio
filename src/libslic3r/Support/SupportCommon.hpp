///|/ Copyright (c) Prusa Research 2023 Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_SupportCommon_hpp_
#define slic3r_SupportCommon_hpp_

#include "../Layer.hpp"
#include "../Polygon.hpp"
#include "../Print.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"

#include <functional>
#include <memory>

namespace Slic3r {

class PrintObject;
class SupportLayer;

// Orio: support printed inside a modifier or a height range which changes how the support is built
// (normal supports only): number of interface layers, interface / base spacing, XY and Z distances to the object,
// walls around the support, expansion of its first layer.
// Outside of any zone, the settings of the object apply (SupportParameters).
struct SupportZone
{
    // Gaps between the support and the object above (top contact) / below (bottom contact), rounded as in SlicingParameters.
    // Those of the object if its interface is soluble (support layers synchronized with the object).
    coordf_t        gap_support_object               { 0. };
    coordf_t        gap_object_support               { 0. };
    // Walls around the base of the support (a sheath).
    bool            with_sheath                      { false };
    // Expansion of the first support layer on the print bed (mm, negative: auto).
    coordf_t        first_layer_expansion            { 0. };
    size_t          num_top_interface_layers         { 0 };
    size_t          num_bottom_interface_layers      { 0 };
    size_t          num_top_base_interface_layers    { 0 };
    size_t          num_bottom_base_interface_layers { 0 };
    // XY distance between the support and the object (support_object_xy_distance, maybe overridden by the top Z distance).
    coordf_t        gap_xy                           { 0. };
    // Flow of the interfaces, the base flow if the zone has no interface layer.
    Flow            interface_flow;
    coordf_t        interface_density                { 1. };
    coordf_t        support_density                  { 0. };
    InfillPattern   base_fill_pattern                { ipSupportBase };
    InfillPattern   contact_fill_pattern             { ipRectilinear };

    // Height range zone: the whole layers between z_min and z_max (object coordinates, as the height range).
    bool            is_height_range                  { false };
    coordf_t        z_min                            { 0. };
    coordf_t        z_max                            { 0. };
    // Modifier zone: slices of the modifier at each object layer.
    std::vector<Polygons> slices;
    // Organic tree support zones (see SupportZones(object, keys)): the support settings of the object overridden by the zone.
    std::shared_ptr<const PrintRegionConfig> config;

    size_t num_top_interface_layers_only()    const { return std::max(0, int(num_top_interface_layers) - int(num_top_base_interface_layers)); }
    size_t num_bottom_interface_layers_only() const { return std::max(0, int(num_bottom_interface_layers) - int(num_bottom_base_interface_layers)); }
};

class SupportZones
{
public:
    SupportZones() = default;
    // Collect the zones of the normal supports of an object. Empty if no modifier / height range changes how the support is built.
    SupportZones(const PrintObject &object, const SupportParameters &support_params);
    // Zones of the organic tree supports: the modifiers / height ranges which set other values of these keys than the object,
    // with their configuration (SupportZone::config). Only their areas are used (split()), see PrintObject::support_passes().
    SupportZones(const PrintObject &object, const t_config_option_keys &keys);

    bool                empty() const { return m_zones.empty(); }
    size_t              size()  const { return m_zones.size(); }
    const SupportZone&  zone(size_t idx) const { return m_zones[idx]; }

    // Split the polygons of a support layer printed at z (print_z coordinates, middle of the layer) by zones.
    // out[0]: outside of any zone, out[i + 1]: inside zone i. Where zones overlap, the last one wins.
    std::vector<Polygons> split(const Polygons &polygons, coordf_t z) const;
    // Is zone idx present at z?
    bool                covers(size_t idx, coordf_t z) const { bool whole = false; return this->area(idx, z, whole) != nullptr || whole; }
    // Middle of a support layer, to be passed to split().
    static coordf_t     layer_z(const SupportGeneratorLayer &layer) { return layer.height > 0. ? layer.print_z - 0.5 * layer.height : layer.print_z; }

    // Gaps of the part split_idx of split(): 0 outside of any zone (the object), i + 1 inside zone i.
    coordf_t            gap_support_object(size_t split_idx) const { return split_idx == 0 ? m_gap_support_object : m_zones[split_idx - 1].gap_support_object; }
    coordf_t            gap_object_support(size_t split_idx) const { return split_idx == 0 ? m_gap_object_support : m_zones[split_idx - 1].gap_object_support; }
    // Some zone has other top / bottom gaps than the object, resp. another expansion of the first layer.
    bool                has_other_z_gaps()                const { return m_other_z_gaps; }
    bool                has_other_first_layer_expansion() const { return m_other_first_layer_expansion; }

private:
    // Adds the zones of the height ranges, then of the modifiers (in this order: the last one wins), for which make_zone()
    // sets the zone and returns true, with their areas.
    void                add_zones(const PrintObject &object, const std::function<bool(const DynamicPrintConfig&, SupportZone&)> &make_zone);
    // Area of a zone at z. Returns nullptr if the zone is absent at this height, sets whole if it covers the whole layer.
    const Polygons*     area(size_t idx, coordf_t z, bool &whole) const;

    std::vector<SupportZone>    m_zones;
    // Top of the object layers (print_z) to find the slices of the modifiers at a given height.
    std::vector<coordf_t>       m_layers_print_z;
    // Print_z of the bottom of the object (raft), to convert print_z into the coordinates of the height ranges.
    coordf_t                    m_object_print_z_min { 0. };
    // Gaps of the object (SlicingParameters).
    coordf_t                    m_gap_support_object { 0. };
    coordf_t                    m_gap_object_support { 0. };
    bool                        m_other_z_gaps                { false };
    bool                        m_other_first_layer_expansion { false };
};

// Turn some of the base layers into base interface layers.
// For soluble interfaces with non-soluble bases, print maximum two first interface layers with the base
// extruder to improve adhesion of the soluble filament to the base.
// For Organic supports, merge top_interface_layers & top_base_interface_layers with the interfaces
// produced by this function.
std::pair<SupportGeneratorLayersPtr, SupportGeneratorLayersPtr> generate_interface_layers(
    const PrintObjectConfig           &config,
    const SupportParameters           &support_params,
    const SupportGeneratorLayersPtr   &bottom_contacts,
    const SupportGeneratorLayersPtr   &top_contacts,
    // Input / output, will be merged with output
    SupportGeneratorLayersPtr         &top_interface_layers,
    SupportGeneratorLayersPtr         &top_base_interface_layers,
    // Input, will be trimmed with the newly created interface layers.
    SupportGeneratorLayersPtr         &intermediate_layers,
    SupportGeneratorLayerStorage      &layer_storage,
    // Orio: zones with other numbers of interface layers (normal supports only).
    const SupportZones                *zones = nullptr);

// Generate raft layers, also expand the 1st support layer
// in case there is no raft layer to improve support adhesion.
SupportGeneratorLayersPtr generate_raft_base(
	const PrintObject				&object,
	const SupportParameters			&support_params,
	const SlicingParameters			&slicing_params,
	const SupportGeneratorLayersPtr &top_contacts,
	const SupportGeneratorLayersPtr &interface_layers,
	const SupportGeneratorLayersPtr &base_interface_layers,
	const SupportGeneratorLayersPtr &base_layers,
	SupportGeneratorLayerStorage    &layer_storage,
    // Orio: zones with another expansion of the first support layer (normal supports only, without raft).
    const SupportZones              *zones = nullptr);

void tree_supports_generate_paths(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Flow &flow, const SupportParameters &support_params);

void fill_expolygons_with_sheath_generate_paths(
    ExtrusionEntitiesPtr &dst, const Polygons &polygons, Fill *filler, float density, ExtrusionRole role, const Flow &flow, const SupportParameters& support_params, bool with_sheath, bool no_sort);

// returns sorted layers
SupportGeneratorLayersPtr generate_support_layers(
	PrintObject							&object,
    const SupportGeneratorLayersPtr     &raft_layers,
    const SupportGeneratorLayersPtr     &bottom_contacts,
    const SupportGeneratorLayersPtr     &top_contacts,
    const SupportGeneratorLayersPtr     &intermediate_layers,
    const SupportGeneratorLayersPtr     &interface_layers,
    const SupportGeneratorLayersPtr     &base_interface_layers);

// Produce the support G-code.
// Used by both classic and tree supports.
void generate_support_toolpaths(
	SupportLayerPtrs    				&support_layers,
	const PrintObjectConfig 			&config,
	const SupportParameters 			&support_params,
	const SlicingParameters 			&slicing_params,
    const SupportGeneratorLayersPtr 	&raft_layers,
    const SupportGeneratorLayersPtr   	&bottom_contacts,
    const SupportGeneratorLayersPtr   	&top_contacts,
    const SupportGeneratorLayersPtr   	&intermediate_layers,
	const SupportGeneratorLayersPtr   	&interface_layers,
    const SupportGeneratorLayersPtr   	&base_interface_layers,
    const std::vector<ExPolygons>       &cooldown_areas = {},
    // Orio: zones with other interface / base densities (normal supports only).
    const SupportZones                  *zones = nullptr);

// FN_HIGHER_EQUAL: the provided object pointer has a Z value >= of an internal threshold.
// Find the first item with Z value >= of an internal threshold of fn_higher_equal.
// If no vec item with Z value >= of an internal threshold of fn_higher_equal is found, return vec.size()
// If the initial idx is size_t(-1), then use binary search.
// Otherwise search linearly upwards.
template<typename IteratorType, typename IndexType, typename FN_HIGHER_EQUAL>
IndexType idx_higher_or_equal(IteratorType begin, IteratorType end, IndexType idx, FN_HIGHER_EQUAL fn_higher_equal)
{
    auto size = int(end - begin);
    if (size == 0) {
        idx = 0;
    } else if (idx == IndexType(-1)) {
        // First of the batch of layers per thread pool invocation. Use binary search.
        int idx_low  = 0;
        int idx_high = std::max(0, size - 1);
        while (idx_low + 1 < idx_high) {
            int idx_mid  = (idx_low + idx_high) / 2;
            if (fn_higher_equal(begin[idx_mid]))
                idx_high = idx_mid;
            else
                idx_low  = idx_mid;
        }
        idx =  fn_higher_equal(begin[idx_low])  ? idx_low  :
              (fn_higher_equal(begin[idx_high]) ? idx_high : size);
    } else {
        // For the other layers of this batch of layers, search incrementally, which is cheaper than the binary search.
        while (int(idx) < size && ! fn_higher_equal(begin[idx]))
            ++ idx;
    }
    return idx;
}
template<typename T, typename IndexType, typename FN_HIGHER_EQUAL>
IndexType idx_higher_or_equal(const std::vector<T>& vec, IndexType idx, FN_HIGHER_EQUAL fn_higher_equal)
{
    return idx_higher_or_equal(vec.begin(), vec.end(), idx, fn_higher_equal);
}

// FN_LOWER_EQUAL: the provided object pointer has a Z value <= of an internal threshold.
// Find the first item with Z value <= of an internal threshold of fn_lower_equal.
// If no vec item with Z value <= of an internal threshold of fn_lower_equal is found, return -1.
// If the initial idx is < -1, then use binary search.
// Otherwise search linearly downwards.
template<typename IT, typename FN_LOWER_EQUAL>
int idx_lower_or_equal(IT begin, IT end, int idx, FN_LOWER_EQUAL fn_lower_equal)
{
    auto size = int(end - begin);
    if (size == 0) {
        idx = -1;
    } else if (idx < -1) {
        // First of the batch of layers per thread pool invocation. Use binary search.
        int idx_low  = 0;
        int idx_high = std::max(0, size - 1);
        while (idx_low + 1 < idx_high) {
            int idx_mid  = (idx_low + idx_high) / 2;
            if (fn_lower_equal(begin[idx_mid]))
                idx_low  = idx_mid;
            else
                idx_high = idx_mid;
        }
        idx =  fn_lower_equal(begin[idx_high]) ? idx_high :
              (fn_lower_equal(begin[idx_low ]) ? idx_low  : -1);
    } else {
        // For the other layers of this batch of layers, search incrementally, which is cheaper than the binary search.
        while (idx >= 0 && ! fn_lower_equal(begin[idx]))
            -- idx;
    }
    return idx;
}
template<typename T, typename FN_LOWER_EQUAL>
int idx_lower_or_equal(const std::vector<T*> &vec, int idx, FN_LOWER_EQUAL fn_lower_equal)
{
    return idx_lower_or_equal(vec.begin(), vec.end(), idx, fn_lower_equal);
}

[[nodiscard]] Polygons safe_union(const Polygons first, const Polygons second = {});

[[nodiscard]] ExPolygons safe_union(const ExPolygons first, const ExPolygons second = {});

[[nodiscard]] Polygons safe_offset_inc(
    const Polygons &me, coord_t distance, const Polygons &collision, coord_t safe_step_size, coord_t last_step_offset_without_check, size_t min_amount_offset);


/*!
 * \brief Offsets (increases the area of) a polygons object in multiple steps to ensure that it does not lag through over a given obstacle.
 * \param me[in] Polygons object that has to be offset.
 * \param distance[in] The distance by which me should be offset. Expects values >=0.
 * \param CollisionPolyType collision[in] The area representing obstacles. CollisionPolyType may be ExPolygons or Polygons.
 * \param last_step_offset_without_check[in] The most it is allowed to offset in one step.
 * \param min_amount_offset[in] How many steps have to be done at least. As this uses round offset this increases the amount of vertices, which may be required if Polygons get
 * very small. Required as arcTolerance is not exposed in offset, which should result with a similar result. \return The resulting Polygons object.
 */
template<typename CollisionPolyType>
[[nodiscard]] ExPolygons safe_offset_inc(
    const ExPolygons &me, coord_t distance, const CollisionPolyType &collision, coord_t safe_step_size, coord_t last_step_offset_without_check, size_t min_amount_offset)
{
    bool     do_final_difference = last_step_offset_without_check == 0;
    ExPolygons ret               = safe_union(me); // ensure sane input

    // Trim the collision polygons with the region of interest for diff() efficiency.
    Polygons collision_trimmed_buffer;
    auto     collision_trimmed = [&collision_trimmed_buffer, &collision, &ret, distance]() -> const Polygons     &{
        if (collision_trimmed_buffer.empty() && !collision.empty())
            collision_trimmed_buffer = ClipperUtils::clip_clipper_polygons_with_subject_bbox(collision, get_extents(ret).inflated(std::max(0, distance) + SCALED_EPSILON));
        return collision_trimmed_buffer;
    };

    if (distance == 0) return do_final_difference ? diff_ex(ret, collision_trimmed()) : union_ex(ret);
    if (safe_step_size < 0 || last_step_offset_without_check < 0) {
        BOOST_LOG_TRIVIAL(error) << "Offset increase got invalid parameter!";
        return do_final_difference ? diff_ex(ret, collision_trimmed()) : union_ex(ret);
    }

    coord_t step_size = safe_step_size;
    int     steps     = distance > last_step_offset_without_check ? (distance - last_step_offset_without_check) / step_size : 0;
    if (distance - steps * step_size > last_step_offset_without_check) {
        if ((steps + 1) * step_size <= distance)
            // This will be the case when last_step_offset_without_check >= safe_step_size
            ++steps;
        else
            do_final_difference = true;
    }
    if (steps + (distance < last_step_offset_without_check || (distance % step_size) != 0) < int(min_amount_offset) && min_amount_offset > 1) {
        // yes one can add a bool as the standard specifies that a result from compare operators has to be 0 or 1
        // reduce the stepsize to ensure it is offset the required amount of times
        step_size = distance / min_amount_offset;
        if (step_size >= safe_step_size) {
            // effectivly reduce last_step_offset_without_check
            step_size = safe_step_size;
            steps     = min_amount_offset;
        } else
            steps = distance / step_size;
    }
    // offset in steps
    for (int i = 0; i < steps; ++i) {
        ret = diff_ex(offset_ex(ret, step_size, ClipperLib::jtRound, scaled<float>(0.01)), collision_trimmed());
        // ensure that if many offsets are done the performance does not suffer extremely by the new vertices of jtRound.
        if (i % 10 == 7) ret = expolygons_simplify(ret, scaled<double>(0.015));
    }
    // offset the remainder
    float last_offset = distance - steps * step_size;
    if (last_offset > SCALED_EPSILON) ret = offset_ex(ret, distance - steps * step_size, ClipperLib::jtRound, scaled<float>(0.01));
    ret = expolygons_simplify(ret, scaled<double>(0.015));

    if (do_final_difference) ret = diff_ex(ret, collision_trimmed());
    return union_ex(ret);
}


} // namespace Slic3r

#endif /* slic3r_SupportCommon_hpp_ */
