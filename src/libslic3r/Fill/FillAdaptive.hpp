// Adaptive cubic infill was inspired by the work of @mboerwinkle
// as implemented for Cura.
// https://github.com/Ultimaker/CuraEngine/issues/381
// https://github.com/Ultimaker/CuraEngine/pull/401
//
// Our implementation is more accurate (discretizes a bit less cubes than Cura's)
// by splitting only such cubes which contain a triangle. 
// Our line extraction is time optimal instead of O(n^2) when connecting extracted lines,
// and we also implemented adaptivity for supporting internal overhangs only.

#ifndef slic3r_FillAdaptive_hpp_
#define slic3r_FillAdaptive_hpp_

#include "FillBase.hpp"

struct indexed_triangle_set;

namespace Slic3r {

class PrintObject;

namespace FillAdaptive
{

struct Octree;
// To keep the definition of Octree opaque, we have to define a custom deleter.
struct OctreeDeleter { void operator()(Octree *p); };
using  OctreePtr = std::unique_ptr<Octree, OctreeDeleter>;

//Orio: line spacing of the adaptive cubic or support cubic infill of each printing region of the object (zero for
// a region without such infill). It was averaged over the regions: a modifier at another density changed the density of the
// whole object and not its own.
std::vector<double>             adaptive_fill_line_spacing(const PrintObject &print_object);

//Orio: octrees of an object, one per distinct line spacing (and kind: adaptive or support cubic), shared by the
// printing regions which ask for it.
struct RegionOctrees
{
    std::vector<OctreePtr> octrees;
    std::vector<Octree*>   by_region; // by printing region, nullptr: none
    Octree*                region(size_t region_id) const { return region_id < by_region.size() ? by_region[region_id] : nullptr; }
};

// Rotation of the octree to stand on one of its corners.
Eigen::Quaterniond              transform_to_world();
// Inverse roation of the above.
Eigen::Quaterniond              transform_to_octree();

FillAdaptive::OctreePtr         build_octree(
    // Mesh is rotated to the coordinate system of the octree.
    const indexed_triangle_set  &triangle_mesh,
    // Overhang triangles extracted from fill surfaces with stInternalBridge type, 
    // rotated to the coordinate system of the octree.
    const std::vector<Vec3d>    &overhang_triangles, 
    coordf_t                     line_spacing, 
    // If true, octree is densified below internal overhangs only.
    bool                         support_overhangs_only);

//
// Some of the algorithms used by class FillAdaptive were inspired by
// Cura Engine's class SubDivCube
// https://github.com/Ultimaker/CuraEngine/blob/master/src/infill/SubDivCube.h
//
class Filler : public Slic3r::Fill
{
public:
    ~Filler() override {}

protected:
    Fill* clone() const override { return new Filler(*this); }
	void _fill_surface_single(
	    const FillParams                &params,
	    unsigned int                     thickness_layers,
	    const std::pair<float, Point>   &direction,
	    ExPolygon                        expolygon,
	    Polylines                       &polylines_out) override;
    // Let the G-code export reoder the infill lines.
    //FIXME letting the G-code exporter to reorder infill lines of Adaptive Cubic Infill
    // may not be optimal as the internal infill lines may get extruded before the long infill
    // lines to which the short infill lines are supposed to anchor.
	bool no_sort() const override { return false; }
    bool is_self_crossing() override { return true; }
};

} // namespace FillAdaptive
} // namespace Slic3r

#endif // slic3r_FillAdaptive_hpp_
