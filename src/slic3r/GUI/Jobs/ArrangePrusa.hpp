#ifndef slic3r_ArrangePrusa_hpp_
#define slic3r_ArrangePrusa_hpp_

// Orio: the "Arrange" tool runs the arrange engine of PrusaSlicer 2.9 (src/slic3r-arrange) instead of the
// libnest2d engine of Bambu Studio, except for sequential printing ("by object"), whose clearance rules only the
// engine of Bambu Studio knows. The plates of Bambu Studio are kept: ArrangeJob prepares the ArrangePolygons and
// applies the result as before, this file only replaces the packing itself.

#include <functional>
#include <string>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/ExPolygon.hpp"

namespace Slic3r {

class AppConfig;

namespace GUI {

// Options of the arrange engine of PrusaSlicer, saved in the "arrange" section of the application config.
struct ArrangePrusaSettings
{
    // Shape of the objects: convex hull ("Fast"), real outline for the objects already placed and convex hull for
    // the object being placed ("Balanced"), real outline for all of them ("Accurate").
    enum GeometryHandling { ghFast, ghBalanced, ghAccurate, ghCount };
    // Where the pile of arranged objects goes on the plate. Front = small Y.
    enum Alignment { alCenter, alRearLeft, alFrontLeft, alFrontRight, alRearRight, alRandom, alCount };

    float bed_distance = 0.f;      // mm, minimum distance between the objects and the edges of the plate
    int   geometry     = ghFast;
    int   alignment    = alCenter;

    // postfix: "_fff", "_fff_seq_print" or "_sla", as the spacing and the rotation of the arrange menu.
    // The spacing from the plate is saved per mode, the geometry and the alignment for all the modes.
    static ArrangePrusaSettings load(const AppConfig &cfg, const std::string &postfix);
    void save(AppConfig &cfg, const std::string &postfix) const;
};

struct ArrangePrusaParams
{
    ArrangePrusaSettings settings;
    // Margin (scaled) already removed from each side of the plate by Bambu Studio (skirt, brim of the supports).
    coord_t bed_margin    = 0;
    // Plates the objects may use; the others are left unarranged.
    int     max_bed_count = 1;
    // Full outline of a real object, in the coordinates of its ArrangePolygon::poly (same transformation, no
    // translation). Empty: the convex hull ArrangePolygon::poly is used. Called from several threads.
    std::function<ExPolygons(const arrangement::ArrangePolygon &ap, bool fixed)> outline;
    // Part of the plate where a wipe tower may go (scaled, coordinates of bedpts), empty: the whole plate. Printers with
    // two nozzles: the area both of them reach (Print::get_extruder_shared_printable_polygon()).
    Polygons tower_area;
};

// Arranges `selected` around `fixed` on the plate `bedpts`, as arrangement::arrange() does: translation, rotation,
// bed_idx (UNARRANGED if it does not fit) and itemid (packing order) of the selected items are updated.
// `fixed` contains the objects that stay in place and the virtual objects of Bambu Studio: wipe towers (obstacles
// of their plate) and excluded / calibration regions (removed from every plate). A wipe tower among `selected` (its
// bed_idx: its plate) is moved against the objects arranged on its plate, the whole then aligned; it gets
// UNARRANGED when it is not moved (no object on its plate, fixed objects there, or no room left: then the objects
// are arranged around it where it is). The inflations of the items must be set
// (update_selected_items_inflation(), update_unselected_items_inflation()).
// Uses params.allow_rotations, align_to_y_axis, allow_multi_materials_on_same_plate, align_center, progressind and
// stopcondition.
void arrange_prusa(arrangement::ArrangePolygons       &selected,
                   const arrangement::ArrangePolygons &fixed,
                   const Points                       &bedpts,
                   const arrangement::ArrangeParams   &params,
                   const ArrangePrusaParams           &pparams);

}} // namespace Slic3r::GUI

#endif // slic3r_ArrangePrusa_hpp_
