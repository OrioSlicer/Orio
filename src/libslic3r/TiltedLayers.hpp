#ifndef slic3r_TiltedLayers_hpp_
#define slic3r_TiltedLayers_hpp_

// Orio: tilted layers, to print overhangs on a 3-axis printer with layers leaning towards them.
//
// Idea: the conical slicing of the ZHAW RotBot project (https://github.com/RotBotSlicer/Transform, GPL-3.0),
// popularized by CNC Kitchen, reduced to planes: the print head can follow a plane leaning towards the side
// where it clears the most, while a cone needs the clearance of its worst side.
//
// The user places the zone printed with tilted layers: its first and its last tilted layers. Below the zone the
// layers progressively tilt, pivoting around a horizontal line, and above it they progressively become flat again:
// these transitions are computed from the part they cross. Every layer is a plane:
//     z = G(p, z') = z' - S(z') w(p) + B(z'),        w(p) = p . d
// z' is the height of the layer in the sliced (warped) part, p the XY point, d the downhill direction of the
// layers (the direction of the overhangs), S the slope of the layer (tangent of its angle) and B an offset that
// keeps the pivot of each transition at its height. S and B are piecewise linear in z':
//     z' <= z0       : S = 0                         B = 0                          flat, as sliced
//     z0 <= z' <= z1 : S = t (z' - z0) / (z1 - z0)   B = S w_low                    lower transition
//     z1 <= z' <= z2 : S = t                         B = t w_low                    tilted layers
//     z2 <= z' <= z3 : S = t (z3 - z') / (z3 - z2)   B = t w_low + (S - t) w_high   upper transition
//     z3 <= z'       : S = 0                         B = t (w_low - w_high)         flat again
// The part is warped before slicing (z' of each real point, the inverse of G, which is piecewise linear in z'),
// sliced with the usual planar layers, and every point of the G-code is unwarped with G. x and y never change:
// the warp shears the part vertically, so the sliced volume is printed as is, except in the transitions where
// a layer gets thinner on one side of its pivot and thicker on the other side (the extrusion follows dG/dz').
// Every layer being a plane, a straight line of a layer stays straight once unwarped: only the meshes need to
// be refined, and only in the transitions.

#include "libslic3r.h"
#include "BoundingBox.hpp"
#include "Point.hpp"

#include <admesh/stl.h>

#include <algorithm>
#include <string>

namespace Slic3r {

class ConfigBase;
class ModelObject;

// Clearance of the print head: steepest slope from the nozzle tip that stays below the print head on each of its
// sides, seen from the front of the printer, in degrees. 0 = unknown.
struct HeadClearance
{
    double left  { 0. }; // towards -X
    double right { 0. }; // towards +X
    double front { 0. }; // towards -Y
    double back  { 0. }; // towards +Y

    // Steepest tilt (degrees) of layers rising towards 'uphill' (unit vector in printer coordinates): the side of
    // the print head facing uphill passes over the part printed so far. 0 if a side involved is unknown.
    double max_tilt(const Vec2d &uphill) const;
};

// Translated name of the side of the print head facing uphill, or of its corner between two sides, the one
// HeadClearance::max_tilt() reads: "left side", "back left corner"...
std::string head_side_name(const Vec2d &uphill);

// Clearance set in the printer settings, each unknown side completed by the value measured on printer_model.
HeadClearance head_clearance(const std::string &printer_model, double left, double right, double front, double back);
// Same, read from a printer configuration.
HeadClearance head_clearance(const ConfigBase &printer_config);

// The tilted layers keep this margin (degrees) below the clearance of the print head.
constexpr double TILTED_LAYERS_CLEARANCE_MARGIN = 1.;
// Steepest tilt accepted by the settings (degrees).
constexpr double TILTED_LAYERS_MAX_ANGLE = 45.;

// Settings of the tilted layers of an object (options tilted_layers*).
struct TiltedLayersSettings
{
    bool   enabled      { false };
    double angle        { 0. };  // degrees
    double direction    { 0. };  // degrees, downhill direction in the coordinates of the model object
    // Heights above the bed of the first and of the last tilted layers of the zone, measured in the middle of the
    // object along the downhill direction. end_height 0: up to the top of the object.
    double start_height { 0. };
    double end_height   { 0. };
};
TiltedLayersSettings tilted_layers_settings(const ConfigBase &config);

// Downhill direction of the layers in the coordinates of the parameters: direction_deg is measured in the
// coordinates of the model object, which object_trafo brings to the coordinates of the parameters.
Vec2d tilted_layers_direction(double direction_deg, const Transform3d &object_trafo);
// Inverse: direction (degrees) in the coordinates of the model object of the downhill direction dir.
double tilted_layers_direction_deg(const Vec2d &dir, const Transform3d &object_trafo);

struct TiltedLayersParams
{
    bool   enabled { false };
    Vec2d  dir     { Vec2d::UnitX() }; // downhill direction
    double angle   { 0. };             // radians
    double t       { 0. };             // tan(angle)
    // Heights of the sliced (warped) part where the slope changes, see above.
    double z0 { 0. }, z1 { 0. }, z2 { 0. }, z3 { 0. };
    // Pivots of the lower and upper transitions (values of w).
    double w_low  { 0. };
    double w_high { 0. };
    // Range of w, its middle where the heights of the tilted layers are measured, and bounding box of the object.
    double       w_min    { 0. };
    double       w_max    { 0. };
    double       w_center { 0. };
    BoundingBoxf bbox_xy;
    // Heights at w_center of the first and of the last tilted layers of the zone, and of the tilted layers touching
    // the bottom and the top of the object.
    double start_height  { 0. };
    double end_height    { 0. };
    double bottom_height { 0. };
    double top_height    { 0. };
    // Lengths of the transitions at their pivots, below and above the zone.
    double low_transition  { 0. };
    double high_transition { 0. };
    // The start of the zone was raised, the lower transition needing room above the first layer.
    bool   start_raised { false };
    // The angle was reduced to the clearance of the print head.
    bool   angle_limited { false };
    // The clearance of the print head is unknown on the side the layers rise towards: the layers stay flat.
    bool   clearance_unknown { false };
    // Top of the real and of the warped part.
    double real_max_z   { 0. };
    double warped_max_z { 0. };
    // Largest error of the warped meshes (mm).
    double tolerance { 0.01 };

    double w(const Vec2d &pt) const { return pt.dot(this->dir); }
    // Slope S (tangent of the angle) and offset B of the layer z'.
    double slope(double zw) const;
    double offset(double zw) const;
    // Real height of the point of the layer z' at w, or at pt.
    double real_z_w(double w, double zw) const { return zw - this->slope(zw) * w + this->offset(zw); }
    double real_z(const Vec2d &pt, double zw) const { return this->real_z_w(this->w(pt), zw); }
    // Real thickness of the layer z' at pt divided by its sliced thickness (dG/dz').
    double thickness_ratio(const Vec2d &pt, double zw) const;
    // Height in the warped part of a real point (inverse of real_z()).
    double warped_z(const Vec2d &pt, double z) const;
    // Highest real point of the layer z' over the object.
    double layer_top(double zw) const { return std::max(this->real_z_w(this->w_min, zw), this->real_z_w(this->w_max, zw)); }
    // Is the layer z' printed flat at its sliced height?
    bool   is_identity(double zw) const { return ! this->enabled || zw <= this->z0; }
    // Does the thickness of the layer z' change along the layer?
    bool   in_transition(double zw) const { return this->enabled && ((zw > this->z0 && zw < this->z1) || (zw > this->z2 && zw < this->z3)); }
    // Does the zone reach the top of the object (no upper transition)?
    bool   up_to_top() const { return this->end_height >= this->top_height - EPSILON; }
};

// Model parts of an object, brought by object_trafo into the coordinates of the parameters.
indexed_triangle_set tilted_layers_object_mesh(const ModelObject &model_object, const Transform3d &object_trafo);

// Parameters of the tilted layers of an object whose model parts are object_mesh (see tilted_layers_object_mesh(),
// same object_trafo). The layer heights are the ones of the object: first layer, layer, and the thickest one allowed
// by the nozzle (0: unknown), which limits how fast the layers can tilt in the transitions. The angle is reduced to
// the clearance of the print head if given, and nothing is tilted if this clearance is unknown.
TiltedLayersParams tilted_layers_params(const TiltedLayersSettings &settings, const indexed_triangle_set &object_mesh,
    const Transform3d &object_trafo, double first_layer_height, double layer_height, double max_layer_height,
    const HeadClearance *clearance);

// Mesh already in the coordinates of the parameters -> warped mesh, refined where the warp is not linear.
indexed_triangle_set tilted_layers_warp_mesh(const indexed_triangle_set &its, const TiltedLayersParams &params);

} // namespace Slic3r

#endif // slic3r_TiltedLayers_hpp_
