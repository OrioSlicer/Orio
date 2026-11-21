#ifndef slic3r_FillLightning_hpp_
#define slic3r_FillLightning_hpp_

#include "FillBase.hpp"

namespace Slic3r {

class PrintObject;

namespace FillLightning {

class Generator;
// To keep the definition of Octree opaque, we have to define a custom deleter.
struct GeneratorDeleter { void operator()(Generator *p); };
using  GeneratorPtr = std::unique_ptr<Generator, GeneratorDeleter>;

//Orio: generators of an object, one per distinct line width, density and number of lines (multiline infill) of the
// regions with lightning infill, shared by the printing regions which ask for it. There was a single one, built for the first
// region: a modifier at another density had no effect.
struct RegionGenerators
{
    std::vector<GeneratorPtr> generators;
    std::vector<Generator*>   by_region; // by printing region, nullptr: none
    Generator*                region(size_t region_id) const { return region_id < by_region.size() ? by_region[region_id] : nullptr; }
};

RegionGenerators build_generators(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback);

class Filler : public Slic3r::Fill
{
public:
    ~Filler() override = default;
    bool is_self_crossing() override { return false; }

    Generator   *generator { nullptr };
protected:
    Fill* clone() const override { return new Filler(*this); }

    void _fill_surface_single(const FillParams              &params,
                              unsigned int                   thickness_layers,
                              const std::pair<float, Point> &direction,
                              ExPolygon                      expolygon,
                              Polylines &polylines_out) override;

    // Let the G-code export reoder the infill lines.
	bool no_sort() const override { return false; }
};

} // namespace FillAdaptive
} // namespace Slic3r

#endif // slic3r_FillLightning_hpp_
