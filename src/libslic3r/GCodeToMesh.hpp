#ifndef slic3r_GCodeToMesh_hpp_
#define slic3r_GCodeToMesh_hpp_

// Orio: "G-code to Mesh" tab. Rebuilds the meshes of the objects printed by a G-code: the outer wall of each
// layer, closed into loops and offset outwards by half its line width, gives the outline of the layer, and the
// layers are stacked with their own heights. The X-Y and elephant foot compensations of the slicer may be undone.
// Reads the G-codes of Bambu Studio, Orio, Orca Slicer and PrusaSlicer (their line type comments).

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class GCodeToMesh
{
public:
    // Outer wall of a layer of an object.
    struct Layer
    {
        // Top of the layer and its height written in the G-code (0: not written), mm.
        float              z{ 0.f };
        float              height{ 0.f };
        // Mean line width of the outer wall, mm.
        float              width{ 0.f };
        // Center lines of its closed loops, with their line widths (Bambu Studio prints small loops thinner).
        Polygons           loops;
        std::vector<float> loop_widths;
        // Center lines of its paths which do not close (walls of a single line), with their widths.
        Polylines          open_paths;
        std::vector<float> open_widths;
    };

    struct Object
    {
        // Label of the object in the G-code ("144", "Cube id:0 copy 0"...), empty without labels; its name if the
        // G-code gives it (PrusaSlicer, Orca Slicer, Klipper), otherwise empty.
        std::string        label;
        std::string        name;
        // From the bottom up.
        std::vector<Layer> layers;
    };

    // Size compensations applied by the slicer, mm: > 0, it grew the contours, enlarged the holes, shrank the first
    // layer.
    struct Compensation
    {
        double contour{ 0. };
        double hole{ 0. };
        double elephant_foot{ 0. };
    };

    struct Stats
    {
        // Different layer tops, top of the highest one.
        size_t layers{ 0 };
        float  height{ 0.f };
        // Height of the first layer, of the other ones.
        float  first_layer_height{ 0.f };
        float  layer_height_min{ 0.f };
        float  layer_height_max{ 0.f };
        // Line widths of the outer walls.
        float  width_min{ 0.f };
        float  width_max{ 0.f };
        size_t loops{ 0 };
        size_t open_paths{ 0 };
    };

    // Reads the G-code; throws Slic3r::RuntimeError if the file cannot be read.
    void load(const std::string &path);

    // Value written in the settings block of the G-code, "" if missing.
    std::string  setting(const std::string &key) const;
    // First number of a setting ("0.4,0.4" gives 0.4; "15%" gives 15), 0 if missing.
    double       number(const std::string &key) const;
    // Compensations written in the G-code.
    Compensation compensation() const;
    Stats        stats() const;

    // Outline of each layer of the object, with the compensations undone. throw_on_cancel is called now and then, from
    // several threads, to stop a computation which is no longer wanted.
    std::vector<ExPolygons> slices(const Object &object, const Compensation &undo, const std::function<void()> &throw_on_cancel = [] {}) const;
    // Closed mesh of the object in the coordinates of the G-code, mm, with the compensations undone. Smooth: the slopes
    // go from the middle of a layer to the middle of the next one instead of keeping the steps of the layers.
    indexed_triangle_set    mesh(const Object &object, const Compensation &undo, bool smooth, const std::function<void()> &throw_on_cancel = [] {}) const;

    // Software which wrote the G-code ("BambuStudio 02.07.00.55", "PrusaSlicer 2.7.1+win64"...), empty if unknown.
    std::string                        producer;
    // Largest picture of the print written by the slicer, base64 encoded, and its format ("png" or "jpg"); empty if none.
    std::string                        thumbnail_base64;
    std::string                        thumbnail_format;
    // Print time estimated by the slicer, as written ("6h 13m 2s"); empty if not found.
    std::string                        print_time;
    // Settings block of the G-code (key = value comments).
    std::map<std::string, std::string> settings;
    std::vector<Object>                objects;
    // Paths of overhang walls lying on inner walls (with material on both sides), left out.
    size_t                             overhangs_left_out{ 0 };
};

} // namespace Slic3r

#endif // slic3r_GCodeToMesh_hpp_
