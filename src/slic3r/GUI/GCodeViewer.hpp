#ifndef slic3r_GCodeViewer_hpp_
#define slic3r_GCodeViewer_hpp_
#include "slic3r/GUI/GCodeRenderer/BaseRenderer.hpp"
#include "3DScene.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "IMSlider.hpp"
#include "GLModel.hpp"
#include "I18N.hpp"
#include <boost/iostreams/device/mapped_file.hpp>
#include <cstdint>
#include <float.h>
#include <set>
#include <unordered_set>
namespace Slic3r {
    class Print;
    class TriangleMesh;
    class PresetBundle;
    class BoundingBoxf3;
    namespace GUI {
        class PartPlateList;
        class IMSlider;
        namespace gcode {
            class BaseRenderer;
            class GCodeViewer
            {
            public:
                explicit GCodeViewer();
                ~GCodeViewer();

                float get_legend_height() const;
                IMSlider* get_moves_slider();
                IMSlider* get_layers_slider();
                // Orio : curseurs de coupe X/Y de l'apercu
                IMSlider* get_x_slider();
                IMSlider* get_y_slider();
                void update_xy_sliders();
                void set_xy_clip_range(double x_min, double x_max, double y_min, double y_max, bool enabled);
                // Orio : molette et fleches haut/bas sur le curseur vertical affiche (Z ou coupe X/Y)
                IMSlider* get_active_vertical_slider();
                void on_vertical_slider_mouse_wheel(wxMouseEvent& evt);
                void move_vertical_slider(int steps);
                EViewType get_view_type() const;
                void on_change_color_mode(bool is_dark);
                void init(ConfigOptionMode mode, Slic3r::PresetBundle* preset_bundle);
                void reset();
                void update_sequential_view_current(unsigned int first, unsigned int last);
                void enable_legend(bool enable);
                void render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);
                bool is_legend_enabled();
                std::vector<double> get_layers_zs() const;
                size_t get_extruders_count() const;
                std::vector<CustomGCode::Item>& get_custom_gcode_per_print_z() const;
                const BoundingBoxf3& get_paths_bounding_box() const;
                const BoundingBoxf3& get_max_bounding_box() const;
                const BoundingBoxf3& get_shell_bounding_box() const;
                int get_options_visibility_flags() const;
                void set_options_visibility_from_flags(unsigned int flags);
                unsigned int get_toolpath_role_visibility_flags() const;
                void update_shells_color_by_extruder(const DynamicPrintConfig* config);
                void reset_shell();
                void load_shells(const Print& print, bool initialized, bool force_previewing = false);
                void set_shells_on_preview(bool is_previewing);
                void load(const GCodeProcessorResult& gcode_result, const Print& print, const BuildVolume& build_volume,
                    const std::vector<BoundingBoxf3>& exclude_bounding_box, bool initialized, ConfigOptionMode mode, bool only_gcode = false);
                void refresh(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors);
                void refresh_render_paths();
                void toggle_gcode_window_visibility();
                bool can_export_toolpaths() const;
                void export_toolpaths_to_obj(const char* filename) const;
                bool has_data() const;
                void set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range);
                void update_marker_curr_move();
                void render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show = true) const;
                bool is_contained_in_bed() const;
                const std::array<unsigned int, 2>& get_layers_z_range() const;
                const float get_max_print_height() const;
                const GCodeCheckResult& get_gcode_check_result() const;
                const FilamentPrintableResult& get_filament_printable_result() const;
                const ConflictResultOpt& get_conflict_result() const;
                void render(int canvas_width, int canvas_height, int right_margin);
                const Shells& get_shells() const;
                // Orio : oeil de l'Apercu qui masque un objet (BaseRenderer::set_object_hidden)
                bool can_hide_object(int label) const;
                bool is_object_hidden(int label) const;
                void set_object_hidden(int label, bool hidden);
                // Orio : objet clique dans l'Apercu (label du G-code, -1 : aucun) : cadre blanc, oeil garde, ses
                // statistiques dans la legende, l'encart de la vue "Fusion de couche"
                int  get_picked_label() const;
                void set_picked_label(int label);
                void set_scale(float scale = 1.0);
                // Orio: the vertical sliders drawn as in a view of this size (BaseRenderer::set_vertical_sliders_view)
                void set_vertical_sliders_view(int width, int height);
                // the color scheme last chosen (or the default one)
                void update_default_view_type();
                void record_record_gcodeviewer_option_item();
            private:
                const std::shared_ptr<gcode::BaseRenderer>& get_renderer() const;

            private:
                mutable std::shared_ptr<gcode::BaseRenderer> m_p_renderer{ nullptr };
                mutable bool m_b_advanced_gcode_viewer_enabled{ true };
            };
        } // namespace gcode
    } // namespace GUI
} // namespace Slic3r
#endif // slic3r_GCodeViewer_hpp_