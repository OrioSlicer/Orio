///|/ Wave overhang generation.
///|/
///|/ Wave overhangs algorithm: Janis A. Andersons (andersonsjanis).
///|/ Builds on arc-overhang algorithm by Steven McCulloch (stmcculloch).
///|/ PrusaSlicer integration: Steven McCulloch.
///|/ Port to OrcaSlicer: Dennis Klappe (dennisklappe).
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/ Ported to Orio from Unlayered Slicer v0.17.
///|/
#ifndef slic3r_WaveOverhangs_hpp_
#define slic3r_WaveOverhangs_hpp_

#include <tuple>
#include <vector>

#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "Flow.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"

namespace Slic3r { namespace WaveOverhangs {

//Orio: Unlayered signature without the corner-taper parameters, plus min_length_mm
// (filter of Unlayered's AndersonsGenerator: overhang islands whose contour is shorter are skipped).
// Returns one ExtrusionPaths per covered overhang island (role erOverhangPerimeter, wave_overhang = true)
// and the area the waves fill.
std::tuple<std::vector<ExtrusionPaths>, Polygons> generate(
    ExPolygons          infill_area,
    const Polygons     &lower_slices_polygons,
    int                 perimeter_count,
    int                 additional_shell_count,
    double              wave_perimeter_overlap,
    double              minimum_wave_width,
    WaveOverhangPattern wave_pattern,
    double              wave_line_spacing,
    double              wave_line_width,
    const Flow         &overhang_flow,
    double              scaled_resolution,
    int                 max_iterations         = 0,
    double              min_new_area_mm2       = 0.01,
    bool                use_instead_of_bridges = false,
    double              min_length_mm          = 0.0,
    WaveOverhangShape   wave_shape             = WaveOverhangShape::Waves,
    double              arc_max_radius_mm      = 0.0);

} } // namespace Slic3r::WaveOverhangs

#endif
