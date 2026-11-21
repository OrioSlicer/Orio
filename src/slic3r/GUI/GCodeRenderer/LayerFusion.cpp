#include "LayerFusion.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <tuple>

namespace Slic3r {
    namespace GUI {
        namespace gcode {
            namespace {
                // Raster used to tell, for each G-code line, which part of it rests on the layer below (mm).
                constexpr double RASTER_RES   = 0.05;
                // Earlier layers searched for the level a line rests on (combined infill rests several layers down).
                constexpr int    LEVEL_SEARCH = 8;
                // A line rests on the level whose top is the nearest to its bottom, within this ratio of its height.
                constexpr double LEVEL_TOL    = 0.4;
                // G-code lines: object NONE: nothing to compare with (not on a part, first layer on the bed or
                // the raft, tilted layers) ; layer IN_AIR: on the part, nothing below.
                constexpr int NONE = -1, IN_AIR = -2;

                bool is_object_role(ExtrusionRole role)
                {
                    switch (role) {
                    case erPerimeter: case erExternalPerimeter: case erOverhangPerimeter: case erInternalInfill:
                    case erSolidInfill: case erFloatingVerticalShell: case erTopSolidInfill: case erBottomSurface:
                    case erBridgeInfill: case erGapFill: case erScarfSeam:
                        return true;
                    default:
                        return false;
                    }
                }

                // Width of the flat of a line, the part touching the line below / above (mm).
                float flat_width(ExtrusionRole role, bool wave_overhang, float width, float height)
                {
                    // ponytail: bridges and wave lines are round (width = height, no flat); count half of the width,
                    // the next layer squashes onto them. Measure on prints before refining.
                    if (role == erBridgeInfill || role == erOverhangPerimeter || wave_overhang)
                        return 0.5f * width;
                    return std::max(width - height, 0.f);
                }

                long long key_um(double z) { return std::llround(z * 1000.); }

                // Union of flats at a given Z (top or bottom of the lines).
                struct Level
                {
                    double   z{ 0. };
                    float    height{ FLT_MAX };  // smallest height of the lines (tolerance of the match)
                    Polygons area;
                };

                // Level a line rests on: among the tops of the LEVEL_SEARCH layers below layer_id, the nearest to the
                // bottom of the line (z), within LEVEL_TOL of its height. Returns false if none (line in the air).
                bool nearest_level(const std::vector<std::vector<Level>>& tops, size_t layer_id, double z, float height,
                                   size_t& out_layer, size_t& out_level)
                {
                    double best_dz = DBL_MAX;
                    for (size_t j = std::min(layer_id, tops.size()); j > 0 && layer_id - j < size_t(LEVEL_SEARCH); --j)
                        for (size_t k = 0; k < tops[j - 1].size(); ++k)
                            if (double dz = std::abs(tops[j - 1][k].z - z); dz < best_dz) {
                                best_dz = dz;
                                out_layer = j - 1;
                                out_level = k;
                            }
                    return best_dz <= LEVEL_TOL * height;
                }

                // Pixels inside the polygons (even-odd, union_ gives no overlap), object coordinates in mm.
                struct Raster
                {
                    double x0{ 0. }, y0{ 0. };
                    int    w{ 0 }, h{ 0 };
                    std::vector<uint8_t> px;

                    explicit Raster(const Polygons& polys)
                    {
                        if (polys.empty())
                            return;
                        const BoundingBox bb = get_extents(polys);
                        x0 = unscale<double>(bb.min.x()) - RASTER_RES;
                        y0 = unscale<double>(bb.min.y()) - RASTER_RES;
                        w = int(std::ceil(unscale<double>(bb.max.x() - bb.min.x()) / RASTER_RES)) + 3;
                        h = int(std::ceil(unscale<double>(bb.max.y() - bb.min.y()) / RASTER_RES)) + 3;
                        px.assign(size_t(w) * size_t(h), 0);
                        std::vector<std::vector<double>> xs(h);
                        for (const Polygon& poly : polys)
                            for (size_t i = 0; i < poly.points.size(); ++i) {
                                const Vec2d p = unscale(poly.points[i]);
                                const Vec2d q = unscale(poly.points[(i + 1) % poly.points.size()]);
                                if (p.y() == q.y())
                                    continue;
                                const double ylo = std::min(p.y(), q.y()), yhi = std::max(p.y(), q.y());
                                // rows whose center is in [ylo, yhi)
                                const int j0 = std::max(0, int(std::ceil((ylo - y0) / RASTER_RES - 0.5)));
                                const int j1 = std::min(h - 1, int(std::ceil((yhi - y0) / RASTER_RES - 0.5)) - 1);
                                for (int j = j0; j <= j1; ++j) {
                                    const double yc = y0 + (j + 0.5) * RASTER_RES;
                                    xs[j].push_back(p.x() + (yc - p.y()) * (q.x() - p.x()) / (q.y() - p.y()));
                                }
                            }
                        for (int j = 0; j < h; ++j) {
                            std::vector<double>& row = xs[j];
                            std::sort(row.begin(), row.end());
                            for (size_t k = 0; k + 1 < row.size(); k += 2) {
                                const int i0 = std::max(0, int(std::ceil((row[k] - x0) / RASTER_RES - 0.5)));
                                const int i1 = std::min(w - 1, int(std::floor((row[k + 1] - x0) / RASTER_RES - 0.5)));
                                for (int i = i0; i <= i1; ++i)
                                    px[size_t(j) * size_t(w) + size_t(i)] = 1;
                            }
                        }
                    }
                    bool inside(double x, double y) const
                    {
                        const int i = int(std::floor((x - x0) / RASTER_RES));
                        const int j = int(std::floor((y - y0) / RASTER_RES));
                        return i >= 0 && j >= 0 && i < w && j < h && px[size_t(j) * size_t(w) + size_t(i)] != 0;
                    }
                };
            } // namespace

            struct LayerFusion::Job
            {
                struct PathS
                {
                    Polyline polyline;
                    float    flat;
                    float    top;
                    float    height;
                };
                struct LayerS
                {
                    double print_z;
                    double height;
                    double section;
                    std::vector<PathS> paths;
                };
                struct ObjectS
                {
                    size_t id;
                    bool   tilted;
                    std::vector<Vec2d>  offsets; // instance shifts (plate origin included): object coordinates -> G-code moves (mm)
                    BoundingBoxf        bbox;    // object coordinates (mm)
                    std::vector<LayerS> layers;
                };
                // A G-code extrusion line (an arc gives several, same id)
                struct MoveS
                {
                    uint32_t id;
                    Vec2f    a, b;
                    float    z, height, flat;
                };

                std::vector<ObjectS> objects;
                std::vector<MoveS>   moves;
                size_t               moves_count{ 0 };
                std::unique_ptr<Result> result;
                unsigned int         result_gcode_id{ 0 };
                std::atomic<bool>    cancel{ false };
                std::atomic<bool>    done{ false };

                void add_path(const ExtrusionPath& path, double print_z, LayerS& out)
                {
                    const ExtrusionRole role = path.role();
                    if (!is_object_role(role) || path.is_force_no_extrusion() || path.width <= 0.f || path.height <= 0.f || path.polyline.size() < 2)
                        return;
                    const float flat = flat_width(role, path.wave_overhang, path.width, path.height);
                    if (flat < 0.01f)
                        return;
                    out.paths.push_back({ path.polyline, flat, float(print_z + path.z_shift), path.height });
                }

                void add_entity(const ExtrusionEntity* entity, double print_z, LayerS& out)
                {
                    if (auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
                        for (const ExtrusionEntity* e : collection->entities)
                            add_entity(e, print_z, out);
                    } else if (auto* path = dynamic_cast<const ExtrusionPath*>(entity)) {
                        add_path(*path, print_z, out);
                    } else if (auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
                        for (const ExtrusionPath& p : multipath->paths)
                            add_path(p, print_z, out);
                    } else if (auto* loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
                        for (const ExtrusionPath& p : loop->paths)
                            add_path(p, print_z, out);
                    }
                }

                // Main thread: copy of what the computation needs, the Print may change while it runs.
                void snapshot(const Print& print, const GCodeProcessorResult& gcode_result)
                {
                    for (const PrintObject* po : print.objects()) {
                        if (!po->is_step_done(posInfill) || po->layers().empty())
                            continue;
                        ObjectS o;
                        o.id = po->model_object()->id().id;
                        o.tilted = po->tilted_params().enabled;
                        for (const PrintInstance& inst : po->instances())
                            o.offsets.push_back(unscale(inst.shift));
                        for (const Layer* layer : po->layers()) {
                            LayerS l{ layer->print_z, layer->height, area(layer->lslices) * SCALING_FACTOR * SCALING_FACTOR, {} };
                            for (const ExPolygon& slice : layer->lslices) {
                                const BoundingBox bb = get_extents(slice.contour);
                                o.bbox.merge(unscale(bb.min));
                                o.bbox.merge(unscale(bb.max));
                            }
                            if (!o.tilted)
                                for (const LayerRegion* region : layer->regions()) {
                                    add_entity(&region->perimeters, layer->print_z, l);
                                    add_entity(&region->fills, layer->print_z, l);
                                }
                            o.layers.push_back(std::move(l));
                        }
                        objects.push_back(std::move(o));
                    }

                    const auto& gmoves = gcode_result.moves;
                    moves_count = gmoves.size();
                    for (size_t i = 1; i < gmoves.size(); ++i) {
                        const GCodeProcessorResult::MoveVertex& m = gmoves[i];
                        if (m.type != EMoveType::Extrude || !is_object_role(m.extrusion_role) || m.width <= 0.f || m.height <= 0.f)
                            continue;
                        // the G-code has no wave flag: wave lines are counted as plain overhang walls
                        const float flat = flat_width(m.extrusion_role, false, m.width, m.height);
                        if (flat < 0.01f)
                            continue;
                        Vec2f a = gmoves[i - 1].position.head<2>();
                        auto add = [&](const Vec2f& b) {
                            moves.push_back({ uint32_t(i), a, b, m.position.z(), m.height, flat });
                            a = b;
                        };
                        if (m.is_arc_move_with_interpolation_points())
                            for (const Vec3f& p : m.interpolation_points)
                                add(p.head<2>());
                        add(m.position.head<2>());
                    }
                }

                void run()
                {
                    auto res = std::make_unique<Result>();
                    std::vector<std::vector<std::vector<Level>>> object_tops(objects.size());

                    for (size_t io = 0; io < objects.size() && !cancel; ++io) {
                        const ObjectS& o = objects[io];
                        ObjectStats& stats = res->objects[o.id];
                        stats.tilted = o.tilted;
                        const size_t n = o.layers.size();
                        std::vector<std::vector<Level>>& tops = object_tops[io];
                        std::vector<std::vector<Level>> bottoms(n);
                        tops.assign(n, {});
                        // flats of each layer, grouped by the Z of their top and of their bottom
                        tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t>& range) {
                            for (size_t il = range.begin(); il < range.end() && !cancel; ++il) {
                                struct Bin { double top, bottom; float height, flat; Polylines polylines; };
                                std::map<std::tuple<long long, long long, int>, Bin> bins;
                                for (const PathS& p : o.layers[il].paths) {
                                    const double bottom = double(p.top) - double(p.height);
                                    Bin& bin = bins[{ key_um(p.top), key_um(bottom), int(std::lround(p.flat * 200.f)) }];
                                    if (bin.polylines.empty())
                                        bin = { p.top, bottom, p.height, p.flat, {} };
                                    bin.height = std::min(bin.height, p.height);
                                    bin.polylines.push_back(p.polyline);
                                }
                                std::map<long long, Level> t, d;
                                for (auto& [key, bin] : bins) {
                                    Polygons footprint = offset(bin.polylines, float(scale_(0.5 * bin.flat)));
                                    Level& lt = t[std::get<0>(key)];
                                    lt.z = bin.top;
                                    lt.height = std::min(lt.height, bin.height);
                                    append(lt.area, footprint);
                                    Level& ld = d[std::get<1>(key)];
                                    ld.z = bin.bottom;
                                    ld.height = std::min(ld.height, bin.height);
                                    append(ld.area, std::move(footprint));
                                }
                                for (auto& [key, level] : t) {
                                    level.area = union_(level.area);
                                    tops[il].push_back(std::move(level));
                                }
                                for (auto& [key, level] : d) {
                                    level.area = union_(level.area);
                                    bottoms[il].push_back(std::move(level));
                                }
                            }
                        });
                        stats.layers.resize(n);
                        tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t>& range) {
                            for (size_t il = range.begin(); il < range.end() && !cancel; ++il) {
                                double contact = il == 0 ? -1. : 0.;
                                size_t jl, jv;
                                if (il > 0)
                                    for (const Level& bottom : bottoms[il])
                                        if (nearest_level(tops, il, bottom.z, bottom.height, jl, jv))
                                            contact += area(intersection(bottom.area, tops[jl][jv].area)) * SCALING_FACTOR * SCALING_FACTOR;
                                stats.layers[il] = { o.layers[il].print_z, o.layers[il].height, contact, o.layers[il].section };
                            }
                        });
                    }
                    if (cancel)
                        return;

                    // G-code lines: object and level they rest on (NONE, IN_AIR: see above)
                    struct Hit { int object{ NONE }; int offset{ 0 }; int layer{ IN_AIR }; int level{ 0 }; };
                    std::vector<Hit> hits(moves.size());
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, moves.size()), [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t im = range.begin(); im < range.end() && !cancel; ++im) {
                            const MoveS& m = moves[im];
                            const Vec2d mid = 0.5 * (m.a + m.b).cast<double>();
                            const double bottom = double(m.z) - double(m.height);
                            Hit& hit = hits[im];
                            bool first = true;
                            for (size_t io = 0; io < objects.size() && hit.layer == IN_AIR; ++io) {
                                const ObjectS& o = objects[io];
                                if (o.layers.empty())
                                    continue;
                                for (size_t ii = 0; ii < o.offsets.size() && hit.layer == IN_AIR; ++ii) {
                                    const Vec2d p = mid - o.offsets[ii];
                                    if (p.x() < o.bbox.min.x() - 1. || p.x() > o.bbox.max.x() + 1. ||
                                        p.y() < o.bbox.min.y() - 1. || p.y() > o.bbox.max.y() + 1.)
                                        continue;
                                    const LayerS& l0 = o.layers.front();
                                    const bool nothing_below = o.tilted || bottom <= l0.print_z - l0.height + LEVEL_TOL * m.height;
                                    if (first)
                                        hit = { nothing_below ? NONE : int(io), int(ii), IN_AIR, 0 };
                                    first = false;
                                    if (nothing_below)
                                        continue;
                                    // layers whose print_z is below the line (a raised brick line also sees its own layer)
                                    const size_t il = size_t(std::upper_bound(o.layers.begin(), o.layers.end(), double(m.z) - 1e-3,
                                        [](double z, const LayerS& l) { return z < l.print_z; }) - o.layers.begin());
                                    size_t jl, jv;
                                    if (nearest_level(object_tops[io], il, bottom, m.height, jl, jv))
                                        hit = { int(io), int(ii), int(jl), int(jv) };
                                }
                            }
                        }
                    });
                    if (cancel)
                        return;

                    // one raster per level touched, the lines resting on it sampled across their flat
                    std::map<std::tuple<int, int, int>, std::vector<size_t>> buckets;
                    for (size_t im = 0; im < moves.size(); ++im)
                        if (hits[im].object != NONE)
                            buckets[{ hits[im].object, hits[im].layer, hits[im].level }].push_back(im);
                    std::vector<const std::vector<size_t>*> bucket_list;
                    std::vector<std::tuple<int, int, int>> bucket_keys;
                    for (const auto& [key, list] : buckets) {
                        bucket_keys.push_back(key);
                        bucket_list.push_back(&list);
                    }
                    std::vector<std::pair<uint32_t, uint32_t>> counts(moves.size(), { 0, 0 }); // inside, total
                    tbb::parallel_for(tbb::blocked_range<size_t>(0, bucket_list.size()), [&](const tbb::blocked_range<size_t>& range) {
                        for (size_t ib = range.begin(); ib < range.end() && !cancel; ++ib) {
                            const auto [io, il, iv] = bucket_keys[ib];
                            if (il == IN_AIR) {
                                for (size_t im : *bucket_list[ib])
                                    counts[im] = { 0, 1 };
                                continue;
                            }
                            const Raster raster(object_tops[io][il][iv].area);
                            for (size_t im : *bucket_list[ib]) {
                                const MoveS& m = moves[im];
                                const Vec2d off = objects[io].offsets[hits[im].offset];
                                const Vec2d a = m.a.cast<double>() - off, b = m.b.cast<double>() - off;
                                const Vec2d ab = b - a;
                                const double len = ab.norm();
                                if (len < 1e-6)
                                    continue;
                                const Vec2d nrm = Vec2d(-ab.y(), ab.x()) / len;
                                const int along = std::max(1, int(len / RASTER_RES));
                                // Points kept 3/4 of a pixel inside the flat: a pixel counts when its center is inside, so a point
                                // on the very edge of the flat came out inside on one side of a wall and outside on the other
                                // (the two outer faces of a cube blue, the two others violet).
                                const double span = std::max(0., double(m.flat) - 1.5 * RASTER_RES);
                                uint32_t in = 0, total = 0;
                                for (int s = 0; s < along; ++s) {
                                    const Vec2d c = a + ab * ((s + 0.5) / along);
                                    for (int k = 0; k < 5; ++k) {
                                        const Vec2d p = c + nrm * (span * (k / 4. - 0.5));
                                        in += raster.inside(p.x(), p.y()) ? 1 : 0;
                                        ++total;
                                    }
                                }
                                counts[im] = { in, total };
                            }
                        }
                    });
                    if (cancel)
                        return;

                    res->move_ratio.assign(moves_count, -1.f);
                    std::vector<std::pair<uint64_t, uint64_t>> per_id(moves_count, { 0, 0 });
                    for (size_t im = 0; im < moves.size(); ++im) {
                        per_id[moves[im].id].first += counts[im].first;
                        per_id[moves[im].id].second += counts[im].second;
                    }
                    for (size_t id = 0; id < moves_count; ++id)
                        if (per_id[id].second > 0)
                            res->move_ratio[id] = float(100. * double(per_id[id].first) / double(per_id[id].second));
                    result = std::move(res);
                }
            };

            void LayerFusion::request(const Print& print, const GCodeProcessorResult& gcode_result)
            {
                if (m_job && m_job->result_gcode_id == gcode_result.id)
                    return;
                // the background slicing works on the Print until its G-code is exported
                if (!print.finished() || gcode_result.moves.empty())
                    return;
                reset();
                m_job = std::make_shared<Job>();
                m_job->result_gcode_id = gcode_result.id;
                m_job->snapshot(print, gcode_result);
                std::shared_ptr<Job> job = m_job;
                m_thread = std::thread([job]() {
                    job->run();
                    if (job->cancel)
                        return;
                    if (job->result)
                        job->result->gcode_id = job->result_gcode_id;
                    job->done = true;
                    wxGetApp().CallAfter([]() {
                        if (Plater* plater = wxGetApp().plater())
                            if (GLCanvas3D* canvas = plater->get_current_canvas3D()) {
                                canvas->set_as_dirty();
                                canvas->request_extra_frame();
                            }
                    });
                });
            }

            void LayerFusion::reset()
            {
                if (m_job)
                    m_job->cancel = true;
                if (m_thread.joinable())
                    m_thread.join();
                m_job.reset();
            }

            bool LayerFusion::busy() const
            {
                return m_job && !m_job->done;
            }

            const LayerFusion::Result* LayerFusion::result() const
            {
                return (m_job && m_job->done) ? m_job->result.get() : nullptr;
            }
        } // namespace gcode
    } // namespace GUI
} // namespace Slic3r
