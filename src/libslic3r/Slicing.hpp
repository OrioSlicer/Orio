// Based on implementation by @platsch

#ifndef slic3r_Slicing_hpp_
#define slic3r_Slicing_hpp_

#include <cstring>
#include <map>
#include <set>
#include <type_traits>
#include <vector>

#include "libslic3r.h"
#include "Utils.hpp"

namespace Slic3r
{

class PrintConfig;
class PrintObjectConfig;
class PrintRegionConfig;
class ModelConfig;
class ModelObject;
class DynamicPrintConfig;

// Parameters to guide object slicing and support generation.
// The slicing parameters account for a raft and whether the 1st object layer is printed with a normal or a bridging flow
// (using a normal flow over a soluble support, using a bridging flow over a non-soluble support).
struct SlicingParameters
{
	SlicingParameters() = default;

    // Orio: support_config holds the support settings of the object which may also be set per region
    // (support_top_z_distance, support_bottom_z_distance), see PrintObject::support_region_config().
    static SlicingParameters create_from_config(
        const PrintConfig       &print_config,
        const PrintObjectConfig &object_config,
        const PrintRegionConfig &support_config,
        coordf_t                 object_height,
        const std::vector<unsigned int> &object_extruders);

    // Has any raft layers?
    bool        has_raft() const { return raft_layers() > 0; }
    size_t      raft_layers() const { return base_raft_layers + interface_raft_layers; }

    // Is the 1st object layer height fixed, or could it be varied?
    bool        first_object_layer_height_fixed()  const { return ! has_raft() || first_object_layer_bridging; }

    // Height of the object to be printed. This value does not contain the raft height.
    coordf_t    object_print_z_height() const { return object_print_z_max - object_print_z_min; }

    bool        valid { false };

    // Number of raft layers.
    size_t      base_raft_layers { 0 };
    // Number of interface layers including the contact layer.
    size_t      interface_raft_layers { 0 };

    // Layer heights of the raft (base, interface and a contact layer).
    coordf_t    base_raft_layer_height { 0 };
    coordf_t    interface_raft_layer_height { 0 };
    coordf_t    contact_raft_layer_height { 0 };

	// The regular layer height, applied for all but the first layer, if not overridden by layer ranges
	// or by the variable layer thickness table.
    coordf_t    layer_height { 0 };
    // Minimum / maximum layer height, to be used for the automatic adaptive layer height algorithm,
    // or by an interactive layer height editor.
    coordf_t    min_layer_height { 0 };
    coordf_t    max_layer_height { 0 };
    coordf_t    max_suport_layer_height { 0 };

    // First layer height of the print, this may be used for the first layer of the raft
    // or for the first layer of the print.
    coordf_t    first_print_layer_height { 0 };

    // Thickness of the first layer. This is either the first print layer thickness if printed without a raft,
    // or a bridging flow thickness if printed over a non-soluble raft,
    // or a normal layer height if printed over a soluble raft.
    coordf_t    first_object_layer_height { 0 };

    // If the object is printed over a non-soluble raft, the first layer may be printed with a briding flow.
    bool 		first_object_layer_bridging { false };

    // Soluble interface? (PLA soluble in water, HIPS soluble in lemonen)
    // otherwise the interface must be broken off.
    bool        soluble_interface { false };
    // Gap when placing object over raft.
    coordf_t    gap_raft_object { 0 };
    // Gap when placing support over object.
    coordf_t    gap_object_support { 0 };
    // Gap when placing object over support.
    coordf_t    gap_support_object { 0 };

    // Bottom and top of the printed object.
    // If printed without a raft, object_print_z_min = 0 and object_print_z_max = object height.
    // Otherwise object_print_z_min is equal to the raft height.
    coordf_t    raft_base_top_z { 0 };
    coordf_t    raft_interface_top_z { 0 };
    coordf_t    raft_contact_top_z { 0 };
    // In case of a soluble interface, object_print_z_min == raft_contact_top_z, otherwise there is a gap between the raft and the 1st object layer.
    coordf_t 	object_print_z_min { 0 };
    coordf_t 	object_print_z_max { 0 };
};
static_assert(IsTriviallyCopyable<SlicingParameters>::value, "SlicingParameters class is not POD (and it should be - see constructor).");

// The two slicing parameters lead to the same layering as long as the variable layer thickness is not in action.
inline bool equal_layering(const SlicingParameters &sp1, const SlicingParameters &sp2)
{
    assert(sp1.valid);
    assert(sp2.valid);
    return  sp1.base_raft_layers                    == sp2.base_raft_layers                     &&
            sp1.interface_raft_layers               == sp2.interface_raft_layers                &&
            sp1.base_raft_layer_height              == sp2.base_raft_layer_height               &&
            sp1.interface_raft_layer_height         == sp2.interface_raft_layer_height          &&
            sp1.contact_raft_layer_height           == sp2.contact_raft_layer_height            &&
            sp1.layer_height                        == sp2.layer_height                         &&
            sp1.min_layer_height                    == sp2.min_layer_height                     &&
            sp1.max_layer_height                    == sp2.max_layer_height                     &&
//            sp1.max_suport_layer_height             == sp2.max_suport_layer_height              &&
            sp1.first_print_layer_height            == sp2.first_print_layer_height             &&
            sp1.first_object_layer_height           == sp2.first_object_layer_height            &&
            sp1.first_object_layer_bridging         == sp2.first_object_layer_bridging          &&
            // BBS: following  are not required for equal layer height.
            // Since the z-gap diff may be multiple of layer height.
#if 0
            sp1.soluble_interface                   == sp2.soluble_interface                    &&
            sp1.gap_raft_object                     == sp2.gap_raft_object                      &&
            sp1.gap_object_support                  == sp2.gap_object_support                   &&
            sp1.gap_support_object                  == sp2.gap_support_object                   &&
#endif
            sp1.raft_base_top_z                     == sp2.raft_base_top_z                      &&
            sp1.raft_interface_top_z                == sp2.raft_interface_top_z                 &&
            sp1.raft_contact_top_z                  == sp2.raft_contact_top_z                   &&
            sp1.object_print_z_min                  == sp2.object_print_z_min;
}

typedef std::pair<coordf_t,coordf_t> t_layer_height_range;
typedef std::map<t_layer_height_range, ModelConfig> t_layer_config_ranges;

extern std::vector<coordf_t> layer_height_profile_from_ranges(
    const SlicingParameters     &slicing_params,
    const t_layer_config_ranges &layer_config_ranges,
    // Orio : plus les intervalles horizontaux de l'outil « Modificateurs » (bornes, hauteur de couche).
    const std::vector<std::pair<t_layer_height_range, coordf_t>> &interval_ranges = {});

extern std::vector<double> layer_height_profile_adaptive(
    const SlicingParameters& slicing_params,
    const ModelObject& object, float quality_factor);

struct HeightProfileSmoothingParams
{
    unsigned int radius;
    bool keep_min;

    HeightProfileSmoothingParams() : radius(5), keep_min(false) {}
    HeightProfileSmoothingParams(unsigned int radius, bool keep_min) : radius(radius), keep_min(keep_min) {}
};

extern std::vector<double> smooth_height_profile(
    const std::vector<double>& profile, const SlicingParameters& slicing_params,
    const HeightProfileSmoothingParams& smoothing_params);

enum LayerHeightEditActionType : unsigned int {
    LAYER_HEIGHT_EDIT_ACTION_INCREASE = 0,
    LAYER_HEIGHT_EDIT_ACTION_DECREASE = 1,
    LAYER_HEIGHT_EDIT_ACTION_REDUCE   = 2,
    LAYER_HEIGHT_EDIT_ACTION_SMOOTH   = 3
};

extern void adjust_layer_height_profile(
    const SlicingParameters     &slicing_params,
    std::vector<coordf_t>       &layer_height_profile,
    coordf_t                     z,
    coordf_t                     layer_thickness_delta, 
    coordf_t                     band_width,
    LayerHeightEditActionType    action);

// Orio: the bounds of a layer height profile (ModelObject::layer_height_bounds), fixed heights in mm the painting
// does not cross. They only guard the editing: the slicing ignores them. Width of the step of the profile just above a
// bound (a layer takes the height read at its middle, generate_object_layers()).
static constexpr coordf_t LAYER_HEIGHT_BOUND_STEP = 0.01;
// What a stroke changed, kept only between the bounds around it: `painted` (the profile after the stroke) on ]lo, hi],
// `before` (the profile before it) elsewhere, their points copied as they are (a profile may hold steps: two points at
// the same z), with a step just above lo and just above hi. lo = 0 and hi = the top of the profile for no bound on that
// side. Both profiles span the same [0, top].
extern std::vector<coordf_t> layer_height_clip_stroke(
    const std::vector<coordf_t> &before,
    const std::vector<coordf_t> &painted,
    coordf_t                     lo,
    coordf_t                     hi);
// smooth_height_profile() on each part of the profile between its bounds (sorted), on its own: the steps at the bounds
// stay.
extern std::vector<coordf_t> smooth_height_profile_bounded(
    const std::vector<coordf_t>        &profile,
    const std::vector<coordf_t>        &bounds,
    const SlicingParameters            &slicing_params,
    const HeightProfileSmoothingParams &smoothing_params);

// Produce object layers as pairs of low / high layer boundaries, stored into a linear vector.
// The object layers are based at z=0, ignoring the raft layers.
extern std::vector<coordf_t> generate_object_layers(
    const SlicingParameters     &slicing_params,
    const std::vector<coordf_t> &layer_height_profile,
    bool is_precise_z_height);

// Check whether the layer height profile describes a fixed layer height profile.
bool check_object_layers_fixed(
    const SlicingParameters     &slicing_params,
    const std::vector<coordf_t> &layer_height_profile);
// Produce a 1D texture packed into a 2D texture describing in the RGBA format
// the planned object layers.
// Returns number of cells used by the texture of the 0th LOD level.
extern int generate_layer_height_texture(
    const SlicingParameters     &slicing_params,
    const std::vector<coordf_t> &layers,
    void *data, int rows, int cols, bool level_of_detail_2nd_level);

// BBS: variable print speed along Z.
// The speed profile works exactly like the variable layer height profile: the user paints a
// value along the Z axis of a single object and the G-code generator interpolates it linearly.
// Contrary to the layer height, the profile does not change the layering at all, it only
// overrides the feedrate of the selected extrusion roles, therefore it only invalidates the
// G-code export step.

// ExtrusionRole is declared in ExtrusionEntity.hpp, forward declared here to keep the include
// graph of Slicing.hpp (which is pulled in by Model.hpp) unchanged.
enum ExtrusionRole : uint8_t;
// Orio: same for PaintAxis (PaintAxis.hpp), the axis of the object a curve is painted along.
enum PaintAxis : uint32_t;

// Which families of extrusion roles a variable speed profile is applied to.
enum VariableSpeedTarget : uint32_t {
    vstOuterWall    = 1 << 0,
    vstInnerWall    = 1 << 1,
    vstSparseInfill = 1 << 2,
    vstSolidInfill  = 1 << 3,
    vstTopSurface   = 1 << 4,
    // Orio: the bridges only, since the 14/10. The overhang walls keep the speeds of the print settings (01/11).
    vstBridge       = 1 << 5,
    vstSupport      = 1 << 6,
    vstGapFill      = 1 << 7,
    vstIroning      = 1 << 8,
    vstCount        = 9,
    vstAll          = (1 << vstCount) - 1,
    // Anything that lays down a regular perimeter or infill. Bridges, supports and ironing are
    // excluded by default because they have very good reasons to run at their own speed.
    vstDefault      = vstOuterWall | vstInnerWall | vstSparseInfill | vstSolidInfill | vstTopSurface | vstGapFill,
};

// Lowest / highest speed the interactive editor and the G-code generator will ever accept, mm/s.
static constexpr coordf_t MIN_VARIABLE_SPEED = 1.;
static constexpr coordf_t MAX_VARIABLE_SPEED = 1000.;

// Per-extrusion-type print speed painted along Z, linearly interpolated when generating the G-code.
// Orio: one independent <z, speed> curve per VariableSpeedTarget bit, so at a given layer the
// infill and the perimeters can run at different painted speeds. Each curve packs <z, speed> pairs into
// a 1D array, z being relative to the bottom of the object (raft excluded), like the layer height profile.
// A role whose bit has no curve keeps its normal (profile) speed. An empty map = no override at all.
// The same roles can be painted along the X / Y axes of the object as well (PaintAxis.hpp): those curves pack
// <coordinate, speed> pairs, the coordinate being in the frame of the object. Where several axes are painted for a
// role, the slowest painted speed applies at each point (decision of the user, 05/10).
struct VariableSpeedProfile
{
    // Key = paint_axis_key(VariableSpeedTarget bit, axis): the bare bit for a curve painted along Z (the only kind
    // the projects held before the X / Y painting). Value = that role's <z, speed> (or <coordinate, speed>) curve.
    std::map<uint32_t, std::vector<coordf_t>> curves;
    // Orio: bounds the painting does not cross (11/10), per curve (same key), sorted, in the coordinates of the
    // curve. They only guide the editor: the G-code ignores them.
    std::map<uint32_t, std::vector<coordf_t>> bounds;

    bool empty() const throw() { return curves.empty(); }
    void clear() { curves.clear(); bounds.clear(); }

    bool operator==(const VariableSpeedProfile &rhs) const { return this->curves == rhs.curves && this->bounds == rhs.bounds; }
    bool operator!=(const VariableSpeedProfile &rhs) const { return ! (*this == rhs); }

    // The VariableSpeedTarget bit an extrusion role maps to, or 0 if the role is never affected.
    static uint32_t target_for_role(ExtrusionRole role);
    // Interpolated speed in mm/s for a role at z, from its curve painted along Z, or 0 when there is none.
    coordf_t speed_at(ExtrusionRole role, coordf_t z) const;
    // Is the given extrusion role affected by this profile, along any axis?
    bool     applies_to(ExtrusionRole role) const;
    // The well formed curve of a role painted along an axis, nullptr when there is none.
    const std::vector<coordf_t>* curve(ExtrusionRole role, PaintAxis axis) const;

    template<class Archive> void serialize(Archive &ar) { ar(curves, bounds); }
};

// A flat profile spanning the whole object, used as a starting point for the interactive editor.
extern std::vector<coordf_t> variable_speed_profile_flat(coordf_t object_height, coordf_t speed);

// Linear interpolation of a raw <z, speed> profile, returns fallback when the profile is unusable.
extern coordf_t variable_speed_at(const std::vector<coordf_t> &profile, coordf_t z, coordf_t fallback);

// Same semantics as adjust_layer_height_profile(), applied to a speed profile.
// speed_delta is expressed in mm/s and base_speed is the value the REDUCE / SMOOTH actions
// converge to.
extern void adjust_variable_speed_profile(
    std::vector<coordf_t>       &speed_profile,
    coordf_t                     object_height,
    coordf_t                     base_speed,
    coordf_t                     min_speed,
    coordf_t                     max_speed,
    coordf_t                     z,
    coordf_t                     speed_delta,
    coordf_t                     band_width,
    LayerHeightEditActionType    action);

extern std::vector<coordf_t> smooth_variable_speed_profile(
    const std::vector<coordf_t>        &profile,
    coordf_t                            min_speed,
    coordf_t                            max_speed,
    const HeightProfileSmoothingParams &smoothing_params);

// Orio: the bounds of a curve (VariableSpeedProfile::bounds). Width of the step of the curve just above a bound:
// the layer whose top is at the bound stays on its lower side, as the G-code reads the speed at the top of a layer.
static constexpr coordf_t VARIABLE_SPEED_BOUND_STEP = 0.01;
// What a stroke changed, kept only between the bounds around it: `painted` (the curve after the stroke) on ]lo, hi],
// `before` (the curve before it) elsewhere, with a step just above lo and just above hi. lo = 0 and hi = length for
// no bound on that side. Both curves span [0, length].
extern std::vector<coordf_t> variable_speed_clip_stroke(
    const std::vector<coordf_t> &before,
    const std::vector<coordf_t> &painted,
    coordf_t                     lo,
    coordf_t                     hi,
    coordf_t                     length);
// smooth_variable_speed_profile() on each part of the curve between its bounds (sorted), on its own: the steps at the
// bounds stay.
extern std::vector<coordf_t> smooth_variable_speed_profile_bounded(
    const std::vector<coordf_t>        &profile,
    const std::vector<coordf_t>        &bounds,
    coordf_t                            min_speed,
    coordf_t                            max_speed,
    const HeightProfileSmoothingParams &smoothing_params);

// Produce a 1D texture packed into a 2D texture describing the speed profile, using the very
// same layout as generate_layer_height_texture() so that the variable_layer_height shader can
// be reused as is. Returns number of cells used by the texture of the 0th LOD level.
extern int generate_variable_speed_texture(
    coordf_t                     object_height,
    coordf_t                     min_speed,
    coordf_t                     max_speed,
    const std::vector<coordf_t> &speed_profile,
    void *data, int rows, int cols, bool level_of_detail_2nd_level);

namespace Slicing {
	// Minimum layer height for the variable layer height algorithm. Nozzle index is 1 based.
	coordf_t min_layer_height_from_nozzle(const DynamicPrintConfig &print_config, int idx_nozzle);

	// Maximum layer height for the variable layer height algorithm, 3/4 of a nozzle dimaeter by default,
	// it should not be smaller than the minimum layer height.
	// Nozzle index is 1 based.
	coordf_t max_layer_height_from_nozzle(const DynamicPrintConfig &print_config, int idx_nozzle);
} // namespace Slicing

} // namespace Slic3r

namespace cereal
{
	template<class Archive> void serialize(Archive& archive, Slic3r::t_layer_height_range &lhr) { archive(lhr.first, lhr.second); }
}

#endif /* slic3r_Slicing_hpp_ */
