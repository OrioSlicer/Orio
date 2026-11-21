#pragma once
// Orio : "Layer Fusion" view of the Preview. Contact area between each layer of the part and the
// layer below, computed from the slicer extrusions (not from the G-code), in a background thread.
//
// "Real contact" model: a line is a rectangle with round ends (the stadium of the slicer flow model), only its
// flat (width - height) touches the line below. With brick layers the raised lines are matched with the
// level they really rest on (top of the line below = bottom of this line).
#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace Slic3r {
    class Print;
    struct GCodeProcessorResult;
    namespace GUI {
        namespace gcode {
            class LayerFusion
            {
            public:
                struct LayerStat
                {
                    double print_z;
                    double height;
                    // mm2 of the flats of this layer resting on the layer below; < 0 for the first layer (on the bed)
                    double contact;
                    // mm2 of the layer section (slices of the part)
                    double section;
                };
                struct ObjectStats
                {
                    bool                   tilted{ false };
                    std::vector<LayerStat> layers;
                };
                struct Result
                {
                    unsigned int gcode_id{ 0 };
                    // per G-code move: % of the line resting on the layer below (0..100), -1 = nothing below to
                    // compare with (first layer, supports, wipe tower, tilted layers...)
                    std::vector<float> move_ratio;
                    // key: ModelObject id
                    std::map<size_t, ObjectStats> objects;
                };

                ~LayerFusion() { reset(); }
                // Main thread. Starts the computation for this G-code result, unless already done or running.
                void request(const Print& print, const GCodeProcessorResult& gcode_result);
                // Main thread. Cancels a running computation and forgets the result.
                void reset();
                bool busy() const;
                // nullptr until the computation is finished
                const Result* result() const;

                struct Job;
            private:
                std::shared_ptr<Job> m_job;
                std::thread          m_thread;
            };
        } // namespace gcode
    } // namespace GUI
} // namespace Slic3r
