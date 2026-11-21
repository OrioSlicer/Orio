#pragma once
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "LayerFusion.hpp"
#include <boost/iostreams/device/mapped_file.hpp>
#include <imgui/imgui.h>
#include <vector>
#include <string>
#include <array>
#include <set>
class wxMouseEvent;
namespace Slic3r {
    class BuildVolume;
    class PresetBundle;
    struct ThumbnailData;
    struct ThumbnailsParams;
    class DynamicPrintConfig;
    class Print;
    struct PrintStatistics;
    namespace GUI {
        class IMSlider;
        class OpenGLManager;
        class PartPlateList;
        class IMSlider;
        namespace gcode
        {
            struct SequentialView;
            struct Extrusions;
            // Orio : tranchages memorises pour le bouton "tranchage precedent" de la legende
            struct SliceSnapshot;
            struct SliceHistory;
            enum class EViewType : unsigned char
            {
                Summary = 0,
                FeatureType,
                Height,
                Width,
                Feedrate,
                FanSpeed,
                Temperature,
                VolumetricRate,
                Tool,
                ColorPrint,
                FilamentId,
                LayerTime,
                AdditionalFanSpeed,
                // Orio : surface de contact avec la couche du dessous (LayerFusion). Valeur 13 lue par gcode.fs.
                LayerFusion,
                Count
            };

            struct ExtruderFilament
            {
                std::string   type;
                std::string   hex_color;
                unsigned char filament_id;
                bool is_support_filament;
            };

            // helper to render shells
            struct Shells
            {
                GLVolumeCollection volumes;
                bool               visible{ false };
                // BBS: always load shell when preview
                int  print_id{ -1 };
                int  print_modify_count{ -1 };
                bool previewing{ false };
                void reset();
            };

            using Color = std::array<float, 4>;

            struct ETools
            {
                std::vector<Color> m_tool_colors;
                std::vector<bool>  m_tool_visibles;
            };

            enum class EOptionsColors : unsigned char
            {
                Retractions,
                Unretractions,
                Seams,
                ToolChanges,
                ColorChanges,
                PausePrints,
                CustomGCodes
            };

            class BaseRenderer
            {
            public:
                explicit BaseRenderer();
                virtual ~BaseRenderer();
                virtual void init(ConfigOptionMode mode, PresetBundle* preset_bundle);
                bool is_legend_enabled() const;
                // Orio : [V] dans l'Apercu : affiche / masque les lignes selectionnees de la legende
                static void toggle_selected_legend_items();
                // Orio : Ctrl+A ; false si la legende n'a pas le focus
                static bool select_all_legend_items();
                void enable_legend(bool enable);
                float get_legend_height() const;
                const Shells& get_shells() const;
                const GCodeCheckResult& get_gcode_check_result() const;
                const FilamentPrintableResult& get_filament_printable_result() const;
                const ConflictResultOpt& get_conflict_result() const;
                size_t get_extruders_count() const;
                bool has_data() const;
                const float get_max_print_height() const;
                const BoundingBoxf3& get_paths_bounding_box() const;
                const BoundingBoxf3& get_max_bounding_box() const;
                const BoundingBoxf3& get_shell_bounding_box() const;
                bool is_contained_in_bed() const;
                EViewType get_view_type() const;
                std::vector<CustomGCode::Item>& get_custom_gcode_per_print_z();
                void update_shells_color_by_extruder(const DynamicPrintConfig* config);
                void load_shells(const Print& print, bool initialized, bool force_previewing = false);
                void set_shells_on_preview(bool is_previewing);
                const std::array<unsigned int, 2>& get_layers_z_range() const;
                void toggle_gcode_window_visibility();
                const std::shared_ptr<SequentialView>& get_sequential_view() const;
                void on_change_color_mode(bool is_dark);
                void set_scale(float scale = 1.0);
                virtual void update_marker_curr_move();
                void load(const GCodeProcessorResult& gcode_result, const Print& print, const BuildVolume& build_volume,
                    const std::vector<BoundingBoxf3>& exclude_bounding_box, bool initialized, ConfigOptionMode mode, bool only_gcode = false);

                void set_view_type(EViewType type, bool reset_feature_type_visible = true);
                void reset_visible(EViewType type);

                //BBS: add only gcode mode
                bool is_only_gcode_in_preview() const;
                /* BBS IMSlider */
                IMSlider* get_moves_slider();
                IMSlider* get_layers_slider();
                // Orio : curseurs de coupe X et Y de l'apercu
                IMSlider* get_x_slider();
                IMSlider* get_y_slider();
                // Met a jour les bornes des curseurs X/Y a partir de l'etendue des trajectoires
                void update_xy_sliders();
                // Definit la plage de coupe X/Y (coordonnees monde) ; enabled=false => aucune coupe
                void set_xy_clip_range(double x_min, double x_max, double y_min, double y_max, bool enabled);
                bool is_xy_clip_enabled() const { return m_clip_xy_enabled; }
                // Curseur vertical affiche : couches (Z) ou coupe X / Y selon la lettre. C'est lui, et non le
                // curseur Z cache, qui recoit la molette et les fleches haut/bas.
                IMSlider* get_active_vertical_slider();
                void on_vertical_slider_mouse_wheel(wxMouseEvent& evt);
                // Fleches haut/bas : deplace la poignee selectionnee de `steps` couches (Z) ou d'environ
                // `steps` mm (coupe X/Y)
                void move_vertical_slider(int steps);
                void reset_shell();
                // Orio : masquer un objet de l'Apercu (oeil au survol de la piece). label = etiquette de
                // l'instance dans le G-code (ModelInstance::get_labeled_id) ; seule sa coque reste, tres pale.
                bool can_hide_object(int label) const { return m_object_labels.count(label) > 0; }
                bool is_object_hidden(int label) const { return m_hidden_object_labels.count(label) > 0; }
                void set_object_hidden(int label, bool hidden) { if (hidden) m_hidden_object_labels.insert(label); else m_hidden_object_labels.erase(label); }
                // Orio : objet clique dans l'Apercu, analyse par l'encart de la vue "Fusion de couche"
                void set_layer_fusion_label(int label) { m_layer_fusion_label = label; }

                //BBS: add all plates filament statistics
                void render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show = true) const;

                void record_gcodeviewer_option_item();
                // Orio : curseurs verticaux (couches, coupes X / Y et leurs boutons) dessines comme dans une vue de cette
                // taille (IMSlider les cale sur son bord droit et en haut) : a gauche de la vue (GLCanvas3D) ; 0 = toute la vue.
                void set_vertical_sliders_view(int width, int height) { m_vertical_sliders_view = { { width, height } }; }
                // The color scheme last chosen (or the default one): also at the beginning of a slicing.
                void update_default_view_type();

                virtual void reset();
                // recalculate ranges in dependence of what is visible and sets tool/print colors
                virtual void refresh(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors);
                virtual void update_sequential_view_current(unsigned int first, unsigned int last) = 0;
                virtual void render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager) = 0;
                virtual std::vector<double> get_layers_zs() const = 0;
                virtual unsigned int get_options_visibility_flags() const = 0;
                virtual void set_options_visibility_from_flags(unsigned int flags) = 0;
                virtual void refresh_render_paths() = 0;
                virtual bool can_export_toolpaths() const = 0;
                virtual void export_toolpaths_to_obj(const char* filename) const = 0;
                virtual void set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range) = 0;
                virtual void render(int canvas_width, int canvas_height, int right_margin) = 0;
                virtual bool is_move_type_visible(EMoveType type) const = 0;
                virtual void set_move_type_visible(EMoveType type, bool visible) = 0;
                virtual bool is_extrusion_role_visible(ExtrusionRole role) const = 0;
                virtual void set_extrusion_role_visible(ExtrusionRole role, bool is_visible) = 0;
                virtual uint32_t get_extrusion_role_visibility_flags() const = 0;
                virtual void set_extrusion_role_visibility_flags(uint32_t flags) = 0;


                static const std::vector<Color> Extrusion_Role_Colors;
                static const std::vector<Color> Options_Colors;
                static const std::vector<Color> Travel_Colors;
                static const std::vector<ColorRGBA> Range_Colors;
                // Orio : vue "Fusion de couche", de 0 % (rouge) a 100 % (bleu)
                static const std::vector<ColorRGBA> Fusion_Colors;
                static const Color              Wipe_Color;
                static const Color              Neutral_Color;

            protected:
                void init_tool_maker(PresetBundle* preset_bundle);
                void render_sequential_view(uint32_t canvas_width, uint32_t canvas_height, int right_margin);
                virtual bool load_toolpaths(const GCodeProcessorResult& gcode_result, const BuildVolume& build_volume, const std::vector<BoundingBoxf3>& exclude_bounding_box) = 0;
                void render_shells();
                void render_slider(int canvas_width, int canvas_height);
                // Orio : bouton lettre X/Y/Z en haut du curseur ; un clic change d'axe actif
                void render_cut_axis_toggle(int canvas_width, int canvas_height);
                // Orio : transmet les uniforms de coupe X/Y au shader actif
                void apply_xy_clip_uniforms(GLShaderProgram& shader) const;
                // Orio : vue "Fusion de couche", piece cliquee (label du G-code) sur ce plateau, seule coupee par
                // le reglet X/Y/Z ; -1 : coupe de tout le plateau
                int layer_fusion_clip_label() const;
                // Orio : reperes des plans de coupe X/Y. fill = remplissage translucide, a dessiner
                // AVANT les trajectoires (elles le recouvrent, la tranche reste nette) ; sinon le cadre, APRES.
                void render_cut_planes(bool fill);
                // Orio : memorise la coupe de l'axe (0 = X, 1 = Y) que l'utilisateur vient de deplacer
                void remember_xy_cut(int axis);
                // Orio : bouton de remise a zero : coupes X, Y et Z en pleine plage, memoire X/Y effacee
                void reset_all_cuts();
                virtual void render_legend(float& legend_height, int canvas_width, int canvas_height, int right_margin);
                void apply_view_type_selection(int view_type_sel, EViewType type);
                void update_by_mode(ConfigOptionMode mode);
                void push_combo_style();
                void pop_combo_style();
                virtual void update_moves_slider(bool set_to_max = false);
                virtual bool show_sequential_view() const;
                virtual void on_visibility_changed();
                virtual void do_set_view_type(EViewType type);
                // Orio : vue "Fusion de couche" (rendu avance seulement : valeur par mouvement)
                virtual bool supports_layer_fusion() const { return false; }
                void render_layer_fusion_stats(float window_padding);
            private:
                void delete_wipe_tower();
                void render_legend_color_arr_recommen(float window_padding, bool is_show_left_right_result);
                // Orio : bouton "tranchage precedent" de la legende. update_slice_history() memorise le
                // tranchage affiche (par plateau) et renvoie le precedent de ce plateau, ou nullptr.
                SliceSnapshot* update_slice_history();
                // Echange les donnees affichees par la legende avec celles de l'instantane (2e appel = retour)
                void swap_with_slice_snapshot(SliceSnapshot& snapshot);
                // Bouton maintenu = tranchage precedent affiche ; sur sa propre ligne (bas de la legende), a
                // right_margin du bord droit du contenu ; showing_prev = le tranchage precedent est affiche
                void render_prev_slice_button(const SliceSnapshot* prev_slice, bool showing_prev, float right_margin);
                // Valeurs affichees par la legende pour les donnees en place, par cle : comparees entre les deux
                // tranchages pour mettre en surbrillance celles qui different
                std::map<std::string, std::string> legend_value_cells(bool imperial_units, const PrintStatistics& ps, double timelapse_time) const;

            protected:
                bool m_legend_enabled{ true };
                float m_legend_height{ 0.0f };
                size_t m_extruders_count{ 0 };
                std::vector<ExtrusionRole> m_roles;
                float m_max_print_height{ 0.0f };
                // bounding box of toolpaths
                BoundingBoxf3 m_paths_bounding_box;
                // bounding box of toolpaths + marker tools
                BoundingBoxf3 m_max_bounding_box;
                //BBS: add shell bounding box
                BoundingBoxf3 m_shell_bounding_box;
                bool m_contained_in_bed{ true };
                IMSlider* m_moves_slider{ nullptr };
                IMSlider* m_layers_slider{ nullptr };
                // Orio : curseurs verticaux de coupe X et Y
                IMSlider* m_x_slider{ nullptr };
                IMSlider* m_y_slider{ nullptr };
                // Axe actif du curseur unique de coupe : 0 = X, 1 = Y, 2 = Z (couches). Z par defaut.
                int m_active_cut_axis{ 2 };
                // Etat de la coupe X/Y transmis aux shaders : (xmin, xmax, ymin, ymax) en coordonnees monde,
                // une borne qui ne coupe pas etant repoussee tres loin
                bool m_clip_xy_enabled{ false };
                std::array<float, 4> m_clip_xy{ { -1.0e5f, 1.0e5f, -1.0e5f, 1.0e5f } };
                // Valeurs (mm) des curseurs X/Y, et bornes qui coupent vraiment (curseur sorti de son bout
                // de course) : seules ces dernieres ont un plan de coupe dessine
                std::array<double, 4> m_clip_xy_mm{ { 0.0, 0.0, 0.0, 0.0 } };
                std::array<bool, 4> m_clip_bound_active{ { false, false, false, false } };
                // Reperes des plans de coupe : rectangle unitaire (remplissage et cadre)
                GLModel m_cut_plane_fill;
                GLModel m_cut_plane_outline;
                // Etendue de reference des curseurs X/Y : ne reinitialiser qu'au changement de piece
                std::array<double, 2> m_xy_extent_x{ { 0.0, 0.0 } };
                std::array<double, 2> m_xy_extent_y{ { 0.0, 0.0 } };
                bool m_xy_extent_valid{ false };
                // Orio : etiquettes d'objets presentes dans le G-code affiche (remplies par le rendu
                // avance, seul a savoir filtrer par objet) et objets masques par l'oeil
                std::set<int> m_object_labels;
                std::set<int> m_hidden_object_labels;
                // Orio : vue "Fusion de couche" : calcul en arriere-plan, et ses valeurs deja passees aux trajets
                LayerFusion m_layer_fusion;
                bool m_layer_fusion_ready{ false };
                int  m_layer_fusion_label{ -1 };
                // Derniere coupe X/Y choisie a la main (xmin, xmax, ymin, ymax), en coordonnees du plateau,
                // et bornes qui coupaient. Survit a reset() : reprise a chaque nouveau tranchage si elle tombe
                // dans l'etendue des trajets ; seul un geste de l'utilisateur la modifie. Une par plateau
                // (indice), pour qu'une coupe ne soit pas reprise sur les autres plateaux.
                struct XYCutMemory {
                    std::array<double, 4> mm{ { 0.0, 0.0, 0.0, 0.0 } };
                    std::array<bool, 4> active{ { false, false, false, false } };
                };
                std::map<int, XYCutMemory> m_xy_cut_memory;
                XYCutMemory& current_xy_cut_memory();
                EViewType m_view_type{ EViewType::FeatureType };
                std::vector<CustomGCode::Item> m_custom_gcode_per_print_z;
                std::array<unsigned int, 2> m_layers_z_range{ 0, 0 };
                bool m_is_dark{ false };
                bool m_gl_data_initialized{ false };
                // the color scheme last chosen (index in view_type_items), -1 if none
                int m_last_option_item{ -1 };
                // Orio : voir set_vertical_sliders_view()
                std::array<int, 2> m_vertical_sliders_view{ { 0, 0 } };
                unsigned int m_last_result_id{ 0 };
                mutable std::shared_ptr<SequentialView> m_p_sequential_view{ nullptr };
                float m_scale{ 1.0 };
                std::vector<unsigned char> m_extruder_ids;
                //BBS: extruder dispensing filament
                std::vector<ExtruderFilament> m_left_extruder_filament;
                std::vector<ExtruderFilament> m_right_extruder_filament;
                size_t m_nozzle_nums{ 0 };
                bool m_fold{ false };
                std::vector<size_t> m_ssid_to_moveid_map;
                std::vector<int> m_plater_extruder;
                //BBS: save m_gcode_result as well
                const GCodeProcessorResult* m_gcode_result{ nullptr };
                GCodeProcessorResult::SettingsIds m_settings_ids;
                std::vector<float> m_filament_diameters;
                std::vector<float> m_filament_densities;
                PrintEstimatedStatistics m_print_statistics;
                PrintEstimatedStatistics::ETimeMode m_time_estimate_mode{ PrintEstimatedStatistics::ETimeMode::Normal };
                //BBS: add only gcode mode
                bool m_only_gcode_in_preview{ false };
                //BBS
                Shells            m_shells;
                const DynamicPrintConfig *m_config{ nullptr };//equal glcanvas3d m_config
                GCodeCheckResult  m_gcode_check_result;
                FilamentPrintableResult filament_printable_reuslt;
                ConflictResultOpt m_conflict_result;

                /*BBS GUI refactor, store displayed items in color scheme combobox */
                std::vector<EViewType> view_type_items;
                int m_view_type_sel = 0;
                struct ImageName
                {
                    std::string option_name;
                    ImTextureID texture_id{ nullptr };
                    ImTextureID texture_id_dark{ nullptr };
                };
                std::vector<ImageName> view_type_image_names;

                std::vector<EMoveType> options_items;
                ConfigOptionMode m_user_mode;

                std::shared_ptr<Extrusions> m_p_extrusions{ nullptr };
                //BBS save m_tools_color and m_tools_visible
                ETools m_tools;
                // Orio : dernier tranchage affiche et le precedent, par plateau (bouton de la legende)
                std::shared_ptr<SliceHistory> m_slice_history;
            };

            class GCodeWindow
            {
            public:
                explicit GCodeWindow();
                ~GCodeWindow();
                void load_gcode(const std::string& filename, const std::vector<size_t>& lines_ends);
                void reset();
                void toggle_visibility();
                //BBS: GUI refactor: add canvas size
                //void render(float top, float bottom, uint64_t curr_line_id) const;
                void render(float top, float bottom, float right, uint64_t curr_line_id) const;
                void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }
                void stop_mapping_file();

            private:
                struct Line
                {
                    std::string command;
                    std::string parameters;
                    std::string comment;
                };
                bool m_is_dark = false;
                bool m_visible{ true };
                uint64_t m_selected_line_id{ 0 };
                size_t m_last_lines_size{ 0 };
                std::string m_filename;
                boost::iostreams::mapped_file_source m_file;
                // map for accessing data in file by line number
                std::vector<size_t> m_lines_ends;
                // current visible lines
                std::vector<Line> m_lines;
            };

            struct SequentialView
            {
                class Marker
                {
                    GLModel m_model;
                    Vec3f m_world_position;
                    Transform3f m_world_transform;
                    // for seams, the position of the marker is on the last endpoint of the toolpath containing it
                    // the offset is used to show the correct value of tool position in the "ToolPosition" window
                    // see implementation of render() method
                    Vec3f m_world_offset;
                    float m_z_offset{ 0.5f };
                    GCodeProcessorResult::MoveVertex m_curr_move;
                    bool m_visible{ true };
                    bool m_is_dark = false;
                public:
                    float m_scale = 1.0f;
                    void init(std::string filename);
                    const BoundingBoxf3& get_bounding_box() const { return m_model.get_bounding_box(); }
                    void set_world_position(const Vec3f& position);
                    void set_world_offset(const Vec3f& offset) { m_world_offset = offset; }
                    bool is_visible() const { return m_visible; }
                    void set_visible(bool visible) { m_visible = visible; }
                    //BBS: GUI refactor: add canvas size
                    void render(int canvas_width, int canvas_height, const EViewType& view_type) const;
                    void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }
                    void update_curr_move(const GCodeProcessorResult::MoveVertex move);
                };
                struct Endpoints
                {
                    size_t first{ 0 };
                    size_t last{ 0 };
                };
                bool skip_invisible_moves{ false };
                Endpoints endpoints;
                Endpoints current;
                Endpoints last_current;
                Endpoints global;
                Vec3f current_position{ Vec3f::Zero() };
                Vec3f current_offset{ Vec3f::Zero() };
                Marker marker;
                GCodeWindow gcode_window;
                std::vector<unsigned int> gcode_ids;
                float m_scale = 1.0;
                //BBS: GUI refactor: add canvas size
                void render(float legend_height, int canvas_width, int canvas_height, int right_margin, const EViewType& view_type) const;
            };

            struct Range
            {
                enum class EType : unsigned char {
                    Linear,
                    Logarithmic
                };
                float min;
                float max;
                unsigned int count;
                bool log_scale;
                std::vector<ColorRGBA> range_colors;
                Range(std::vector<ColorRGBA> range_colors_a = BaseRenderer::Range_Colors) : range_colors(range_colors_a) { reset(); }

                void update_from(const float value) {
                    if (value != max && value != min)
                        ++count;
                    min = std::min(min, value);
                    max = std::max(max, value);
                }
                void reset(bool log = false) {
                    min = FLT_MAX;
                    max = -FLT_MAX;
                    count = 0;
                    log_scale = log;
                }
                float step_size(EType type = EType::Linear) const;
                ColorRGBA get_color_at(float value, EType type = EType::Linear) const;

                float get_value_at_step(int step) const;
                float get_color_size() const;
            };

            struct Ranges
            {
                // Color mapping by layer height.
                Range height;
                // Color mapping by extrusion width.
                Range width;
                // Color mapping by feedrate.
                Range feedrate;
                // Color mapping by fan speed.
                Range fan_speed;
                // Color mapping by additional fan speed.
                Range additional_fan_speed;
                // Color mapping by volumetric extrusion rate.
                Range volumetric_rate;
                // Color mapping by extrusion temperature.
                Range temperature;
                // Color mapping by layer time.
                Range layer_duration;

                void reset() {
                    height.reset();
                    width.reset();
                    feedrate.reset();
                    fan_speed.reset();
                    additional_fan_speed.reset();
                    volumetric_rate.reset();
                    temperature.reset();
                    layer_duration.reset(true);
                }
            };

            // helper to render extrusion paths
            struct Extrusions
            {
                uint32_t role_visibility_flags{ 0 };
                Ranges ranges;
                void reset_role_visibility_flags() {
                    role_visibility_flags = 0;
                    for (unsigned int i = 0; i < erCount; ++i) {
                        role_visibility_flags |= 1 << i;
                    }
                }
                void reset_ranges() { ranges.reset(); }
            };
        } // namespace gcode
    } // namespace GUI
} // namespace Slic3r