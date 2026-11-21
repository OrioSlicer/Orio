#include "GCodeViewer.hpp"
#include "slic3r/GUI/GCodeRenderer/AdvancedRenderer.hpp"
#include "slic3r/GUI/GCodeRenderer/LegacyRenderer.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/BoundingBox.hpp"
namespace Slic3r {
    namespace GUI {
        namespace gcode
        {
            GCodeViewer::GCodeViewer()
            {
            }
            GCodeViewer::~GCodeViewer()
            {
            }
            float GCodeViewer::get_legend_height() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_legend_height();
                }
                return 0.0f;
            }
            IMSlider* GCodeViewer::get_moves_slider()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_moves_slider();
                }
                return nullptr;
            }
            IMSlider* GCodeViewer::get_layers_slider()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_layers_slider();
                }
                return nullptr;
            }
            IMSlider* GCodeViewer::get_x_slider()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_x_slider();
                }
                return nullptr;
            }
            IMSlider* GCodeViewer::get_y_slider()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_y_slider();
                }
                return nullptr;
            }
            void GCodeViewer::update_xy_sliders()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->update_xy_sliders();
                }
            }
            void GCodeViewer::set_xy_clip_range(double x_min, double x_max, double y_min, double y_max, bool enabled)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->set_xy_clip_range(x_min, x_max, y_min, y_max, enabled);
                }
            }
            IMSlider* GCodeViewer::get_active_vertical_slider()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_active_vertical_slider();
                }
                return nullptr;
            }
            void GCodeViewer::on_vertical_slider_mouse_wheel(wxMouseEvent& evt)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->on_vertical_slider_mouse_wheel(evt);
                }
            }
            void GCodeViewer::move_vertical_slider(int steps)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->move_vertical_slider(steps);
                }
            }
            EViewType GCodeViewer::get_view_type() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_view_type();
                }
                return EViewType::Count;
            }
            void GCodeViewer::on_change_color_mode(bool is_dark)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->on_change_color_mode(is_dark);
                }
            }
            void GCodeViewer::init(ConfigOptionMode mode, Slic3r::PresetBundle* preset_bundle)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->init(mode, preset_bundle);
                }
            }
            void GCodeViewer::reset()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->reset();
                }
            }
            void GCodeViewer::update_sequential_view_current(unsigned int first, unsigned int last)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->update_sequential_view_current(first, last);
                }
            }
            void GCodeViewer::enable_legend(bool enable)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->enable_legend(enable);
                }
            }
            void GCodeViewer::render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->render_calibration_thumbnail(thumbnail_data, w, h, thumbnail_params, partplate_list, opengl_manager);
                }
            }
            bool GCodeViewer::is_legend_enabled()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->is_legend_enabled();
                }
                return false;
            }
            std::vector<double> GCodeViewer::get_layers_zs() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_layers_zs();
                }
                static std::vector<double> s_empty_list{};
                return s_empty_list;
            }
            size_t GCodeViewer::get_extruders_count() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_extruders_count();
                }
                return 0;
            }
            std::vector<CustomGCode::Item>& GCodeViewer::get_custom_gcode_per_print_z() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_custom_gcode_per_print_z();
                }
                static std::vector<CustomGCode::Item> s_empty_list{};
                return s_empty_list;
            }
            const BoundingBoxf3& GCodeViewer::get_paths_bounding_box() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_paths_bounding_box();
                }
                static BoundingBoxf3 s_empty;
                return s_empty;
            }
            const BoundingBoxf3& GCodeViewer::get_max_bounding_box() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_max_bounding_box();
                }
                static BoundingBoxf3 s_empty;
                return s_empty;
            }
            const BoundingBoxf3& GCodeViewer::get_shell_bounding_box() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_shell_bounding_box();
                }
                static BoundingBoxf3 s_empty;
                return s_empty;
            }
            int GCodeViewer::get_options_visibility_flags() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_options_visibility_flags();
                }
                return 0;
            }
            void GCodeViewer::set_options_visibility_from_flags(unsigned int flags)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->set_options_visibility_from_flags(flags);
                }
            }
            unsigned int GCodeViewer::get_toolpath_role_visibility_flags() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_extrusion_role_visibility_flags();
                }
                return 0;
            }
            void GCodeViewer::update_shells_color_by_extruder(const DynamicPrintConfig* config)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->update_shells_color_by_extruder(config);
                }
            }
            void GCodeViewer::reset_shell()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->reset_shell();
                }
            }
            void GCodeViewer::load_shells(const Print& print, bool initialized, bool force_previewing)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->load_shells(print, initialized, force_previewing);
                }
            }
            void GCodeViewer::set_shells_on_preview(bool is_previewing)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->set_shells_on_preview(is_previewing);
                }
            }
            void GCodeViewer::load(const GCodeProcessorResult& gcode_result, const Print& print, const BuildVolume& build_volume, const std::vector<BoundingBoxf3>& exclude_bounding_box, bool initialized, ConfigOptionMode mode, bool only_gcode)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->load(gcode_result, print, build_volume, exclude_bounding_box, initialized, mode, only_gcode);
                }
            }
            void GCodeViewer::refresh(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->refresh(gcode_result, str_tool_colors);
                }
            }
            void GCodeViewer::refresh_render_paths()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->refresh_render_paths();
                }
            }
            void GCodeViewer::toggle_gcode_window_visibility()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->toggle_gcode_window_visibility();
                }
            }
            bool GCodeViewer::can_export_toolpaths() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->can_export_toolpaths();
                }
                return false;
            }
            void GCodeViewer::export_toolpaths_to_obj(const char* filename) const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->export_toolpaths_to_obj(filename);
                }
            }
            bool GCodeViewer::has_data() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->has_data();
                }
                return false;
            }
            void GCodeViewer::set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->set_layers_z_range(layers_z_range);
                }
            }
            void GCodeViewer::update_marker_curr_move()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->update_marker_curr_move();
                }
            }
            void GCodeViewer::render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show) const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->render_all_plates_stats(gcode_result_list, show);
                }
            }
            bool GCodeViewer::is_contained_in_bed() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->is_contained_in_bed();
                }
                return false;
            }
            const std::array<unsigned int, 2>& GCodeViewer::get_layers_z_range() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_layers_z_range();
                }
                static std::array<unsigned int, 2> s_empty_array{};
                return s_empty_array;
            }
            const float GCodeViewer::get_max_print_height() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_max_print_height();
                }
                return 0.0f;
            }
            const GCodeCheckResult& GCodeViewer::get_gcode_check_result() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_gcode_check_result();
                }
                static GCodeCheckResult temp;
                return temp;
            }
            const FilamentPrintableResult& GCodeViewer::get_filament_printable_result() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_filament_printable_result();
                }
                static FilamentPrintableResult temp;
                return temp;
            }
            const ConflictResultOpt& GCodeViewer::get_conflict_result() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_conflict_result();
                }
                static ConflictResultOpt temp;
                return temp;
            }
            void GCodeViewer::render(int canvas_width, int canvas_height, int right_margin)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->render(canvas_width, canvas_height, right_margin);
                }
            }
            const Shells& GCodeViewer::get_shells() const
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    return p_renderer->get_shells();
                }
                static Shells s_empty;
                return s_empty;
            }
            bool GCodeViewer::can_hide_object(int label) const
            {
                const auto& p_renderer = get_renderer();
                return p_renderer && p_renderer->can_hide_object(label);
            }
            bool GCodeViewer::is_object_hidden(int label) const
            {
                const auto& p_renderer = get_renderer();
                return p_renderer && p_renderer->is_object_hidden(label);
            }
            void GCodeViewer::set_object_hidden(int label, bool hidden)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->set_object_hidden(label, hidden);
                }
            }
            void GCodeViewer::set_layer_fusion_label(int label)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->set_layer_fusion_label(label);
                }
            }
            void GCodeViewer::set_scale(float scale)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->set_scale(scale);
                }
            }
            void GCodeViewer::set_vertical_sliders_view(int width, int height)
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->set_vertical_sliders_view(width, height);
                }
            }
            void GCodeViewer::update_default_view_type()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->update_default_view_type();
                }
            }

            void GCodeViewer::record_record_gcodeviewer_option_item()
            {
                const auto& p_renderer = get_renderer();
                if (p_renderer) {
                    p_renderer->record_gcodeviewer_option_item();
                }
            }

            const std::shared_ptr<gcode::BaseRenderer>& GCodeViewer::get_renderer() const
            {
                const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
                const bool b_advanced_gcode_viewer_enabled = p_ogl_manager->is_advanced_gcode_viewer_enabled();
                const bool b_dirty = m_b_advanced_gcode_viewer_enabled != b_advanced_gcode_viewer_enabled;
                if (!m_p_renderer || b_dirty) {
                    if (p_ogl_manager) {
                        if (p_ogl_manager->init_gl()) {
                            const auto& gl_version = p_ogl_manager->get_gl_info().get_formated_gl_version();
                            if (b_advanced_gcode_viewer_enabled && gl_version >= 31) {
                                m_p_renderer = std::make_shared<gcode::AdvancedRenderer>();
                            }
                            else {
                                m_p_renderer = std::make_shared<gcode::LegacyRenderer>();
                            }
                            if (m_p_renderer) {
                                m_p_renderer->reset();
                            }
                        }
                    }
                    m_b_advanced_gcode_viewer_enabled = b_advanced_gcode_viewer_enabled;
                }
                return m_p_renderer;
            }
        } // namespace gcode
    } // namespace GUI
} // namespace Slic3r