#include "../Print.hpp"
#include "../ShortestPath.hpp"

#include "ClipperUtils.hpp"
#include "FillLightning.hpp"
#include "Lightning/Generator.hpp"

#include <algorithm>

namespace Slic3r::FillLightning {

void Filler::_fill_surface_single(
    const FillParams              &params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point> &direction,
    ExPolygon                      expolygon,
    Polylines                     &polylines_out)
{
    if (!generator) return;
    const Layer &layer      = generator->getTreesForLayer(this->layer_id);
    Polylines    fill_lines = layer.convertToLines(to_polygons(expolygon), scaled<coord_t>(0.5 * this->spacing - this->overlap));
    
    // Apply multiline offset if needed
    multiline_fill(fill_lines, params, spacing);
    Polylines all_polylines = intersection_pl(std::move(fill_lines), expolygon);
    if (params.dont_connect() || all_polylines.size() <= 1) {
        append(polylines_out, chain_polylines(std::move(all_polylines)));
    } else
        connect_infill(std::move(all_polylines), expolygon, polylines_out, this->spacing, params);
}

void GeneratorDeleter::operator()(Generator *p) {
    delete p;
}

//Orio: see FillLightning.hpp.
RegionGenerators build_generators(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback)
{
    RegionGenerators                      out;
    std::vector<const PrintRegionConfig*> keys; // the region config each generator was built for
    out.by_region.assign(print_object.num_printing_regions(), nullptr);
    for (size_t region_id = 0; region_id < out.by_region.size(); ++ region_id) {
        const PrintRegionConfig &config = print_object.printing_region(region_id).config();
        if (! (config.sparse_infill_density > 0) || config.sparse_infill_pattern != ipLightning)
            continue;
        const size_t idx = std::find_if(keys.begin(), keys.end(), [&config](const PrintRegionConfig *key) {
            return key->sparse_infill_line_width == config.sparse_infill_line_width && key->sparse_infill_density == config.sparse_infill_density &&
                   key->fill_multiline == config.fill_multiline;
        }) - keys.begin();
        if (idx == keys.size()) {
            keys.push_back(&config);
            out.generators.emplace_back(new Generator(print_object, config, throw_on_cancel_callback));
        }
        out.by_region[region_id] = out.generators[idx].get();
    }
    return out;
}

} // namespace Slic3r::FillAdaptive
