#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "Print.hpp"
#include "Fill/Fill.hpp"
#include "ShortestPath.hpp"
#include "SVG.hpp"
#include "BoundingBox.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include <boost/log/trivial.hpp>
#include "PerimeterGenerator.hpp"

#include <algorithm>
#include <limits>
#include "InfillWallsProfile.hpp"
static const int Continuitious_length = scale_(0.01);
static const int dist_scale_threshold = 1.2;

namespace Slic3r {

Layer::~Layer()
{
    this->lower_layer = this->upper_layer = nullptr;
    for (LayerRegion *region : m_regions)
        delete region;
    m_regions.clear();
}

// Test whether whether there are any slices assigned to this layer.
bool Layer::empty() const
{
	for (const LayerRegion *layerm : m_regions)
        if (layerm != nullptr && ! layerm->slices.empty())
            // Non empty layer.
            return false;
    return true;
}

LayerRegion* Layer::add_region(const PrintRegion *print_region)
{
    m_regions.emplace_back(new LayerRegion(this, print_region));
    return m_regions.back();
}
void Layer::apply_auto_circle_compensation()
{
    for (LayerRegion *layerm : m_regions) {
        layerm->auto_circle_compensation(layerm->slices, this->object()->get_auto_circle_compenstaion_params(), scale_(this->object()->config().circle_compensation_manual_offset));
    }
}

// merge all regions' slices to get islands
void Layer::make_slices()
{
    ExPolygons slices;
    if (m_regions.size() == 1) {
        // optimization: if we only have one region, take its slices
        slices = to_expolygons(m_regions.front()->slices.surfaces);
    } else {
        Polygons slices_p;
        for (LayerRegion *layerm : m_regions)
            polygons_append(slices_p, to_polygons(layerm->slices.surfaces));
        slices = union_safety_offset_ex(slices_p);
    }

    this->lslices.clear();
    this->lslices.reserve(slices.size());

    // prepare ordering points
    Points ordering_points;
    ordering_points.reserve(slices.size());
    for (const ExPolygon &ex : slices)
        ordering_points.push_back(ex.contour.first_point());

    // sort slices
    std::vector<Points::size_type> order = chain_points(ordering_points);

    // populate slices vector
    for (size_t i : order)
        this->lslices.emplace_back(std::move(slices[i]));
}

static inline bool layer_needs_raw_backup(const Layer *layer)
{
    // BBS: backup raw slice for generating support
    //return ! (layer->regions().size() == 1 && (layer->id() > 0 || layer->object()->config().elefant_foot_compensation.value == 0));
    return true;
}

void Layer::backup_untyped_slices()
{
    if (layer_needs_raw_backup(this)) {
        for (LayerRegion *layerm : m_regions) {
            layerm->raw_slices = to_expolygons(layerm->slices.surfaces);
            layerm->raw_counter_circle_compensation.clear();
            layerm->raw_holes_circle_compensation.clear();
            for (Surface &surface : layerm->slices.surfaces) {
                layerm->raw_counter_circle_compensation.push_back(surface.counter_circle_compensation);
                layerm->raw_holes_circle_compensation.push_back(surface.holes_circle_compensation);
            }
        }
    } else {
        assert(m_regions.size() == 1);
        m_regions.front()->raw_slices.clear();
        m_regions.front()->raw_counter_circle_compensation.clear();
        m_regions.front()->raw_holes_circle_compensation.clear();
    }
}

void Layer::restore_untyped_slices()
{
    if (layer_needs_raw_backup(this)) {
        for (LayerRegion *layerm : m_regions) {
            layerm->slices.set(layerm->raw_slices, stInternal);
            int surface_idx = 0;
            for (Surface &surface : layerm->slices.surfaces) {
                if (surface_idx < layerm->raw_counter_circle_compensation.size()
                    && surface_idx < layerm->raw_holes_circle_compensation.size()) {
                    surface.counter_circle_compensation = layerm->raw_counter_circle_compensation[surface_idx];
                    surface.holes_circle_compensation   = layerm->raw_holes_circle_compensation[surface_idx];
                }
            }
        }
    } else {
        assert(m_regions.size() == 1);
        m_regions.front()->slices.set(this->lslices, stInternal);
        if (0 < m_regions.front()->raw_counter_circle_compensation.size()
            && 0 < m_regions.front()->raw_holes_circle_compensation.size()
            && 0 < m_regions.front()->slices.surfaces.size()) {
            m_regions.front()->slices.surfaces.front().counter_circle_compensation = m_regions.front()->raw_counter_circle_compensation.front();
            m_regions.front()->slices.surfaces.front().holes_circle_compensation   = m_regions.front()->raw_holes_circle_compensation.front();
        }
    }
}

// Similar to Layer::restore_untyped_slices()
// To improve robustness of detect_surfaces_type() when reslicing (working with typed slices), see GH issue #7442.
// Only resetting layerm->slices if Slice::extra_perimeters is always zero or it will not be used anymore
// after the perimeter generator.
void Layer::restore_untyped_slices_no_extra_perimeters()
{
    if (layer_needs_raw_backup(this)) {
        for (LayerRegion *layerm : m_regions)
            //BBS: remove extra_perimeters. Always false
        	//if (! layerm->region().config().extra_perimeters.value)
            	layerm->slices.set(layerm->raw_slices, stInternal);
    } else {
    	assert(m_regions.size() == 1);
    	LayerRegion *layerm = m_regions.front();
    	// This optimization is correct, as extra_perimeters are only reused by prepare_infill() with multi-regions.
        //if (! layerm->region().config().extra_perimeters.value)
        	layerm->slices.set(this->lslices, stInternal);
    }
}

ExPolygons Layer::merged(float offset_scaled) const
{
	assert(offset_scaled >= 0.f);
    // If no offset is set, apply EPSILON offset before union, and revert it afterwards.
	float offset_scaled2 = 0;
	if (offset_scaled == 0.f) {
		offset_scaled  = float(  EPSILON);
		offset_scaled2 = float(- EPSILON);
    }
    Polygons polygons;
	for (LayerRegion *layerm : m_regions) {
		const PrintRegionConfig &config = layerm->region().config();
		// Our users learned to bend Slic3r to produce empty volumes to act as subtracters. Only add the region if it is non-empty.
		if (config.bottom_shell_layers > 0 || config.top_shell_layers > 0 || config.sparse_infill_density > 0. || config.wall_loops > 0)
			append(polygons, offset(layerm->slices.surfaces, offset_scaled));
	}
    ExPolygons out = union_ex(polygons);
	if (offset_scaled2 != 0.f)
		out = offset_ex(out, offset_scaled2);
    return out;
}

//Orio: wave overhangs (Unlayered). True when two regions, one of them at least with wave overhangs, would not make
// the same waves: the waves replace parts of the walls, so these regions cannot share their walls (Layer::make_perimeters()).
// Not the wave speeds, support_remaining_areas_after_wave_overhangs nor wave_overhang_floor_layers (read per region by
// PrintObject::prepare_infill, it does not change the walls and only invalidates posPrepareInfill), applied elsewhere.
static bool other_wave_overhangs(const PrintRegionConfig &config, const PrintRegionConfig &other)
{
    if (! config.wave_overhangs.value && ! other.wave_overhangs.value)
        return false;
    return config.wave_overhangs.value                    != other.wave_overhangs.value
        || config.wave_overhangs_instead_of_bridges.value != other.wave_overhangs_instead_of_bridges.value
        || config.wave_overhang_min_length.value          != other.wave_overhang_min_length.value
        || config.wave_overhang_max_iterations.value      != other.wave_overhang_max_iterations.value
        || config.wave_overhang_pattern.value             != other.wave_overhang_pattern.value
        || config.wave_overhang_shape.value               != other.wave_overhang_shape.value
        || config.wave_overhang_arc_max_radius.value      != other.wave_overhang_arc_max_radius.value
        || config.wave_overhang_outer_perimeters.value    != other.wave_overhang_outer_perimeters.value
        || config.wave_overhang_line_spacing.value        != other.wave_overhang_line_spacing.value
        || config.wave_overhang_perimeter_overlap.value   != other.wave_overhang_perimeter_overlap.value
        || config.wave_overhang_minimum_width.value       != other.wave_overhang_minimum_width.value
        || config.wave_overhang_min_new_area.value        != other.wave_overhang_min_new_area.value
        || config.wave_overhang_flow_mm3_per_mm.value     != other.wave_overhang_flow_mm3_per_mm.value;
}

// If there is any incompatibility, separate LayerRegions have to be created.
bool Layer::has_compatible_layer_regions(const PrintRegionConfig &config, const PrintRegionConfig &other_config)
{
    //Orio: OrcaSlicer's feature filaments: outer and inner walls.
    return config.outer_wall_filament_id == other_config.outer_wall_filament_id
           && config.inner_wall_filament_id == other_config.inner_wall_filament_id
           && config.wall_loops == other_config.wall_loops
           && config.wall_sequence == other_config.wall_sequence
           && config.inner_wall_speed.get_at(get_process_config_idx(config.outer_wall_filament_id)) == other_config.inner_wall_speed.get_at(get_process_config_idx(config.outer_wall_filament_id))
           && config.outer_wall_speed.get_at(get_process_config_idx(config.outer_wall_filament_id)) == other_config.outer_wall_speed.get_at(get_process_config_idx(config.outer_wall_filament_id))
           && config.gap_infill_speed.get_at(get_process_config_idx(config.outer_wall_filament_id)) == other_config.gap_infill_speed.get_at(get_process_config_idx(config.outer_wall_filament_id))
           && config.detect_overhang_wall == other_config.detect_overhang_wall
           && config.filter_out_gap_fill.value == other_config.filter_out_gap_fill.value
           && config.opt_serialize("inner_wall_line_width") == other_config.opt_serialize("inner_wall_line_width")
           && config.opt_serialize("outer_wall_line_width") == other_config.opt_serialize("outer_wall_line_width")
           && config.detect_thin_wall == other_config.detect_thin_wall
           && config.infill_wall_overlap == other_config.infill_wall_overlap
           && config.override_filament_scarf_seam_setting == other_config.override_filament_scarf_seam_setting
           && config.seam_slope_type == other_config.seam_slope_type
           && config.seam_slope_start_height == other_config.seam_slope_start_height
           && config.seam_slope_gap == other_config.seam_slope_gap
           && config.seam_slope_min_length == other_config.seam_slope_min_length
           && config.seam_slope_conditional == other_config.seam_slope_conditional
           && config.seam_slope_entire_loop == other_config.seam_slope_entire_loop
           && config.seam_slope_steps == other_config.seam_slope_steps
           && config.seam_slope_inner_walls == other_config.seam_slope_inner_walls
           //Orio: walls of regions with different brick layers settings must stay separate.
           && config.brick_layers == other_config.brick_layers
           && config.brick_layers_flow == other_config.brick_layers_flow
           //Orio: nor can regions with and without the alternate extra wall share their walls.
           && config.alternate_extra_wall == other_config.alternate_extra_wall
           //Orio: nor regions making other wave overhangs.
           && ! other_wave_overhangs(config, other_config)
           //Orio: not the flow ratios by line type, the walls shared by regions with other ones get them by parts
           // (share_region_walls()).
           && (this->id() != 0 || config.initial_layer_flow_ratio == other_config.initial_layer_flow_ratio);
}

//Orio: two regions printing their walls with other filaments keep their own walls, as usual (parts of other colours,
// painted colours), unless they are of the same part: a modifier shares its walls with the part it modifies (see
// Layer::make_perimeters()).
static bool own_walls_with_other_filaments(const Layer &layer, const PrintRegion &region, const PrintRegion &other)
{
    const PrintRegionConfig &config       = region.config();
    const PrintRegionConfig &other_config = other.config();
    if (config.outer_wall_filament_id.value == other_config.outer_wall_filament_id.value &&
        config.inner_wall_filament_id.value == other_config.inner_wall_filament_id.value)
        return false;
    const PrintObjectRegions *regions = layer.object()->shared_regions();
    if (regions == nullptr)
        return true;
    // The model parts a region is made of, a modifier belonging to the part it modifies (none for a painted colour).
    auto parts_of = [regions](const PrintRegion &print_region) {
        std::vector<ObjectID> parts;
        for (const PrintObjectRegions::LayerRangeRegions &range : regions->layer_ranges)
            for (const PrintObjectRegions::VolumeRegion &volume_region : range.volume_regions)
                if (volume_region.region == &print_region) {
                    const PrintObjectRegions::VolumeRegion *part = &volume_region;
                    while (part->parent >= 0)
                        part = &range.volume_regions[size_t(part->parent)];
                    parts.emplace_back(part->model_volume->id());
                }
        return parts;
    };
    const std::vector<ObjectID> parts = parts_of(region);
    for (const ObjectID &part : parts_of(other))
        if (std::find(parts.begin(), parts.end(), part) != parts.end())
            return false;
    return true;
}

//Orio: whether the G-code prints a part of a shared wall lying in the other region with other settings than the region
// holding it: they differ in something else than the fuzzy skin (applied with the walls, see PerimeterRegion).
static bool other_wall_settings(const PrintRegionConfig &config, const PrintRegionConfig &other)
{
    for (const t_config_option_key &key : config.diff(other))
        if (key.rfind("fuzzy_skin", 0) != 0)
            return true;
    return false;
}

//Orio: the walls shared by several regions are generated around their union and held by the first one (see
// Layer::make_perimeters()). Their parts lying in another region get its settings: the G-code applies them to these parts
// (ExtrusionPath::config_region: speeds, accelerations, jerk, flow ratios...), or that region prints them itself when it prints
// its walls with other filaments or in brick layers (they are moved to its walls). The walls stay whole across the boundary of a
// modifier, whatever it changes.
class SharedWallParts
{
public:
    SharedWallParts(const LayerRegion &holder, const LayerRegionPtrs &layerms)
    {
        const PrintRegionConfig &config = holder.region().config();
        Lines                    edges;
        for (LayerRegion *layerm : layerms) {
            if (layerm == &holder)
                continue;
            const PrintRegionConfig &other = layerm->region().config();
            const bool own = config.outer_wall_filament_id.value != other.outer_wall_filament_id.value ||
                             config.inner_wall_filament_id.value != other.inner_wall_filament_id.value ||
                             config.brick_layers.value != other.brick_layers.value ||
                             (other.brick_layers.value && config.brick_layers_flow.value != other.brick_layers_flow.value);
            if (! own && ! other_wall_settings(config, other))
                // Its parts are printed as those of the holder.
                continue;
            m_regions.emplace_back(layerm);
            m_own.emplace_back(own);
            m_areas.emplace_back(to_expolygons(layerm->slices.surfaces));
            m_area_bboxes.emplace_back();
            for (const ExPolygon &expoly : m_areas.back())
                m_area_bboxes.back().emplace_back(get_extents(expoly));
            append(edges, to_lines(m_areas.back()));
            m_bbox.merge(get_extents(m_areas.back()));
        }
        if (! edges.empty())
            m_boundary = AABBTreeLines::LinesDistancer<Line>(std::move(edges));
    }

    size_t       size() const { return m_regions.size(); }
    LayerRegion *region(size_t i) const { return m_regions[i]; }

    // In place: the extrusions of the holder cut into their pieces in each region and tagged with it, the parts printed by
    // another region added to moved[that region]. The collections are kept (those of the perimeter generator are indexed by the
    // loop nodes of the layer, see Layer::recrod_cooling_node_for_each_extrusion()), the emptied ones removed.
    void apply(ExtrusionEntityCollection &collection, std::vector<ExtrusionEntitiesPtr> &moved) const
    {
        ExtrusionEntitiesPtr out;
        out.reserve(collection.entities.size());
        for (ExtrusionEntity *entity : collection.entities) {
            // -1: kept, >= 0: moved whole to this region, -2: replaced by its pieces.
            int owner = -1;
            if (auto *child = dynamic_cast<ExtrusionEntityCollection*>(entity)) {
                this->apply(*child, moved);
                owner = child->entities.empty() ? -2 : -1;
            } else if (auto *loop = dynamic_cast<ExtrusionLoop*>(entity))
                owner = this->apply(loop->paths, true, out, moved);
            else if (auto *multi_path = dynamic_cast<ExtrusionMultiPath*>(entity))
                owner = this->apply(multi_path->paths, false, out, moved);
            else if (auto *path = dynamic_cast<ExtrusionPath*>(entity)) {
                ExtrusionPaths paths { std::move(*path) };
                owner = this->apply(paths, false, out, moved);
                if (owner >= -1) {
                    if (paths.size() == 1)
                        *path = std::move(paths.front());
                    else {
                        // Its pieces follow each other: one multi path.
                        delete entity;
                        auto *pieces  = new ExtrusionMultiPath();
                        pieces->paths = std::move(paths);
                        entity        = pieces;
                    }
                }
            }
            if (owner == -1)
                out.emplace_back(entity);
            else if (owner >= 0)
                moved[owner].emplace_back(entity);
            else
                delete entity;
        }
        collection.entities = std::move(out);
    }

private:
    int area_of(const Point &pt) const
    {
        for (size_t i = 0; i < m_areas.size(); ++ i)
            for (size_t j = 0; j < m_areas[i].size(); ++ j)
                if (m_area_bboxes[i][j].contains(pt) && m_areas[i][j].contains(pt))
                    return int(i);
        return -1;
    }

    // The pieces of the polyline in order along it, cut where it crosses the boundary of a region, each with the index of its
    // region (-1: the holder).
    std::vector<std::pair<Polyline, int>> split(const Polyline &polyline) const
    {
        std::vector<std::pair<Polyline, int>> pieces;
        // The region of the piece being built: known from its first segment on, until the polyline crosses a boundary.
        int  area  = -1;
        bool known = false;
        auto add   = [this, &pieces, &area, &known](const Point &from, const Point &to) {
            if (! known) {
                area  = this->area_of(Point((from.x() + to.x()) / 2, (from.y() + to.y()) / 2));
                known = true;
            }
            if (pieces.empty() || pieces.back().second != area) {
                pieces.emplace_back(Polyline(), area);
                pieces.back().first.points = { from, to };
            } else
                pieces.back().first.points.emplace_back(to);
        };
        for (size_t i = 0; i + 1 < polyline.points.size(); ++ i) {
            const Point &b    = polyline.points[i + 1];
            Point        from = polyline.points[i];
            for (const auto &crossing : m_boundary.intersections_with_line<true>(Line(from, b))) {
                const Point to(crossing.first);
                // A piece shorter than that is merged into the next one.
                if ((to - from).cast<double>().norm() >= SCALED_EPSILON) {
                    add(from, to);
                    from = to;
                }
                // Beyond it, another region.
                known = false;
            }
            if (from != b)
                add(from, b);
        }
        return pieces;
    }

    // Printed by their region: with its settings already.
    void untag(ExtrusionPaths &paths, int owner) const
    {
        const int region_id = m_regions[owner]->region().print_region_id();
        for (ExtrusionPath &path : paths)
            if (path.config_region == region_id)
                path.config_region = -1;
    }

    // Consecutive paths (a loop when closed) cut into their pieces, those lying in another region tagged with it. Returns -1 when
    // the holder prints them all, the index of the region printing them all otherwise (paths: their pieces in both cases); else
    // -2, their runs of consecutive pieces printed by the same region added to kept (the holder's) or to moved.
    int apply(ExtrusionPaths &paths, bool closed, ExtrusionEntitiesPtr &kept, std::vector<ExtrusionEntitiesPtr> &moved) const
    {
        ExtrusionPaths   pieces;
        std::vector<int> owners; // the region printing each piece, -1: the holder
        for (ExtrusionPath &path : paths) {
            std::vector<std::pair<Polyline, int>> cut;
            if (get_extents(path.polyline).overlap(m_bbox))
                cut = this->split(path.polyline);
            if (cut.empty()) {
                pieces.emplace_back(std::move(path));
                owners.emplace_back(-1);
                continue;
            }
            for (std::pair<Polyline, int> &piece : cut) {
                pieces.emplace_back(std::move(piece.first), path);
                if (piece.second >= 0)
                    pieces.back().config_region = m_regions[piece.second]->region().print_region_id();
                owners.emplace_back(piece.second >= 0 && m_own[piece.second] ? piece.second : -1);
            }
        }
        if (pieces.empty())
            return -1;
        // A run of pieces of one region shorter than their width (the corner of a region) goes with the run before it (the
        // first one with the run after it) rather than being printed on its own.
        for (size_t i = 0; i < pieces.size();) {
            size_t j      = i + 1;
            double length = pieces[i].polyline.length();
            while (j < pieces.size() && owners[j] == owners[i])
                length += pieces[j ++].polyline.length();
            if (length < scale_(pieces[i].width) && (i > 0 || j < pieces.size()))
                std::fill(owners.begin() + i, owners.begin() + j, i > 0 ? owners[i - 1] : owners[j]);
            i = j;
        }
        if (std::all_of(owners.begin(), owners.end(), [&owners](int owner) { return owner == owners.front(); })) {
            paths = std::move(pieces);
            if (owners.front() >= 0)
                this->untag(paths, owners.front());
            return owners.front();
        }
        std::vector<ExtrusionPaths> runs;
        std::vector<int>            run_owners;
        for (size_t i = 0; i < pieces.size(); ++ i) {
            if (run_owners.empty() || run_owners.back() != owners[i]) {
                runs.emplace_back();
                run_owners.emplace_back(owners[i]);
            }
            runs.back().emplace_back(std::move(pieces[i]));
        }
        if (closed && run_owners.front() == run_owners.back()) {
            // The run through the start of the loop: its end goes first.
            ExtrusionPaths &first = runs.front();
            first.insert(first.begin(), std::make_move_iterator(runs.back().begin()), std::make_move_iterator(runs.back().end()));
            runs.pop_back();
            run_owners.pop_back();
        }
        for (size_t r = 0; r < runs.size(); ++ r) {
            auto *run  = new ExtrusionMultiPath();
            run->paths = std::move(runs[r]);
            if (run_owners[r] < 0)
                kept.emplace_back(run);
            else {
                this->untag(run->paths, run_owners[r]);
                moved[run_owners[r]].emplace_back(run);
            }
        }
        return -2;
    }

    std::vector<LayerRegion*>             m_regions; // the other regions printing their parts with their own settings
    std::vector<char>                     m_own;     // they print their parts themselves
    std::vector<ExPolygons>               m_areas;
    std::vector<BoundingBoxes>            m_area_bboxes;
    BoundingBox                           m_bbox;
    AABBTreeLines::LinesDistancer<Line>   m_boundary;
};

static void share_region_walls(LayerRegion &holder, const LayerRegionPtrs &layerms)
{
    const SharedWallParts parts(holder, layerms);
    if (parts.size() == 0)
        return;
    std::vector<ExtrusionEntitiesPtr> moved(parts.size());
    parts.apply(holder.perimeters, moved);
    for (size_t i = 0; i < moved.size(); ++ i)
        if (! moved[i].empty()) {
            // In a collection of their own, as the walls of an island (see GCode::process_layer()).
            auto *walls     = new ExtrusionEntityCollection();
            walls->entities = std::move(moved[i]);
            parts.region(i)->perimeters.entities.emplace_back(walls);
            moved[i].clear();
        }
    parts.apply(holder.thin_fills, moved);
    for (size_t i = 0; i < moved.size(); ++ i)
        for (ExtrusionEntity *entity : moved[i])
            parts.region(i)->thin_fills.entities.emplace_back(entity);
}

//Orio: "Alternate extra wall" (ported from OrcaSlicer). Every other layer gets one more wall, so that the
// infill of the layers in between is wedged between the extra walls of the layers above and below. Layers are counted
// from the first layer of the object (raft excluded), which never gets the extra wall. No extra wall on a layer without
// walls (0 walls set or painted) nor in spiral vase mode.
int layer_wall_loops(const Layer &layer, const PrintRegionConfig &region_config)
{
    const int          wall_loops = infill_walls_wall_loops(layer, region_config);
    const PrintObject &object     = *layer.object();
    const size_t       raft       = object.slicing_parameters().raft_layers();
    const size_t       index      = layer.id() >= raft ? layer.id() - raft : 0;
    if (region_config.alternate_extra_wall.value && wall_loops > 0 && index % 2 == 1 && !object.print()->config().spiral_mode.value)
        return wall_loops + 1;
    return wall_loops;
}

// Here the perimeters are created cummulatively for all layer regions sharing the same parameters influencing the perimeters.
// The perimeter paths and the thin fills (ExtrusionEntityCollection) are assigned to the first compatible layer region.
// The resulting fill surface is split back among the originating regions.
void Layer::make_perimeters()
{
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id();
    //Orio: wave overhangs, filled again by LayerRegion::make_perimeters().
    this->wave_overhang_floor_polygons.clear();
    this->wave_overhang_covered_polygons.clear();
    // keep track of regions whose perimeters we have already generated
    std::vector<unsigned char> done(m_regions.size(), false);

    for (LayerRegionPtrs::iterator layerm = m_regions.begin(); layerm != m_regions.end(); ++ layerm)
    	if ((*layerm)->slices.empty()) {
 			(*layerm)->perimeters.clear();
 			(*layerm)->fills.clear();
 			(*layerm)->thin_fills.clear();
    	} else {
	        size_t region_id = layerm - m_regions.begin();
	        if (done[region_id])
	            continue;
	        BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id() << ", region " << region_id;
	        done[region_id] = true;
	        const PrintRegionConfig &config = (*layerm)->region().config();

	        // find compatible regions
            LayerRegionPtrs layerms;
            layerms.push_back(*layerm);

            PerimeterRegions perimeter_regions;
            for (LayerRegionPtrs::const_iterator it = layerm + 1; it != m_regions.end(); ++it) {
                if ((*it)->slices.empty())
                    continue;

                LayerRegion* other_layerm = *it;
                const size_t next_region_id = std::distance(m_regions.cbegin(), it);
                const PrintRegionConfig &other_config = other_layerm->region().config();
                //Orio: every region shares the walls of the object (modifier, height range), whatever its settings, else
                // the walls would be cut at the boundary of the modifier and closed along it, inside the part, weakening it: the
                // walls common to all the regions are generated around their union, the regions with more walls add theirs inside
                // (see below), and the parts of these walls in a region get its settings (share_region_walls()). Only the parts
                // and painted colours of other filaments keep their own walls, as usual, and the parts of a gradient (below).
                if (own_walls_with_other_filaments(*this, (*layerm)->region(), other_layerm->region()))
                    continue;
                if ((*layerm)->region().gradient_volume_id() != other_layerm->region().gradient_volume_id())
                    continue;
                //Orio: nor the regions making other wave overhangs: the waves replace parts of the walls, made with the
                // settings of the first region only.
                if (other_wave_overhangs(config, other_config))
                    continue;

                other_layerm->perimeters.clear();
                other_layerm->fills.clear();
                other_layerm->thin_fills.clear();
                // layer_region_ids.push_back(next_region_id);
                layerms.push_back(other_layerm);
                done[it - m_regions.begin()] = true;

                if (!PerimeterRegion::has_compatible_perimeter_regions(config, other_config)) {
                    perimeter_regions.emplace_back(*other_layerm);
                }
            }

	        if (layerms.size() == 1) {  // optimization
	            (*layerm)->fill_surfaces.surfaces.clear();
                (*layerm)->fill_no_overlap_expolygons.clear();
                (*layerm)->make_perimeters((*layerm)->slices, perimeter_regions, &(*layerm)->fill_surfaces, &(*layerm)->fill_no_overlap_expolygons, this->loop_nodes);

	            (*layerm)->fill_expolygons = to_expolygons((*layerm)->fill_surfaces.surfaces);
                //Orio: walls painted along X / Y, the zones painted with more walls get theirs inside.
                infill_walls_make_zone_walls(**layerm, (*layerm)->fill_no_overlap_expolygons, this->loop_nodes);
	        } else {
	            SurfaceCollection new_slices;
	            // Use the region with highest infill rate, as the make_perimeters() function below decides on the gap fill based on the infill existence.
	            LayerRegion *layerm_config = layerms.front();
	            {
	                // group slices (surfaces) according to number of extra perimeters
	                std::map<unsigned short, Surfaces> slices;  // extra_perimeters => [ surface, surface... ]
	                for (LayerRegion *layerm : layerms) {
	                    for (const Surface &surface : layerm->slices.surfaces)
	                        slices[surface.extra_perimeters].emplace_back(surface);
	                }
	                // merge the surfaces assigned to each group
	                for (std::pair<const unsigned short,Surfaces> &surfaces_with_extra_perimeters : slices)
	                    new_slices.append(offset_ex(surfaces_with_extra_perimeters.second, ClipperSafetyOffset), surfaces_with_extra_perimeters.second.front());
	            }

                // Try to merge compatible PerimeterRegions.
                if (perimeter_regions.size() > 1) {
                    PerimeterRegion::merge_compatible_perimeter_regions(perimeter_regions);
                }

	            // make perimeters
	            SurfaceCollection fill_surfaces;
                //BBS
                ExPolygons fill_no_overlap;
                //Orio: the walls common to all these regions.
                int common_wall_loops = std::numeric_limits<int>::max();
                for (const LayerRegion *l : layerms)
                    common_wall_loops = std::min(common_wall_loops, layer_wall_loops(*this, l->region().config()));
                (*layerm)->make_perimeters(new_slices, perimeter_regions, &fill_surfaces, &fill_no_overlap, this->loop_nodes, common_wall_loops);
                //Orio: the parts of these walls in the other regions get their settings.
                share_region_walls(**layerm, layerms);

	            // assign fill_surfaces to each layer
	            if (!fill_surfaces.surfaces.empty()) {
	                for (LayerRegionPtrs::iterator l = layerms.begin(); l != layerms.end(); ++l) {
	                    // Separate the fill surfaces.
	                    ExPolygons expp = intersection_ex(fill_surfaces.surfaces, (*l)->slices.surfaces);
	                    (*l)->fill_expolygons = expp;
	                    (*l)->fill_surfaces.set(std::move(expp), fill_surfaces.surfaces.front());
                        //BBS: Separate fill_no_overlap
                        (*l)->fill_no_overlap_expolygons = intersection_ex((*l)->slices.surfaces, fill_no_overlap);
	                }
                    //Orio: then the walls of the regions with more walls, inside the common ones. Generated over the whole area
                    // left to the infill by the common walls, then kept only in their region: they follow the contour of the object
                    // and stop at the boundary of the modifier, rather than closing along it across the part.
                    this->shared_walls_fill_area = std::move(fill_no_overlap);
                    for (LayerRegion *l : layerms)
                        if (const int extra_wall_loops = layer_wall_loops(*this, l->region().config()) - common_wall_loops; extra_wall_loops > 0)
                            l->make_extra_perimeters(extra_wall_loops, common_wall_loops, this->loop_nodes);
                    //Orio: walls painted along X / Y, the zones painted with more walls get theirs inside. Generated over
                    // the area left to the infill around all these regions, then kept in each region: they follow the contour of
                    // the object across the boundaries of the modifiers, rather than along them.
                    if (infill_walls_has_zone_walls(*this)) {
                        ExPolygons zone_walls_area;
                        for (const LayerRegion *l : layerms)
                            append(zone_walls_area, l->fill_no_overlap_expolygons);
                        zone_walls_area = union_safety_offset_ex(zone_walls_area);
                        for (LayerRegion *l : layerms)
                            infill_walls_make_zone_walls(*l, zone_walls_area, this->loop_nodes);
                    }
	            }
	        }
	    }

    //Orio: only needed while making the walls.
    this->shared_walls_fill_area.clear();
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id() << " - Done";
}

//BBS: use aabbtree to get distance
class ContinuitiousDistancer
{
    std::vector<Linef>                lines;
    AABBTreeIndirect::Tree<2, double> tree;

public:
    ContinuitiousDistancer(const Points &pts)
    {
        Lines pt_to_lines = to_lines(pts);
        for (const auto &line : pt_to_lines)
            lines.emplace_back(line.a.cast<double>(), line.b.cast<double>());

        tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);
    }

    float distance_from_perimeter(const Vec2f &point) const
    {
        Vec2d  p = point.cast<double>();
        size_t hit_idx_out{};
        Vec2d  hit_point_out = Vec2d::Zero();
        auto   distance      = AABBTreeLines::squared_distance_to_indexed_lines(lines, tree, p, hit_idx_out, hit_point_out);
        if (distance < 0) {
            return std::numeric_limits<float>::max();
        }

        distance          = sqrt(distance);
        const Linef &line = lines[hit_idx_out];
        Vec2d        v1   = line.b - line.a;
        Vec2d        v2   = p - line.a;
        if ((v1.x() * v2.y()) - (v1.y() * v2.x()) > 0.0) { distance *= -1; }
        return distance;
    }

    Lines to_lines(const Points &pts)
    {
        Lines lines;
        if (pts.size() >= 2) {
            lines.reserve(pts.size() - 1);
            for (Points::const_iterator it = pts.begin(); it != pts.end() - 1; ++it) { lines.push_back(Line(*it, *(it + 1))); }
        }
        return lines;
    }
};


static double get_node_continuity_rang_limit(const std::vector<coord_t> &Prev_node_widths, int prev_pt_idx) {
    double width = 0;
    double prev_width = Prev_node_widths.front();
    if (prev_pt_idx!=0 && prev_pt_idx < Prev_node_widths.size()) {
            prev_width = static_cast<double>(Prev_node_widths[prev_pt_idx]);
    }

    width = prev_width * dist_scale_threshold;
    return width;
}

void Layer::calculate_perimeter_continuity(std::vector<LoopNode> &prev_nodes) {
    for (size_t node_pos = 0; node_pos < loop_nodes.size(); ++node_pos) {
        LoopNode &node=loop_nodes[node_pos];
        double width = 0;
        ContinuitiousDistancer node_distancer(node.node_contour.pts);
        for (size_t prev_pos = 0; prev_pos < prev_nodes.size(); ++prev_pos) {
            LoopNode &prev_node = prev_nodes[prev_pos];

            // no overlap or has diff speed
            if (!node.bbox.overlap(prev_node.bbox))
                continue;

            //calculate dist, checkout the continuity
            Polyline continuitious_pl;
            //check start pt
            size_t start = 0;
            bool conntiouitious_flag = false;
            int end = prev_node.node_contour.pts.size() - 1;

            //if the countor is loop
            if (prev_node.node_contour.is_loop) {
                for (; end >= 0; --end) {
                    if (continuitious_pl.length() >= Continuitious_length) {
                        node.lower_node_id.push_back(prev_node.node_id);
                        prev_node.upper_node_id.push_back(node.node_id);
                        conntiouitious_flag = true;
                        break;
                    }

                    Point pt   = prev_node.node_contour.pts[end];
                    float dist = node_distancer.distance_from_perimeter(pt.cast<float>());
                    // get corr width
                    width = get_node_continuity_rang_limit(prev_node.node_contour.widths, end);

                    if (dist < width && dist > -width)
                        continuitious_pl.append_before(pt);
                    else
                        break;
                }

                if (conntiouitious_flag || end < 0)
                    continue;
            }

            int last_pt_idx = end;
            // line need to check end point
            if (!prev_node.node_contour.is_loop)
                last_pt_idx ++;

            for (; start < last_pt_idx; ++start) {
                Point pt   = prev_node.node_contour.pts[start];
                float dist = node_distancer.distance_from_perimeter(pt.cast<float>());
                //get corr width
                width = get_node_continuity_rang_limit(prev_node.node_contour.widths, start);

                if (dist < width && dist > -width) {
                    continuitious_pl.append(pt);
                    continue;
                }

                if (continuitious_pl.empty() || continuitious_pl.length() < Continuitious_length) {
                    continuitious_pl.clear();
                    continue;
                }

                node.lower_node_id.push_back(prev_node.node_id);
                prev_node.upper_node_id.push_back(node.node_id);
                continuitious_pl.clear();
                break;

            }

            if (continuitious_pl.length() >= Continuitious_length) {
                node.lower_node_id.push_back(prev_node.node_id);
                prev_node.upper_node_id.push_back(node.node_id);
            }
        }
    }

}

void Layer::recrod_cooling_node_for_each_extrusion() {
    for (LayerRegion *region : this->regions()) {
        for (int extrusion_idx = 0; extrusion_idx < region->perimeters.entities.size(); extrusion_idx++) {
            const auto *extrusions = static_cast<const ExtrusionEntityCollection *>(region->perimeters.entities[extrusion_idx]);
            int         start      = extrusions->loop_node_range.first;
            int         end        = extrusions->loop_node_range.second;
            if (start >= end)
                continue;

            int cooling_node = this->loop_nodes[start].merged_id;
            int pos          = this->loop_nodes[start].loop_id;
            int next_pos     = start + 1 < end ? this->loop_nodes[start + 1].loop_id : -1;
            for (int idx = 0; idx < extrusions->entities.size(); idx++) {
                // Orio: several nodes may share an index (wave overhangs deleted the loop of a node,
                // apply_wave_overhangs()): the last one is the entity's.
                while (idx == next_pos && next_pos > 0) {
                    start++;
                    cooling_node = this->loop_nodes[start].merged_id;
                    next_pos     = start + 1 < end ? this->loop_nodes[start + 1].loop_id : -1;
                }

                extrusions->entities[idx]->set_cooling_node(cooling_node);
            }

        }
    }
}

void Layer::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const auto *region : m_regions)
        for (const auto &surface : region->slices.surfaces)
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const auto *region : m_regions)
        for (const auto &surface : region->slices.surfaces)
            svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void Layer::export_region_slices_to_svg_debug(const char *name) const
{
    static size_t idx = 0;
    this->export_region_slices_to_svg(debug_out_path("Layer-slices-%s-%d.svg", name, idx ++).c_str());
}

void Layer::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const auto *region : m_regions)
        for (const auto &surface : region->slices.surfaces)
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const auto *region : m_regions)
        for (const auto &surface : region->slices.surfaces)
            svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

//BBS: method to simplify support path
void Layer::simplify_support_entity_collection(ExtrusionEntityCollection* entity_collection)
{
    for (size_t i = 0; i < entity_collection->entities.size(); i++) {
        if (ExtrusionEntityCollection* collection = dynamic_cast<ExtrusionEntityCollection*>(entity_collection->entities[i]))
            this->simplify_support_entity_collection(collection);
        else if (ExtrusionPath* path = dynamic_cast<ExtrusionPath*>(entity_collection->entities[i]))
            this->simplify_support_path(path);
        else if (ExtrusionMultiPath* multipath = dynamic_cast<ExtrusionMultiPath*>(entity_collection->entities[i]))
            this->simplify_support_multi_path(multipath);
        else if (ExtrusionLoop* loop = dynamic_cast<ExtrusionLoop*>(entity_collection->entities[i]))
            this->simplify_support_loop(loop);
        else
            throw Slic3r::InvalidArgument("Invalid extrusion entity supplied to simplify_support_entity_collection()");
    }
}
//BBS: method to simplify support path
void Layer::simplify_support_path(ExtrusionPath * path)
{
    const auto print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    //Orio: no arcs on tilted layers, whose G-code follows the points of the paths.
    const bool enable_arc_fitting = print_config.enable_arc_fitting && ! this->object()->tilted_params().enabled;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    if (enable_arc_fitting &&
        !spiral_mode) {
        path->simplify_by_fitting_arc(SCALED_SUPPORT_RESOLUTION);
    } else {
        path->simplify(scaled_resolution);
    }
}
//BBS: method to simplify support path
void Layer::simplify_support_multi_path(ExtrusionMultiPath* multipath)
{
    const auto print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    //Orio: no arcs on tilted layers, whose G-code follows the points of the paths.
    const bool enable_arc_fitting = print_config.enable_arc_fitting && ! this->object()->tilted_params().enabled;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < multipath->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            multipath->paths[i].simplify_by_fitting_arc(SCALED_SUPPORT_RESOLUTION);
        } else {
            multipath->paths[i].simplify(scaled_resolution);
        }
    }
}
//BBS: method to simplify support path
void Layer::simplify_support_loop(ExtrusionLoop* loop)
{
    const auto print_config = this->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    //Orio: no arcs on tilted layers, whose G-code follows the points of the paths.
    const bool enable_arc_fitting = print_config.enable_arc_fitting && ! this->object()->tilted_params().enabled;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < loop->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            loop->paths[i].simplify_by_fitting_arc(SCALED_SUPPORT_RESOLUTION);
        } else {
            loop->paths[i].simplify(scaled_resolution);
        }
    }
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void Layer::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static size_t idx = 0;
    this->export_region_fill_surfaces_to_svg(debug_out_path("Layer-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

coordf_t Layer::get_sparse_infill_max_void_area()
{
    double max_void_area = 0.;
    for (auto layerm : m_regions) {
        Flow flow = layerm->flow(frInfill);
        float density = layerm->region().config().sparse_infill_density;
        InfillPattern pattern = layerm->region().config().sparse_infill_pattern;
        if (density == 0.)
            return -1;

        //BBS: rough estimation and need to be optimized
        double spacing = flow.scaled_spacing() * (100 - density) / density;
        switch (pattern) {
            case ipConcentric:
            case ipRectilinear:
            case ipLine:
            case ipGyroid:
            case ipAlignedRectilinear:
            case ipOctagramSpiral:
            case ipHilbertCurve:
            case ip3DHoneycomb:
            case ipArchimedeanChords:
            case ip2DLattice: //this function seems to have been abandoned, there's no anywhere called this
                max_void_area = std::max(max_void_area, spacing * spacing);
                break;
            case ipGrid:
            case ipHoneycomb:
            case ipLightning:
                max_void_area = std::max(max_void_area, 4.0 * spacing * spacing);
                break;
            case ipCubic:
            case ipAdaptiveCubic:
            case ipTriangles:
            case ipStars:
            case ipSupportCubic:
                max_void_area = std::max(max_void_area, 4.5 * spacing * spacing);
                break;
            default:
                max_void_area = std::max(max_void_area, spacing * spacing);
                break;
        }
    };
    return max_void_area;
}

size_t Layer::get_filament_config_idx(unsigned int filament_id) const
{
    return m_object->print()->get_filament_config_idx(filament_id);
}

size_t Layer::get_process_config_idx(unsigned int filament_id) const
{
    return m_object->print()->get_process_config_idx(filament_id);
}


BoundingBox get_extents(const LayerRegion &layer_region)
{
    BoundingBox bbox;
    if (!layer_region.slices.surfaces.empty()) {
        bbox = get_extents(layer_region.slices.surfaces.front());
        for (auto it = layer_region.slices.surfaces.cbegin() + 1; it != layer_region.slices.surfaces.cend(); ++it)
            bbox.merge(get_extents(*it));
    }
    return bbox;
}

BoundingBox get_extents(const LayerRegionPtrs &layer_regions)
{
    BoundingBox bbox;
    if (!layer_regions.empty()) {
        bbox = get_extents(*layer_regions.front());
        for (auto it = layer_regions.begin() + 1; it != layer_regions.end(); ++it)
            bbox.merge(get_extents(**it));
    }
    return bbox;
}

}
