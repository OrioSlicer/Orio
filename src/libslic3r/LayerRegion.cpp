#include "Layer.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "PerimeterGenerator.hpp"
#include "Print.hpp"
#include "Surface.hpp"
#include "BoundingBox.hpp"
#include "SVG.hpp"
#include "RegionExpansion.hpp"
#include "InfillWallsProfile.hpp"
#include "AABBTreeLines.hpp"

#include <algorithm>
#include <string>
#include <map>
#include <memory>

#include <boost/log/trivial.hpp>
static const double max_deviation = scale_(0.5);
static const double max_variance  = 5 * scale_(0.01) * scale_(0.01);

namespace Slic3r {

Flow LayerRegion::flow(FlowRole role) const
{
    return this->flow(role, m_layer->height);
}

Flow LayerRegion::flow(FlowRole role, double layer_height) const
{
    return m_region->flow(*m_layer->object(), role, layer_height, m_layer->id() == 0);
}

Flow LayerRegion::bridging_flow(FlowRole role, bool thick_bridge) const
{
    const PrintRegion       &region         = this->region();
    const PrintRegionConfig &region_config  = region.config();
    const PrintObject       &print_object   = *this->layer()->object();
    if (thick_bridge) {
        // The old Slic3r way (different from all other slicers): Use rounded extrusions.
        // Get the configured nozzle_diameter for the extruder associated to the flow role requested.
        // Here this->extruder(role) - 1 may underflow to MAX_INT, but then the get_at() will follback to zero'th element, so everything is all right.
        auto nozzle_diameter = float(print_object.print()->config().nozzle_diameter.get_at(region.extruder(role) - 1));
        // Applies default bridge spacing.
        return Flow::bridging_flow(float(sqrt(region_config.bridge_flow)) * nozzle_diameter, nozzle_diameter);
    } else {
        // The same way as other slicers: Use normal extrusions. Apply bridge_flow while maintaining the original spacing.
        return this->flow(role).with_flow_ratio(region_config.bridge_flow);
    }
}

// Fill in layerm->fill_surfaces by trimming the layerm->slices by the cummulative layerm->fill_surfaces.
void LayerRegion::slices_to_fill_surfaces_clipped()
{
    // Note: this method should be idempotent, but fill_surfaces gets modified
    // in place. However we're now only using its boundaries (which are invariant)
    // so we're safe. This guarantees idempotence of prepare_infill() also in case
    // that combine_infill() turns some fill_surface into VOID surfaces.
    // Collect polygons per surface type.
    std::array<SurfacesPtr, size_t(stCount)> by_surface;
    for (Surface &surface : this->slices.surfaces)
        by_surface[size_t(surface.surface_type)].emplace_back(&surface);
    // Trim surfaces by the fill_boundaries.
    this->fill_surfaces.surfaces.clear();
    for (size_t surface_type = 0; surface_type < size_t(stCount); ++ surface_type) {
        const SurfacesPtr &this_surfaces = by_surface[surface_type];
        if (! this_surfaces.empty())
            this->fill_surfaces.append(intersection_ex(this_surfaces, this->fill_expolygons), SurfaceType(surface_type));
    }
}

void LayerRegion::auto_circle_compensation(SurfaceCollection& slices, const AutoContourHolesCompensationParams &auto_contour_holes_compensation_params, float manual_offset)
{
     // filament is 1 base
     int filament_idx = this->region().config().outer_wall_filament_id - 1;

     double limited_speed = auto_contour_holes_compensation_params.circle_compensation_speed[filament_idx];
     double counter_speed_coef      = auto_contour_holes_compensation_params.counter_speed_coef[filament_idx];
     double counter_diameter_coef   = auto_contour_holes_compensation_params.counter_diameter_coef[filament_idx];
     double counter_compensate_coef = scale_(auto_contour_holes_compensation_params.counter_compensate_coef[filament_idx]);

     double hole_speed_coef      = auto_contour_holes_compensation_params.hole_speed_coef[filament_idx];
     double hole_diameter_coef   = auto_contour_holes_compensation_params.hole_diameter_coef[filament_idx];
     double hole_compensate_coef = scale_(auto_contour_holes_compensation_params.hole_compensate_coef[filament_idx]);

     double counter_limit_min_value = scale_(auto_contour_holes_compensation_params.counter_limit_min_value[filament_idx]);
     double counter_limit_max_value = scale_(auto_contour_holes_compensation_params.counter_limit_max_value[filament_idx]);
     double hole_limit_min_value    = scale_(auto_contour_holes_compensation_params.hole_limit_min_value[filament_idx]);
     double hole_limit_max_value    = scale_(auto_contour_holes_compensation_params.hole_limit_max_value[filament_idx]);

     double diameter_limit_value = scale_(auto_contour_holes_compensation_params.diameter_limit[filament_idx]);

    for (Surface &surface : slices.surfaces) {
        Point  center;
        double diameter = 0;
        if (surface.expolygon.contour.is_approx_circle(max_deviation, max_variance, center, diameter)) {
            double offset_value = scale_(counter_speed_coef * limited_speed) + counter_diameter_coef * diameter + counter_compensate_coef;
            if (offset_value < counter_limit_min_value) {
                offset_value = counter_limit_min_value;
            } else if (offset_value > counter_limit_max_value) {
                offset_value = counter_limit_max_value;
            }
            offset_value -= manual_offset / 2;
            Polygons offseted_polys = offset(surface.expolygon.contour, offset_value);
            if (offseted_polys.size() == 1) {
                surface.expolygon.contour = offseted_polys[0];
                if (diameter < diameter_limit_value)
                    surface.counter_circle_compensation = true;
            }
        }
        for (size_t i = 0; i < surface.expolygon.holes.size(); ++i) {
            Polygon &hole = surface.expolygon.holes[i];
            if (hole.is_approx_circle(max_deviation, max_variance, center, diameter)) {
                double offset_value = scale_(hole_speed_coef * limited_speed) + hole_diameter_coef * diameter + hole_compensate_coef;
                if (offset_value < hole_limit_min_value) {
                    offset_value = hole_limit_min_value;
                } else if (offset_value > hole_limit_max_value) {
                    offset_value = hole_limit_max_value;
                }
                // positive value means shrinking hole, which oppsite to contour
                offset_value            = -offset_value;
                offset_value -= manual_offset / 2;
                Polygons offseted_polys = offset(hole, offset_value);
                if (offseted_polys.size() == 1) {
                    hole = offseted_polys[0];
                    if (diameter < diameter_limit_value)
                        surface.holes_circle_compensation.push_back(i);
                }
            }
        }
    }
}

void LayerRegion::make_perimeters(const SurfaceCollection &slices, const PerimeterRegions &perimeter_regions, SurfaceCollection *fill_surfaces, ExPolygons *fill_no_overlap, std::vector<LoopNode> &loop_nodes, int wall_loops_override)
{
    this->perimeters.clear();
    this->thin_fills.clear();

    const PrintConfig &      print_config  = this->layer()->object()->print()->config();
    //Orio: the "Variable infill and walls" tool may have painted another wall count or sparse infill
    // density on this layer, and "Alternate extra wall" adds a wall on every other layer (layer_wall_loops()).
    // The perimeter generator then works on a copy of the region config.
    const PrintRegionConfig &region_config_base = this->region().config();
    //Orio: or the walls shared with other regions with another number of walls (see Layer::make_perimeters()).
    const int                wall_loops         = wall_loops_override >= 0 ? wall_loops_override : layer_wall_loops(*this->layer(), region_config_base);
    const coordf_t           painted_density    = infill_walls_density(*this->layer(), region_config_base);
    std::unique_ptr<PrintRegionConfig> region_config_painted;
    if (wall_loops != region_config_base.wall_loops.value || painted_density != region_config_base.sparse_infill_density.value) {
        region_config_painted = std::make_unique<PrintRegionConfig>(region_config_base);
        region_config_painted->wall_loops.value            = wall_loops;
        region_config_painted->sparse_infill_density.value = painted_density;
    }
    const PrintRegionConfig &region_config = region_config_painted ? *region_config_painted : region_config_base;
    const PrintObjectConfig& object_config = this->layer()->object()->config();
    // This needs to be in sync with PrintObject::_slice() slicing_mode_normal_below_layer!
    bool spiral_mode = print_config.spiral_mode &&
        //FIXME account for raft layers.
        (this->layer()->id() >= size_t(region_config.bottom_shell_layers.value) &&
         this->layer()->print_z >= region_config.bottom_shell_thickness - EPSILON);

    PerimeterGenerator g(
        // input:
        &slices,
        this->layer()->height,
        this->flow(frPerimeter),
        &region_config,
        &this->layer()->object()->config(),
        &print_config,
        spiral_mode,
        // output:
        &this->perimeters,
        &this->thin_fills,
        fill_surfaces,
        //BBS
        fill_no_overlap,
        &loop_nodes
    );

    if (this->layer()->lower_layer != nullptr)
        // Cummulative sum of polygons over all the regions.
        g.lower_slices = &this->layer()->lower_layer->lslices;
    if (this->layer()->upper_layer != NULL)
        g.upper_slices = &this->layer()->upper_layer->lslices;

    g.layer_id              = (int)this->layer()->id();
    g.slice_z               = this->layer()->slice_z;
    g.ext_perimeter_flow    = this->flow(frExternalPerimeter);
    g.overhang_flow         = this->bridging_flow(frPerimeter, object_config.thick_bridges);
    g.solid_infill_flow     = this->flow(frSolidInfill);
    g.perimeter_regions     = &perimeter_regions;
    //Orio: wave overhangs (Unlayered), only on the layers printed flat: in the tilted zone the wave fronts would
    // grow downhill (tilted layers, TiltedLayers.hpp).
    // Heights measured above the raft, as GCode::begin_tilted_extrusion().
    const TiltedLayersParams &tp = this->layer()->object()->tilted_params();
    const double zs = this->layer()->object()->slicing_parameters().object_print_z_min;
    auto flat = [&tp, zs](double z) { return std::abs(tp.slope(z - zs)) < EPSILON && ! tp.in_transition(z - zs); };
    const bool wave_flat = ! tp.enabled || (flat(this->layer()->print_z - this->layer()->height) && flat(this->layer()->print_z));
    g.wave_overhangs_enabled = region_config.wave_overhangs.value && this->layer()->lower_layer != nullptr && wave_flat;

    if (this->layer()->object()->config().wall_generator.value == PerimeterGeneratorType::Arachne && !spiral_mode)
        g.process_arachne();
    else
        g.process_classic();

    //Orio: wave overhangs, the walls added later over the area left to the infill (make_extra_perimeters(),
    // infill_walls_make_zone_walls()) stay out of the waves.
    if (! g.out_wave_overhang_covered_polygons.empty())
        *fill_no_overlap = diff_ex(*fill_no_overlap, g.out_wave_overhang_covered_polygons);

    //Orio: wave overhangs, the areas covered by the waves of all the regions of the layer (see Layer.hpp).
    append(this->layer()->wave_overhang_floor_polygons, std::move(g.out_wave_overhang_floor_polygons));
    append(this->layer()->wave_overhang_covered_polygons, std::move(g.out_wave_overhang_covered_polygons));
}

//Orio: the walls generated by make_extra_perimeters() are inner walls (the generator made their outermost loops
// external), numbered after the walls shared with the other regions (wall_index, used by the brick layers).
static void set_inner_walls(ExtrusionEntity &entity, int wall_index_offset)
{
    auto set_path = [wall_index_offset](ExtrusionPath &path) {
        if (path.role() == erExternalPerimeter)
            path.set_extrusion_role(erPerimeter);
        if (path.wall_index >= 0)
            path.wall_index += wall_index_offset;
    };
    if (auto *collection = dynamic_cast<ExtrusionEntityCollection*>(&entity)) {
        for (ExtrusionEntity *child : collection->entities)
            set_inner_walls(*child, wall_index_offset);
    } else if (auto *loop = dynamic_cast<ExtrusionLoop*>(&entity)) {
        for (ExtrusionPath &path : loop->paths)
            set_path(path);
    } else if (auto *multi_path = dynamic_cast<ExtrusionMultiPath*>(&entity)) {
        for (ExtrusionPath &path : multi_path->paths)
            set_path(path);
    } else if (auto *path = dynamic_cast<ExtrusionPath*>(&entity))
        set_path(*path);
}

//Orio: keeps the parts of extrusions inside a zone, cutting them where they cross its boundary (walls in addition of
// a region, kept only in it, see LayerRegion::make_extra_perimeters()).
namespace {
class ExtrusionZoneClipper
{
public:
    explicit ExtrusionZoneClipper(const ExPolygons &zone) : m_bbox(get_extents(zone)), m_boundary(to_lines(zone)) {}

    // In place. The collections are kept (those of the perimeter generator are indexed by the loop nodes of the layer, see
    // Layer::recrod_cooling_node_for_each_extrusion()), the emptied ones removed. A loop or a path crossing the boundary is
    // replaced by its pieces in the zone, those shorter than their width being dropped.
    void clip(ExtrusionEntityCollection &collection) const
    {
        ExtrusionEntitiesPtr out;
        out.reserve(collection.entities.size());
        for (ExtrusionEntity *entity : collection.entities) {
            bool keep = false;
            if (auto *child = dynamic_cast<ExtrusionEntityCollection*>(entity)) {
                this->clip(*child);
                keep = ! child->entities.empty();
            } else if (auto *loop = dynamic_cast<ExtrusionLoop*>(entity))
                keep = this->clip_paths(loop->paths, true, true, out);
            else if (auto *multi_path = dynamic_cast<ExtrusionMultiPath*>(entity))
                keep = this->clip_paths(multi_path->paths, false, true, out);
            else if (auto *path = dynamic_cast<ExtrusionPath*>(entity))
                keep = this->clip_paths(ExtrusionPaths{ *path }, false, false, out);
            else
                keep = this->inside(entity->first_point());
            if (keep)
                out.push_back(entity);
            else
                delete entity;
        }
        collection.entities = std::move(out);
    }

private:
    bool inside(const Point &point) const { return m_bbox.contains(point) && m_boundary.outside(point) <= 0; }

    // The points of a polyline, with the points where it crosses the boundary of the zone.
    Points cut(const Points &points) const
    {
        Points out;
        out.reserve(points.size());
        for (size_t i = 0; i < points.size(); ++ i) {
            if (i > 0)
                for (const auto &crossing : m_boundary.intersections_with_line<true>(Line(points[i - 1], points[i]))) {
                    const Point point(crossing.first);
                    if (point != out.back() && point != points[i])
                        out.push_back(point);
                }
            if (out.empty() || points[i] != out.back())
                out.push_back(points[i]);
        }
        return out;
    }

    // Cuts consecutive paths (a loop when closed) where they cross the boundary of the zone. Returns true when they are all in
    // it, else adds to out their pieces in it: runs of pieces continuing each other (ExtrusionMultiPath when multi_path: the
    // brick layers only take walls made of loops or multi-paths), the run through the start of a loop staying in one piece.
    bool clip_paths(const ExtrusionPaths &paths, bool closed, bool multi_path, ExtrusionEntitiesPtr &out) const
    {
        std::vector<ExtrusionPaths> runs;
        bool in_run       = false;
        bool all_inside   = true;
        bool first_known  = false;
        bool first_inside = false;
        bool last_inside  = false;
        for (const ExtrusionPath &path : paths) {
            const Points points = this->cut(path.polyline.points);
            if (points.size() < 2)
                continue;
            // In the zone or not, segment by segment (the crossing points end the segments).
            std::vector<char> kept(points.size() - 1);
            for (size_t i = 0; i + 1 < points.size(); ++ i)
                kept[i] = this->inside(Point((points[i].x() + points[i + 1].x()) / 2, (points[i].y() + points[i + 1].y()) / 2));
            if (! first_known) {
                first_known  = true;
                first_inside = kept.front() != 0;
            }
            last_inside = kept.back() != 0;
            for (size_t i = 0; i < kept.size();) {
                size_t j = i + 1;
                while (j < kept.size() && kept[j] == kept[i])
                    ++ j;
                // Segments [i, j), points [i, j], on the same side of the boundary.
                if (kept[i]) {
                    if (! in_run) {
                        runs.emplace_back();
                        in_run = true;
                    }
                    runs.back().emplace_back(Polyline(Points(points.begin() + i, points.begin() + j + 1)), path);
                } else {
                    all_inside = false;
                    in_run     = false;
                }
                i = j;
            }
        }
        if (all_inside)
            return true;
        if (closed && first_inside && last_inside && runs.size() > 1) {
            // The run through the start of the loop: its end goes first.
            ExtrusionPaths &first = runs.front();
            first.insert(first.begin(), std::make_move_iterator(runs.back().begin()), std::make_move_iterator(runs.back().end()));
            runs.pop_back();
        }
        for (ExtrusionPaths &run : runs) {
            double length = 0.;
            for (const ExtrusionPath &path : run)
                length += path.polyline.length();
            if (length < scale_(run.front().width))
                continue;
            if (multi_path)
                out.push_back(new ExtrusionMultiPath(run));
            else
                for (ExtrusionPath &path : run)
                    out.push_back(new ExtrusionPath(std::move(path)));
        }
        return false;
    }

    BoundingBox                         m_bbox;
    AABBTreeLines::LinesDistancer<Line> m_boundary;
};
} // namespace

//Orio: see Layer.hpp.
void LayerRegion::make_extra_perimeters(int extra_wall_loops, int shared_wall_loops, std::vector<LoopNode> &loop_nodes)
{
    // Over the area left to the infill by the shared walls around all the regions sharing them, kept in this region.
    const ExPolygons &area = this->layer()->shared_walls_fill_area.empty() ? this->fill_no_overlap_expolygons : this->layer()->shared_walls_fill_area;
    this->make_extra_perimeters(extra_wall_loops, shared_wall_loops, area, to_expolygons(this->slices.surfaces), loop_nodes);
}

//Orio: see Layer.hpp.
void LayerRegion::make_extra_perimeters(int extra_wall_loops, int shared_wall_loops, const ExPolygons &area, const ExPolygons &zone,
                                        std::vector<LoopNode> &loop_nodes)
{
    const Layer             &layer         = *this->layer();
    const PrintConfig       &print_config  = layer.object()->print()->config();
    const PrintObjectConfig &object_config = layer.object()->config();
    // A single wall everywhere on the first layer or the topmost one: already there, shared.
    if (extra_wall_loops <= 0 || this->fill_no_overlap_expolygons.empty() || zone.empty() ||
        (object_config.only_one_wall_first_layer && layer.id() == 0) ||
        (object_config.top_one_wall_type != TopOneWallType::None && layer.upper_layer == nullptr))
        return;
    // The islands of the area around the zone (area is not read after this: it may be fill_no_overlap_expolygons).
    const BoundingBox zone_bbox = get_extents(zone);
    ExPolygons        islands;
    for (const ExPolygon &island : area)
        if (get_extents(island).overlap(zone_bbox))
            islands.emplace_back(island);
    if (islands.empty())
        return;

    // Inner walls, inside the shared ones. Without shared walls (0 walls on this layer for another region), all the walls
    // of this region, outer wall included, as make_perimeters() would generate them.
    const bool               inner_walls        = shared_wall_loops > 0;
    const Flow               perimeter_flow     = this->flow(frPerimeter);
    // Config of the inner walls: no outer wall among them (precise outer wall, fuzzy skin of the outer walls).
    const PrintRegionConfig &region_config_base = this->region().config();
    PrintRegionConfig        region_config      = region_config_base;
    region_config.wall_loops.value            = extra_wall_loops;
    region_config.sparse_infill_density.value = infill_walls_density(layer, region_config_base);
    if (inner_walls) {
        region_config.precise_outer_wall.value = false;
        if (region_config.fuzzy_skin.value == FuzzySkinType::External || region_config.fuzzy_skin.value == FuzzySkinType::All)
            region_config.fuzzy_skin.value = FuzzySkinType::None;
    }

    // Generated over the area left to the infill by the shared walls. This area starts half a wall spacing inside the last
    // shared wall and the generator puts the first wall half a wall width inside it: grown by the difference, for the first
    // extra wall to overlap the last shared wall as usual (one wall spacing between them).
    SurfaceCollection         slices;
    slices.append(inner_walls ? offset_ex(islands, float(scale_(0.5 * (perimeter_flow.width() - perimeter_flow.spacing())))) :
                                islands,
                  stInternal);
    SurfaceCollection         fill_surfaces;
    ExPolygons                fill_no_overlap;
    ExtrusionEntityCollection perimeters;
    ExtrusionEntityCollection thin_fills;
    const PerimeterRegions    perimeter_regions;
    PerimeterGenerator g(
        // input:
        &slices,
        layer.height,
        perimeter_flow,
        &region_config,
        &object_config,
        &print_config,
        false,
        // output:
        &perimeters,
        &thin_fills,
        &fill_surfaces,
        &fill_no_overlap,
        &loop_nodes
    );
    if (layer.lower_layer != nullptr)
        g.lower_slices = &layer.lower_layer->lslices;
    if (layer.upper_layer != nullptr)
        g.upper_slices = &layer.upper_layer->lslices;
    g.layer_id           = (int)layer.id();
    g.slice_z            = layer.slice_z;
    // Inner walls: the flow of the inner walls for all of them.
    g.ext_perimeter_flow = inner_walls ? perimeter_flow : this->flow(frExternalPerimeter);
    g.overhang_flow      = this->bridging_flow(frPerimeter, object_config.thick_bridges);
    g.solid_infill_flow  = this->flow(frSolidInfill);
    g.perimeter_regions  = &perimeter_regions;
    if (object_config.wall_generator.value == PerimeterGeneratorType::Arachne)
        g.process_arachne();
    else
        g.process_classic();

    if (inner_walls)
        set_inner_walls(perimeters, shared_wall_loops);
    // Only their parts in the zone: they follow the contour and stop at the boundary of the zone, rather than closing along it
    // across the part. Where this boundary crosses the area (outside: the area beyond it), they stop one infill line and half a
    // wall width (their round end) before it, room for an infill anchor wrapping their ends (hook_lines below). Not on the
    // first and the last layers of the object (seen from below or from above): there the walls go up to the boundary.
    const Flow       infill_flow  = this->flow(frInfill);
    const ExPolygons outside      = diff_ex(islands, zone);
    const bool       hooks        = ! outside.empty() && region_config.sparse_infill_density.value > 0. &&
                                    layer.lower_layer != nullptr && layer.upper_layer != nullptr;
    const double     hook_spacing = double(infill_flow.scaled_spacing());
    const double     wall_inset   = hook_spacing + 0.5 * double(perimeter_flow.scaled_width());
    const ExtrusionZoneClipper clipper(hooks ? diff_ex(zone, offset_ex(outside, float(wall_inset))) : zone);
    clipper.clip(perimeters);
    clipper.clip(thin_fills);
    // Printed before the shared walls (inner walls first), unless the outer wall is printed first.
    ExtrusionEntitiesPtr extra = std::move(perimeters.entities);
    perimeters.entities.clear();
    this->perimeters.entities.insert(region_config_base.wall_sequence.value == WallSequence::OuterInner ? this->perimeters.entities.end() :
                                                                                                         this->perimeters.entities.begin(),
                                     extra.begin(), extra.end());
    this->thin_fills.append(std::move(thin_fills.entities));
    thin_fills.entities.clear();

    // Infill anchor wrapping the ends of these walls inside the part: an infill line half a line away from them, along their inner
    // side over the anchor length (sparse_infill_anchor, 2 to 10 mm), around their end and along it down to the shared walls. The
    // infill on both sides anchors to it.
    Polylines hook_lines;
    if (hooks && ! extra.empty()) {
        const double     length = scale_(std::clamp(region_config_base.sparse_infill_anchor.get_abs_value(infill_flow.spacing()), 2., 10.));
        const ExPolygons near_boundary = intersection_ex(intersection_ex(zone, islands), offset_ex(outside, float(wall_inset + length)));
        Polygons footprint;
        for (const ExtrusionEntity *entity : extra)
            entity->polygons_covered_by_width(footprint, float(SCALED_EPSILON));
        // The walls near the boundary, the gaps between them closed.
        const float      width = float(perimeter_flow.scaled_width());
        const ExPolygons walls = offset2_ex(intersection_ex(union_ex(footprint), offset_ex(outside, float(wall_inset + length + 2. * hook_spacing))),
                                            width, - width);
        hook_lines = intersection_pl(to_polylines(offset_ex(walls, float(0.5 * hook_spacing))), near_boundary);
        // The line around a wall end may come in two pieces, cut at the start of the closed line it comes from: joined.
        for (bool joined = true; joined;) {
            joined = false;
            for (size_t i = 0; i < hook_lines.size() && ! joined; ++ i)
                for (size_t j = 0; j < hook_lines.size() && ! joined; ++ j)
                    if (i != j && hook_lines[i].last_point() == hook_lines[j].first_point()) {
                        hook_lines[i].points.insert(hook_lines[i].points.end(), hook_lines[j].points.begin() + 1, hook_lines[j].points.end());
                        hook_lines.erase(hook_lines.begin() + j);
                        joined = true;
                    }
        }
        hook_lines.erase(std::remove_if(hook_lines.begin(), hook_lines.end(), [hook_spacing](const Polyline &line) { return line.length() < hook_spacing; }),
                         hook_lines.end());
    }

    // The infill keeps its area outside the zone and gets, inside it, the area left by these walls, without the anchors (with the
    // usual overlap of the infill on what it touches, infill_wall_overlap).
    const Surface surface_template = this->fill_surfaces.surfaces.empty() ? Surface(stInternal) : this->fill_surfaces.surfaces.front();
    ExPolygons    fill             = diff_ex(this->fill_expolygons, zone);
    append(fill, intersection_ex(fill_surfaces.surfaces, intersection_ex(this->fill_expolygons, zone)));
    ExPolygons fill_no_overlap_left = diff_ex(this->fill_no_overlap_expolygons, zone);
    append(fill_no_overlap_left, intersection_ex(fill_no_overlap, intersection_ex(this->fill_no_overlap_expolygons, zone)));
    if (! hook_lines.empty()) {
        const double overlap = scale_(region_config_base.infill_wall_overlap.get_abs_value(infill_flow.spacing()));
        fill                 = diff_ex(union_ex(fill), offset(hook_lines, float(std::max(0., 0.5 * hook_spacing - overlap))));
        fill_no_overlap_left = diff_ex(union_ex(fill_no_overlap_left), offset(hook_lines, float(0.5 * hook_spacing)));
        // Printed with the infill (Layer::make_fills() moves the thin fills to the fills).
        for (Polyline &line : hook_lines) {
            ExtrusionPath path(erInternalInfill, infill_flow.mm3_per_mm(), infill_flow.width(), infill_flow.height());
            path.polyline = std::move(line);
            this->thin_fills.append(std::move(path));
        }
    }
    this->fill_expolygons = union_ex(fill);
    this->fill_surfaces.set(this->fill_expolygons, surface_template);
    this->fill_no_overlap_expolygons = union_ex(fill_no_overlap_left);
}


#if 1

// Extract surfaces of given type from surfaces, extract fill (layer) thickness of one of the surfaces.
static ExPolygons fill_surfaces_extract_expolygons(Surfaces &surfaces, std::initializer_list<SurfaceType> surface_types, double &thickness)
{
    size_t cnt = 0;
    for (const Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end()) {
            ++cnt;
            thickness = surface.thickness;
        }
    if (cnt == 0)
        return {};

    ExPolygons out;
    out.reserve(cnt);
    for (Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end())
            out.emplace_back(std::move(surface.expolygon));
    return out;
}

// Cache for detecting bridge orientation and merging regions with overlapping expansions.
struct Bridge {
    ExPolygon expolygon;
    uint32_t group_id;
    std::vector<Algorithm::RegionExpansionEx>::const_iterator bridge_expansion_begin;
    std::optional<double> angle{std::nullopt};
};

// Group the bridge surfaces by overlaps.
uint32_t group_id(std::vector<Bridge> &bridges, uint32_t src_id) {
    uint32_t group_id = bridges[src_id].group_id;
    while (group_id != src_id) {
        src_id = group_id;
        group_id = bridges[src_id].group_id;
    }
    bridges[src_id].group_id = group_id;
    return group_id;
};

std::vector<Bridge> get_grouped_bridges(
    ExPolygons&& bridge_expolygons,
    const std::vector<Algorithm::RegionExpansionEx>& bridge_expansions
) {
    using namespace Algorithm;

    std::vector<Bridge> result;
    {
        result.reserve(bridge_expansions.size());
        uint32_t group_id = 0;
        using std::move_iterator;
        for (ExPolygon& expolygon : bridge_expolygons)
            result.push_back({ std::move(expolygon), group_id ++, bridge_expansions.end() });
    }


    // Detect overlaps of bridge anchors inside their respective shell regions.
    // bridge_expansions are sorted by boundary id and source id.
    for (auto expansion_iterator = bridge_expansions.begin(); expansion_iterator != bridge_expansions.end();) {
        auto boundary_region_begin = expansion_iterator;
        auto boundary_region_end = std::find_if(
            next(expansion_iterator),
            bridge_expansions.end(),
            [&](const RegionExpansionEx& expansion){
                return expansion.boundary_id != expansion_iterator->boundary_id;
            }
        );

        // Cache of bboxes per expansion boundary.
        std::vector<BoundingBox> bounding_boxes;
        bounding_boxes.reserve(std::distance(boundary_region_begin, boundary_region_end));
        std::transform(
            boundary_region_begin,
            boundary_region_end,
            std::back_inserter(bounding_boxes),
            [](const RegionExpansionEx& expansion){
                return get_extents(expansion.expolygon.contour);
            }
        );

        // For each bridge anchor of the current source:
        for (;expansion_iterator != boundary_region_end; ++expansion_iterator) {
            auto candidate_iterator = std::next(expansion_iterator);
            for (;candidate_iterator != boundary_region_end; ++candidate_iterator) {
                const BoundingBox& current_bounding_box{
                    bounding_boxes[expansion_iterator - boundary_region_begin]
                };
                const BoundingBox& candidate_bounding_box{
                    bounding_boxes[candidate_iterator - boundary_region_begin]
                };
                if (
                    expansion_iterator->src_id != candidate_iterator->src_id
                    && current_bounding_box.overlap(candidate_bounding_box)
                    // One may ignore holes, they are irrelevant for intersection test.
                    && !intersection(expansion_iterator->expolygon.contour, candidate_iterator->expolygon.contour).empty()
                ) {
                    // The two bridge regions intersect. Give them the same (lower) group id.
                    uint32_t id  = group_id(result, expansion_iterator->src_id);
                    uint32_t id2 = group_id(result, candidate_iterator->src_id);
                    if (id < id2)
                        result[id2].group_id = id;
                    else
                        result[id].group_id = id2;
                }
            }
        }
    }
    return result;
}

void detect_bridge_directions(
    const Algorithm::WaveSeeds& bridge_anchors,
    std::vector<Bridge>& bridges,
    const std::vector<ExpansionZone>& expansion_zones
) {
    if (expansion_zones.empty()) {
        throw std::runtime_error("At least one expansion zone must exist!");
    }
    auto it_bridge_anchor = bridge_anchors.begin();
    for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id) {
        Bridge &bridge = bridges[bridge_id];
        Polygons anchor_areas;
        int32_t last_anchor_id = -1;
        for (; it_bridge_anchor != bridge_anchors.end() && it_bridge_anchor->src == bridge_id; ++ it_bridge_anchor) {
            if (last_anchor_id != int(it_bridge_anchor->boundary)) {
                last_anchor_id = int(it_bridge_anchor->boundary);

                unsigned start_index{};
                unsigned end_index{};
                for (const ExpansionZone& expansion_zone: expansion_zones) {
                    end_index += expansion_zone.expolygons.size();
                    if (last_anchor_id < static_cast<int64_t>(end_index)) {
                        append(anchor_areas, to_polygons(expansion_zone.expolygons[last_anchor_id - start_index]));
                        break;
                    }
                    start_index += expansion_zone.expolygons.size();
                }
            }
        }
        Lines lines{to_lines(diff_pl(to_polylines(bridge.expolygon), expand(anchor_areas, float(SCALED_EPSILON))))};
        auto [bridging_dir, unsupported_dist] = detect_bridging_direction(lines, to_polygons(bridge.expolygon));
        bridge.angle = M_PI + std::atan2(bridging_dir.y(), bridging_dir.x());

        if constexpr (false) {
            coordf_t    stroke_width = scale_(0.06);
            BoundingBox bbox         = get_extents(anchor_areas);
            bbox.merge(get_extents(bridge.expolygon));
            bbox.offset(scale_(1.));
            ::Slic3r::SVG
                svg(debug_out_path(("bridge" + std::to_string(*bridge.angle) + "_" /* + std::to_string(this->layer()->bottom_z())*/).c_str()),
                bbox);
            svg.draw(bridge.expolygon, "cyan");
            svg.draw(lines, "green", stroke_width);
            svg.draw(anchor_areas, "red");
        }
    }
}

Surfaces merge_bridges(
    std::vector<Bridge>& bridges,
    const std::vector<Algorithm::RegionExpansionEx>& bridge_expansions,
    const float closing_radius
) {
    for (auto it = bridge_expansions.begin(); it != bridge_expansions.end(); ) {
        bridges[it->src_id].bridge_expansion_begin = it;
        uint32_t src_id = it->src_id;
        for (++ it; it != bridge_expansions.end() && it->src_id == src_id; ++ it) ;
    }

    Surfaces result;
    for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id) {
        if (group_id(bridges, bridge_id) == bridge_id) {
            // Head of the group.
            Polygons acc;
            for (uint32_t bridge_id2 = bridge_id; bridge_id2 < uint32_t(bridges.size()); ++ bridge_id2)
                if (group_id(bridges, bridge_id2) == bridge_id) {
                    append(acc, to_polygons(std::move(bridges[bridge_id2].expolygon)));
                    auto it_bridge_expansion = bridges[bridge_id2].bridge_expansion_begin;
                    assert(it_bridge_expansion == bridge_expansions.end() || it_bridge_expansion->src_id == bridge_id2);
                    for (; it_bridge_expansion != bridge_expansions.end() && it_bridge_expansion->src_id == bridge_id2; ++ it_bridge_expansion)
                        append(acc, to_polygons(it_bridge_expansion->expolygon));
                }
            //FIXME try to be smart and pick the best bridging angle for all?
            if (!bridges[bridge_id].angle) {
                assert(false && "Bridge angle must be pre-calculated!");
            }
            Surface templ{ stBottomBridge, {} };
            templ.bridge_angle = bridges[bridge_id].angle ? *bridges[bridge_id].angle : -1;
            //NOTE: The current regularization of the shells can create small unasigned regions in the object (E.G. benchy)
            // without the following closing operation, those regions will stay unfilled and cause small holes in the expanded surface.
            // look for narrow_ensure_vertical_wall_thickness_region_radius filter.
            ExPolygons final = closing_ex(acc, closing_radius);
            // without safety offset, artifacts are generated (GH #2494)
            // union_safety_offset_ex(acc)
            for (ExPolygon &ex : final)
                result.emplace_back(templ, std::move(ex));
        }
    }
    return result;
}

struct ExpansionResult {
    Algorithm::WaveSeeds anchors;
    std::vector<Algorithm::RegionExpansionEx> expansions;
};

ExpansionResult expand_expolygons(
    const ExPolygons& expolygons,
    std::vector<ExpansionZone>& expansion_zones
) {
    using namespace Algorithm;
    WaveSeeds bridge_anchors;
    std::vector<RegionExpansionEx> bridge_expansions;

    unsigned processed_bridges_count = 0;
    for (ExpansionZone& expansion_zone : expansion_zones) {
        WaveSeeds seeds{wave_seeds(
            expolygons,
            expansion_zone.expolygons,
            expansion_zone.parameters.tiny_expansion,
            true
        )};
        std::vector<RegionExpansionEx> expansions{propagate_waves_ex(
            seeds,
            expansion_zone.expolygons,
            expansion_zone.parameters
        )};

        for (WaveSeed &seed : seeds)
            seed.boundary += processed_bridges_count;
        for (RegionExpansionEx &expansion : expansions)
            expansion.boundary_id += processed_bridges_count;

        expansion_zone.expanded_into = ! expansions.empty();

        append(bridge_anchors, std::move(seeds));
        append(bridge_expansions, std::move(expansions));

        processed_bridges_count += expansion_zone.expolygons.size();
    }
    return {bridge_anchors, bridge_expansions};
}

// Extract bridging surfaces from "surfaces", expand them into "shells" using expansion_params,
// detect bridges.
// Trim "shells" by the expanded bridges.
Surfaces expand_bridges_detect_orientations(
    Surfaces &surfaces,
    std::vector<ExpansionZone>& expansion_zones,
    const float closing_radius
)
{
    using namespace Slic3r::Algorithm;

    double thickness;
    ExPolygons bridge_expolygons = fill_surfaces_extract_expolygons(surfaces, {stBottomBridge}, thickness);
    if (bridge_expolygons.empty())
        return {};

    // Calculate bridge anchors and their expansions in their respective shell region.
    ExpansionResult expansion_result{expand_expolygons(
        bridge_expolygons,
        expansion_zones
    )};

    std::vector<Bridge> bridges{get_grouped_bridges(
        std::move(bridge_expolygons),
        expansion_result.expansions
    )};
    bridge_expolygons.clear();

    std::sort(expansion_result.anchors.begin(), expansion_result.anchors.end(), Algorithm::lower_by_src_and_boundary);
    detect_bridge_directions(expansion_result.anchors, bridges, expansion_zones);

    // Merge the groups with the same group id, produce surfaces by merging source overhangs with their newly expanded anchors.
    std::sort(expansion_result.expansions.begin(), expansion_result.expansions.end(), [](auto &l, auto &r) {
        return l.src_id < r.src_id || (l.src_id == r.src_id && l.boundary_id < r.boundary_id);
    });
    Surfaces out{merge_bridges(bridges, expansion_result.expansions, closing_radius)};

    // Clip by the expanded bridges.
    for (ExpansionZone& expansion_zone : expansion_zones)
        if (expansion_zone.expanded_into)
            expansion_zone.expolygons = diff_ex(expansion_zone.expolygons, out);
    return out;
}

Surfaces expand_merge_surfaces(
    Surfaces &surfaces,
    SurfaceType surface_type,
    std::vector<ExpansionZone>& expansion_zones,
    const float closing_radius,
    const double bridge_angle
)
{
    using namespace Slic3r::Algorithm;

    double thickness;
    ExPolygons src = fill_surfaces_extract_expolygons(surfaces, {surface_type}, thickness);
    if (src.empty())
        return {};

    unsigned processed_expolygons_count = 0;
    std::vector<RegionExpansion> expansions;
    for (ExpansionZone& expansion_zone : expansion_zones) {
        std::vector<RegionExpansion> zone_expansions = propagate_waves(src, expansion_zone.expolygons, expansion_zone.parameters);
        expansion_zone.expanded_into = !zone_expansions.empty();

        for (RegionExpansion &expansion : zone_expansions)
            expansion.boundary_id += processed_expolygons_count;

        processed_expolygons_count += expansion_zone.expolygons.size();
        append(expansions, std::move(zone_expansions));
    }

    std::vector<ExPolygon> expanded = merge_expansions_into_expolygons(std::move(src), std::move(expansions));
    //NOTE: The current regularization of the shells can create small unasigned regions in the object (E.G. benchy)
    // without the following closing operation, those regions will stay unfilled and cause small holes in the expanded surface.
    // look for narrow_ensure_vertical_wall_thickness_region_radius filter.
    expanded = closing_ex(expanded, closing_radius);
    // Trim the zones by the expanded expolygons.
    for (ExpansionZone& expansion_zone : expansion_zones)
        if (expansion_zone.expanded_into)
            expansion_zone.expolygons = diff_ex(expansion_zone.expolygons, expanded);

    Surface templ{ surface_type, {} };
    templ.bridge_angle = bridge_angle;
    Surfaces out;
    out.reserve(expanded.size());
    for (auto &expoly : expanded)
        out.emplace_back(templ, std::move(expoly));
    return out;
}

void LayerRegion::process_external_surfaces(const Layer *lower_layer, const Polygons *lower_layer_covered)
{
    using namespace Slic3r::Algorithm;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // Width of the perimeters.
    float shell_width = 0;
    float expansion_min = 0;
    //Orio: wall count of this layer (painted with the "Variable infill and walls" tool, alternate extra wall).
    if (int num_perimeters = layer_wall_loops(*this->layer(), this->region().config()); num_perimeters > 0) {
        Flow external_perimeter_flow = this->flow(frExternalPerimeter);
        Flow perimeter_flow          = this->flow(frPerimeter);
        shell_width  = 0.5f * external_perimeter_flow.scaled_width() + external_perimeter_flow.scaled_spacing();
        shell_width += perimeter_flow.scaled_spacing() * (num_perimeters - 1);
        expansion_min = perimeter_flow.scaled_spacing();
    } else {
        // TODO: Maybe there is better solution when printing with zero perimeters, but this works reasonably well, given the situation
        shell_width   = float(SCALED_EPSILON);
        expansion_min = float(SCALED_EPSILON);;
    }

    // Scaled expansions of the respective external surfaces.
    float                           expansion_top           = shell_width * sqrt(2.);
    float                           expansion_bottom        = expansion_top;
    float                           expansion_bottom_bridge = expansion_top;
    // Expand by waves of expansion_step size (expansion_step is scaled), but with no more steps than max_nr_expansion_steps.
    static constexpr const float    expansion_step          = scaled<float>(0.1);
    // Don't take more than max_nr_steps for small expansion_step.
    static constexpr const size_t   max_nr_expansion_steps  = 5;
    // Radius (with added epsilon) to absorb empty regions emering from regularization of ensuring, viz  const float narrow_ensure_vertical_wall_thickness_region_radius = 0.5f * 0.65f * min_perimeter_infill_spacing;
    const float closing_radius = 0.55f * 0.65f * 1.05f * this->flow(frSolidInfill).scaled_spacing();

    // Expand the top / bottom / bridge surfaces into the shell thickness solid infills.
    double     layer_thickness;
    ExPolygons shells = union_ex(fill_surfaces_extract_expolygons(fill_surfaces.surfaces, { stInternalSolid }, layer_thickness));
    ExPolygons sparse = union_ex(fill_surfaces_extract_expolygons(fill_surfaces.surfaces, { stInternal }, layer_thickness));
    ExPolygons top_expolygons = union_ex(fill_surfaces_extract_expolygons(fill_surfaces.surfaces, { stTop }, layer_thickness));
    const auto expansion_params_into_sparse_infill = RegionExpansionParameters::build(expansion_min, expansion_step, max_nr_expansion_steps);
    const auto expansion_params_into_solid_infill  = RegionExpansionParameters::build(expansion_bottom_bridge, expansion_step, max_nr_expansion_steps);

    std::vector<ExpansionZone> expansion_zones{
        ExpansionZone{std::move(shells), expansion_params_into_solid_infill},
        ExpansionZone{std::move(sparse), expansion_params_into_sparse_infill},
        ExpansionZone{std::move(top_expolygons), expansion_params_into_solid_infill}
    };

    SurfaceCollection bridges;
    {
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->print_z;
        const double custom_angle = this->region().config().bridge_angle.value;
        bridges.surfaces = custom_angle > 0 ?
            expand_merge_surfaces(fill_surfaces.surfaces, stBottomBridge, expansion_zones, closing_radius, Geometry::deg2rad(custom_angle)) :
            expand_bridges_detect_orientations(fill_surfaces.surfaces, expansion_zones, closing_radius);
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
#if 0
        {
            static int iRun = 0;
            bridges.export_to_svg(debug_out_path("bridges-after-grouping-%d.svg", iRun++), true);
        }
#endif
    }

    fill_surfaces.remove_types({ stTop });
    {
        Surface top_templ(stTop, {});
        top_templ.thickness = layer_thickness;
        fill_surfaces.append(std::move(expansion_zones.back().expolygons), top_templ);
    }

    expansion_zones.pop_back();

    expansion_zones.at(0).parameters = RegionExpansionParameters::build(expansion_bottom, expansion_step, max_nr_expansion_steps);
    Surfaces bottoms = expand_merge_surfaces(fill_surfaces.surfaces, stBottom, expansion_zones, closing_radius);

    expansion_zones.at(0).parameters = RegionExpansionParameters::build(expansion_top, expansion_step, max_nr_expansion_steps);
    Surfaces tops = expand_merge_surfaces(fill_surfaces.surfaces, stTop, expansion_zones, closing_radius);

    //expansion_zone[0]: shell , expansion_zone[1]: sparse
    //apply minimu sparse infill area logic, this should also be added in bridge_over_infill
    if (!this->layer()->object()->print()->config().spiral_mode && infill_walls_density(*this->layer(), this->region().config()) > 0) {
        auto &sparse=expansion_zones[1].expolygons;
        auto &shells=expansion_zones[0].expolygons;
        double min_area = scale_(scale_(this->region().config().minimum_sparse_infill_area.value));
        ExPolygons areas_to_be_solid{};
        sparse.erase(std::remove_if(sparse.begin(), sparse.end(), [min_area, &areas_to_be_solid](ExPolygon& expoly) {
            if (expoly.area() <= min_area) {
                areas_to_be_solid.push_back(expoly);
                return true;
            }
            return false;
        }), sparse.end());

        if (!areas_to_be_solid.empty())
            shells = union_ex(shells, areas_to_be_solid);
    }


//    m_fill_surfaces.remove_types({ stBottomBridge, stBottom, stTop, stInternal, stInternalSolid });
    fill_surfaces.clear();
    unsigned zones_expolygons_count = 0;
    for (const ExpansionZone& zone : expansion_zones)
        zones_expolygons_count += zone.expolygons.size();
    reserve_more(fill_surfaces.surfaces, zones_expolygons_count + bridges.size() + bottoms.size() + tops.size());
    {
        Surface solid_templ(stInternalSolid, {});
        solid_templ.thickness = layer_thickness;
        fill_surfaces.append(std::move(expansion_zones[0].expolygons), solid_templ);
    }
    {
        Surface sparse_templ(stInternal, {});
        sparse_templ.thickness = layer_thickness;
        fill_surfaces.append(std::move(expansion_zones[1].expolygons), sparse_templ);
    }
    fill_surfaces.append(std::move(bridges.surfaces));
    fill_surfaces.append(std::move(bottoms));
    fill_surfaces.append(std::move(tops));

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}

#else

#endif

void LayerRegion::prepare_fill_surfaces()
{
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-initial");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    /*  Note: in order to make the psPrepareInfill step idempotent, we should never
        alter fill_surfaces boundaries on which our idempotency relies since that's
        the only meaningful information returned by psPerimeters. */

    bool spiral_mode = this->layer()->object()->print()->config().spiral_mode;

#if 0
    // if no solid layers are requested, turn top/bottom surfaces to internal
    if (! spiral_mode && this->region().config().top_shell_layers == 0) {
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.is_top())
                //BBS
                //surface.surface_type = this->layer()->object()->config().infill_only_where_needed ? stInternalVoid : stInternal;
                surface.surface_type = PrintObject::infill_only_where_needed ? stInternalVoid : stInternal;
    }
    if (this->region().config().bottom_shell_layers == 0) {
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.is_bottom()) // (surface.surface_type == stBottom)
                surface.surface_type = stInternal;
    }
#endif

    // turn too small internal regions into solid regions according to the user setting
    if (! spiral_mode && infill_walls_density(*this->layer(), this->region().config()) > 0) {
        // scaling an area requires two calls!
        double min_area = scale_(scale_(this->region().config().minimum_sparse_infill_area.value));
        //Orio: the area of the sparse infill of all the regions of the layer around it. The regions share the walls of
        // the object (Layer::make_perimeters()): a piece cut off by the boundary of a modifier (a strip along a wall) is a part
        // of a larger area, not a small one to fill solid.
        ExPolygons sparse;
        bool       sparse_done = false;
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.surface_type == stInternal && surface.area() <= min_area) {
                if (! sparse_done) {
                    for (const LayerRegion *layerm : this->layer()->regions())
                        for (const Surface &other : layerm->fill_surfaces.surfaces)
                            if (other.surface_type == stInternal)
                                sparse.emplace_back(other.expolygon);
                    sparse      = union_safety_offset_ex(sparse);
                    sparse_done = true;
                }
                double area = 0.;
                for (const ExPolygon &island : sparse)
                    if (island.overlaps(surface.expolygon))
                        area += island.area();
                if (area <= min_area)
                    surface.surface_type = stInternalSolid;
            }
    }

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-final");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}

double LayerRegion::infill_area_threshold() const
{
    double ss = this->flow(frSolidInfill).scaled_spacing();
    return ss*ss;
}

void LayerRegion::trim_surfaces(const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices.surfaces)
        assert(surface.surface_type == stInternal);
#endif /* NDEBUG */
	this->slices.set(intersection_ex(this->slices.surfaces, trimming_polygons), stInternal);
}

void LayerRegion::elephant_foot_compensation_step(const float elephant_foot_compensation_perimeter_step, const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices.surfaces)
        assert(surface.surface_type == stInternal);
#endif /* NDEBUG */
    Polygons tmp = intersection(this->slices.surfaces, trimming_polygons);
    append(tmp, diff(this->slices.surfaces, opening(this->slices.surfaces, elephant_foot_compensation_perimeter_step)));
    this->slices.set(union_ex(tmp), stInternal);
}

void LayerRegion::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = this->slices.surfaces.begin(); surface != this->slices.surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (Surfaces::const_iterator surface = this->slices.surfaces.begin(); surface != this->slices.surfaces.end(); ++surface)
        svg.draw(surface->expolygon, surface_type_to_color_name(surface->surface_type), transparency);
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        svg.draw(surface->expolygon.lines(), surface_type_to_color_name(surface->surface_type));
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_slices_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_slices_to_svg(debug_out_path("LayerRegion-slices-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const Surface &surface : this->fill_surfaces.surfaces) {
        svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
        svg.draw_outline(surface.expolygon, "black", "blue", scale_(0.05));
    }
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_fill_surfaces_to_svg(debug_out_path("LayerRegion-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::simplify_entity_collection(ExtrusionEntityCollection* entity_collection)
{
    for (size_t i = 0; i < entity_collection->entities.size(); i++) {
        if (ExtrusionEntityCollection* collection = dynamic_cast<ExtrusionEntityCollection*>(entity_collection->entities[i]))
            this->simplify_entity_collection(collection);
        else if (ExtrusionPath* path = dynamic_cast<ExtrusionPath*>(entity_collection->entities[i]))
            this->simplify_path(path);
        else if (ExtrusionMultiPath* multipath = dynamic_cast<ExtrusionMultiPath*>(entity_collection->entities[i]))
            this->simplify_multi_path(multipath);
        else if (ExtrusionLoop* loop = dynamic_cast<ExtrusionLoop*>(entity_collection->entities[i]))
            this->simplify_loop(loop);
        else
            throw Slic3r::InvalidArgument("Invalid extrusion entity supplied to simplify_entity_collection()");
    }
}

void LayerRegion::simplify_path(ExtrusionPath* path)
{
    const auto print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    //Orio: no arcs on tilted layers, whose G-code follows the points of the paths.
    const bool enable_arc_fitting = print_config.enable_arc_fitting && ! this->layer()->object()->tilted_params().enabled;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    if (enable_arc_fitting &&
        !spiral_mode) {
        if (path->role() == erInternalInfill)
            path->simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
        else
            path->simplify_by_fitting_arc(scaled_resolution);
    } else {
        path->simplify(scaled_resolution);
    }
}

void LayerRegion::simplify_multi_path(ExtrusionMultiPath* multipath)
{
    const auto print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    //Orio: no arcs on tilted layers, whose G-code follows the points of the paths.
    const bool enable_arc_fitting = print_config.enable_arc_fitting && ! this->layer()->object()->tilted_params().enabled;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < multipath->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            if (multipath->paths[i].role() == erInternalInfill)
                multipath->paths[i].simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
            else
                multipath->paths[i].simplify_by_fitting_arc(scaled_resolution);
        } else {
            multipath->paths[i].simplify(scaled_resolution);
        }
    }
}

void LayerRegion::simplify_loop(ExtrusionLoop* loop)
{
    const auto print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    //Orio: no arcs on tilted layers, whose G-code follows the points of the paths.
    const bool enable_arc_fitting = print_config.enable_arc_fitting && ! this->layer()->object()->tilted_params().enabled;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < loop->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            if (loop->paths[i].role() == erInternalInfill)
                loop->paths[i].simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
            else
                loop->paths[i].simplify_by_fitting_arc(scaled_resolution);
        } else {
            loop->paths[i].simplify(scaled_resolution);
        }
    }
}

}
