#ifndef slic3r_PaintAxis_hpp_
#define slic3r_PaintAxis_hpp_

// Orio: painting a quantity along an axis of an object, shared by the tools painting along X / Y as well as
// along Z ("Variable infill and walls"; meant to be reused by "Variable speed" and "Variable layer height").
//
// A painted curve packs <coordinate, value> pairs. Along Z, the coordinate is the height above the bottom of the
// object (the curves painted before the X / Y painting existed). Along X or Y, it is the coordinate in the frame of
// the ModelObject: its volumes placed by their own matrices, before the instance transformation. Moving, rotating or
// scaling the object carries the painting along, and all its instances are painted alike. A curve painted along X or
// Y spans the extent of the object along that axis when it was painted; beyond its ends, the end values apply.
//
// The curves of an object are kept in a map keyed by paint_axis_key(target, axis): the painted quantity in the low
// 16 bits, the axis above. Z being 0, the keys of the curves painted along Z are the ones of the older projects.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r.h"
#include "BoundingBox.hpp"
#include "Point.hpp"
#include "Polygon.hpp"

namespace Slic3r {

class ModelObject;
class PrintObject;

enum PaintAxis : uint32_t {
    paZ = 0,
    paX = 1,
    paY = 2,
};

inline constexpr uint32_t  paint_axis_key(uint32_t target, PaintAxis axis) { return (target & 0xFFFFu) | (uint32_t(axis) << 16); }
inline constexpr uint32_t  paint_axis_key_target(uint32_t key)               { return key & 0xFFFFu; }
inline constexpr PaintAxis paint_axis_key_axis(uint32_t key)                 { return PaintAxis(key >> 16); }
inline constexpr bool      paint_axis_is_valid(uint32_t axis)                { return axis <= uint32_t(paY); }

// Name of an axis in the project files: "x", "y" or "z".
extern const char* paint_axis_name(PaintAxis axis);
// Reads "x", "y" or "z" (any case). Returns false for anything else.
extern bool        paint_axis_from_name(const std::string &name, PaintAxis &axis);

// Extent [min, max] of a ModelObject along its own X (paX) or Y (paY) axis: its model parts placed by their volume
// matrices, before the instance transformation. Walks the convex hulls of the volumes: compute it once, not per layer.
// {0, 0} for an object without any model part.
extern std::pair<double, double> paint_axis_object_extent(const ModelObject &object, PaintAxis axis);

// Coordinate along the X or Y axis of its object of a point of a layer of a PrintObject, whose slices are expressed in
// the frame of PrintObject::trafo_centered() (scaled x, y; slice_z unscaled).
class PaintAxisFrame
{
public:
    // On the plane of a layer, the coordinate is an affine function of the scaled point: gx * x + gy * y + c (in mm).
    struct Plane
    {
        double gx { 0. };
        double gy { 0. };
        double c  { 0. };

        double operator()(const Point &p) const { return this->gx * double(p.x()) + this->gy * double(p.y()) + this->c; }
        double at(double x, double y) const { return this->gx * x + this->gy * y + this->c; }
        // Range of the coordinate over a bounding box.
        std::pair<double, double> range(const BoundingBox &bbox) const;
        // The part of the bounding box where the coordinate lies in [from, to], counter-clockwise; empty if none. On a
        // plane parallel to the axis the coordinate is constant: the whole bounding box or nothing.
        Polygon band(const BoundingBox &bbox, double from, double to) const;
    };

    PaintAxisFrame() = default;
    PaintAxisFrame(const PrintObject &object, PaintAxis axis);
    PaintAxisFrame(const Transform3d &trafo_centered, PaintAxis axis);

    // Plane of the layer sliced at slice_z.
    Plane plane(double slice_z) const;

private:
    // Row of the inverse of trafo_centered() giving the coordinate along the axis.
    Vec3d  m_row    { Vec3d::Zero() };
    double m_offset { 0. };
};

// A run of a painted curve along which its value is taken as constant.
struct PaintAxisBand
{
    double from;
    double to;
    double value;
};
using PaintAxisBands = std::vector<PaintAxisBand>;

// Splits a <coordinate, value> curve into bands of constant value, for the slicer: a few large bands rather than
// countless thin ones.
// - integer_values: the values are rounded, a band ends where the rounded value changes (a number of walls...);
// - otherwise a band ends where the value would leave a window 2 * tolerance wide, its value being the middle of
//   that window: a flat stretch of the curve keeps its exact value.
// Bands narrower than min_width are merged into their neighbour of closest value (the higher one on a tie). The
// first band starts at -PAINT_AXIS_INFINITY and the last one ends at +PAINT_AXIS_INFINITY: beyond the ends of the
// curve, its end values apply. Empty for a malformed curve.
static constexpr double PAINT_AXIS_INFINITY = 1e30;
extern PaintAxisBands paint_axis_bands(const std::vector<double> &curve, bool integer_values, double tolerance, double min_width);
// Index of the band holding a coordinate (bands as returned by paint_axis_bands(), not empty).
extern size_t paint_axis_band_index(const PaintAxisBands &bands, double coordinate);
// The coordinates where the value changes, sorted.
extern std::vector<double> paint_axis_band_boundaries(const PaintAxisBands &bands);

// The boundaries between the bands painted along one axis, on the plane of one layer.
struct PaintAxisCut
{
    PaintAxisFrame::Plane plane;
    std::vector<double>   boundaries;
};
// Adds to a polyline (or to the points of a closed loop) its crossings of the band boundaries of each cut: each segment
// of the result then lies within a single band along each axis, which the midpoint of the segment tells.
extern Points paint_axis_cut_points(const Points &points, const std::vector<PaintAxisCut> &cuts);

} // namespace Slic3r

#endif /* slic3r_PaintAxis_hpp_ */
