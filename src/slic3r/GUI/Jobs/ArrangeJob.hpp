#ifndef ARRANGEJOB_HPP
#define ARRANGEJOB_HPP

#include "PlaterJob.hpp"
#include "ArrangePrusa.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {

class ModelInstance;

namespace GUI {

class ArrangeJob : public PlaterJob
{
    using ArrangePolygon = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    //BBS: add locked logic
    ArrangePolygons m_selected, m_unselected, m_unprintable, m_locked;
    std::map<int, ArrangePolygons> m_selected_groups;   // groups of selected items for sequential printing
    std::vector<int> m_uncompatible_plates;  // plate indices with different printing sequence than global

    arrangement::ArrangeParams params;
    int current_plate_index = 0;
    Polygon bed_poly;
    //Orio: "Arrange this plate" on a plate with the bed of the machine of its sheet (another printer): the config of the plate
    // sliced for that machine (PartPlate::sheet_machine_config()), read in prepare() (UI thread).
    bool               m_machine_bed { false };
    DynamicPrintConfig m_machine_bed_config;
    //Orio: "Arrange" (all the plates) when plates have the bed of another machine (sheets of the plates): the objects of all
    // the plates are spread over all of them, each plate with the bed and the settings of its machine. The plates are filled one by
    // one, the largest beds first (fewest plates; an object too large for a small bed goes to a larger one), then new plates of the
    // project printer are added (process_machine_beds()). One bed by plate which is not locked, read in prepare_all() (UI thread).
    struct MachineBed
    {
        int                        plate_index { 0 };
        int                        bed_index { 0 };    // among the plates which are not locked: ArrangePolygon::bed_idx for finalize()
        bool                       machine { false };  // bed of another machine
        DynamicPrintConfig         config;             // the plate sliced for its machine, else the global config
        arrangement::ArrangeParams params;
        double                     area { 0. };        // mm2
    };
    std::vector<MachineBed> m_machine_beds;            // empty: all the plates have the bed of the project printer
    DynamicPrintConfig      m_project_config;          // the global config, for the new plates
    double                  m_max_printable_height { 0. };
    int                     m_new_plates_max { 0 };

    //Orio: the arrange engine of PrusaSlicer (ArrangePrusa.hpp) arranges by layer printing. Its options
    // are read in prepare(), and it may use the real outline of the objects: the meshes of their model parts and
    // their transformations are copied in prepare() (UI thread), the outline is computed in process(). Key =
    // container (m_selected or m_unselected) and ArrangePolygon::itemid given by prepare_*().
    struct OutlineSource
    {
        std::vector<std::pair<std::shared_ptr<const TriangleMesh>, Transform3d>> parts;
    };
    ArrangePrusaSettings m_prusa_settings;
    std::map<std::pair<const ArrangePolygons *, int>, OutlineSource> m_outline_sources;

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    void remember_instance(const ArrangePolygons &cont, int itemid, const ModelInstance *instance);
    // Union of the projections of the model parts, in the coordinates of ArrangePolygon::poly.
    ExPolygons full_outline(const ArrangePolygon &ap, bool fixed) const;

    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    void prepare_all();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();

    // prepare the items which are selected and not on the current partplate
    void prepare_outside_plate();

    void prepare_wipe_tower(bool select = false);

    //Orio: plates with the bed of another machine (m_machine_beds).
    void prepare_machine_beds();
    void process_machine_beds();
    // Arranges the items around the fixed ones on beds of that config (shape, excluded regions), at most max_beds of them, as
    // process() does on the beds of the project printer; placed: items already arranged (progress).
    void arrange_on_beds(ArrangePolygons &items, ArrangePolygons &fixed, const DynamicPrintConfig &config,
                         arrangement::ArrangeParams &arr_params, bool machine_bed, int max_beds, size_t placed);

protected:

    void prepare() override;

    void check_unprintable();

    void on_exception(const std::exception_ptr &) override;

    void process() override;

public:
    ArrangeJob(std::shared_ptr<ProgressIndicator> pri, Plater *plater)
        : PlaterJob{std::move(pri), plater}
    {}

    int status_range() const override
    {
        // ensure finalize() is called after all operations in process() is finished.
        return int(m_selected.size() + m_unprintable.size() + 1);
    }

    void finalize() override;

    static ArrangePolygon prepare_arrange_polygon(void *instance);
};

std::optional<arrangement::ArrangePolygon> get_wipe_tower_arrangepoly(const Plater &);

// The gap between logical beds in the x axis expressed in ratio of
// the current bed width.
static const constexpr double LOGICAL_BED_GAP = 1. / 5.;

//BBS: add sudoku-style strides for x and y
// Stride between logical beds
double bed_stride_x(const Plater* plater);
double bed_stride_y(const Plater* plater);

arrangement::ArrangeParams init_arrange_params(Plater *p);

}} // namespace Slic3r::GUI

#endif // ARRANGEJOB_HPP
