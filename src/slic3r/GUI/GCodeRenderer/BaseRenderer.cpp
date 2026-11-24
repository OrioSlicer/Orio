#include "BaseRenderer.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/FilamentGroupPopup.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/DeviceCore/DevUtilBackend.h"
#include "../DeviceCore/DevConfigUtil.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include <boost/nowide/convert.hpp>
#include <imgui/imgui_internal.h>
#include <GL/glew.h>
#include <wx/datetime.h>
#include <set>
namespace
{
    std::string get_view_type_string(Slic3r::GUI::gcode::EViewType view_type)
    {
        if (view_type == Slic3r::GUI::gcode::EViewType::Summary)
            return _u8L("Summary");
        else if (view_type == Slic3r::GUI::gcode::EViewType::FeatureType)
            return _u8L("Line Type");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Height)
            return _u8L("Layer Height");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Width)
            return _u8L("Line Width");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Feedrate)
            return _u8L("Speed");
        else if (view_type == Slic3r::GUI::gcode::EViewType::FanSpeed)
            return _u8L("Fan Speed");
        else if (view_type == Slic3r::GUI::gcode::EViewType::AdditionalFanSpeed)
            return _u8L("AUX Fan Speed");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Temperature)
            return _u8L("Temperature");
        else if (view_type == Slic3r::GUI::gcode::EViewType::VolumetricRate)
            return _u8L("Flow");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Tool)
            return _u8L("Tool");
        else if (view_type == Slic3r::GUI::gcode::EViewType::ColorPrint)
            return _u8L("Filament");
        else if (view_type == Slic3r::GUI::gcode::EViewType::LayerTime)
            return _u8L("Layer Time");
        else if (view_type == Slic3r::GUI::gcode::EViewType::LayerFusion)
            return _u8L("Layer Fusion");
        return "";
    }

    static std::array<float, 4> decode_color(const std::string& color) {
        static const float INV_255 = 1.0f / 255.0f;
        std::array<float, 4> ret = { 0.0f, 0.0f, 0.0f, 1.0f };
        const char* c = color.data() + 1;
        if (color.size() == 7 && color.front() == '#') {
            for (size_t j = 0; j < 3; ++j) {
                int digit1 = Slic3r::GUI::hex_digit_to_int(*c++);
                int digit2 = Slic3r::GUI::hex_digit_to_int(*c++);
                if (digit1 == -1 || digit2 == -1)
                    break;
                ret[j] = float(digit1 * 16 + digit2) * INV_255;
            }
        }
        else if (color.size() == 9 && color.front() == '#') {
            for (size_t j = 0; j < 4; ++j) {
                int digit1 = Slic3r::GUI::hex_digit_to_int(*c++);
                int digit2 = Slic3r::GUI::hex_digit_to_int(*c++);
                if (digit1 == -1 || digit2 == -1)
                    break;
                ret[j] = float(digit1 * 16 + digit2) * INV_255;
            }
        }
        return ret;
    }

    std::vector<std::array<float, 4>> decode_colors(const std::vector<std::string>& colors) {
        std::vector<std::array<float, 4>> output(colors.size(), { 0.0f, 0.0f, 0.0f, 1.0f });
        for (size_t i = 0; i < colors.size(); ++i) {
            output[i] = decode_color(colors[i]);
        }
        return output;
    }

    // Round to a bin with minimum two digits resolution.
            // Equivalent to conversion to string with sprintf(buf, "%.2g", value) and conversion back to float, but faster.
    static float round_to_bin(const float value)
    {
        //    assert(value > 0);
        constexpr float const scale[5] = { 100.f,  1000.f,  10000.f,  100000.f,  1000000.f };
        constexpr float const invscale[5] = { 0.01f,  0.001f,  0.0001f,  0.00001f,  0.000001f };
        constexpr float const threshold[5] = { 0.095f, 0.0095f, 0.00095f, 0.000095f, 0.0000095f };
        // Scaling factor, pointer to the tables above.
        int                   i = 0;
        // While the scaling factor is not yet large enough to get two integer digits after scaling and rounding:
        for (; value < threshold[i] && i < 4; ++i);
        return std::round(value * scale[i]) * invscale[i];
    }

    // Orio : lisibilite de la coupe X/Y de l'apercu
    // borne qui ne coupe pas : repoussee hors de portee (aucun rejet de ce cote)
    constexpr float CLIP_XY_FAR = 1.0e5f;
    // marge du repere de plan autour des trajectoires (cotes non coupes et dessus)
    constexpr double CUT_PLANE_MARGIN_MM = 1.0;
}
namespace Slic3r
{
    namespace GUI
    {
        namespace gcode
        {
            const std::vector<Color> BaseRenderer::Extrusion_Role_Colors{ {
                { 0.90f, 0.70f, 0.70f, 1.0f },   // erNone
                { 1.00f, 0.90f, 0.30f, 1.0f },   // erPerimeter
                { 1.00f, 0.49f, 0.22f, 1.0f },   // erExternalPerimeter
                { 0.12f, 0.12f, 1.00f, 1.0f },   // erOverhangPerimeter
                { 0.69f, 0.19f, 0.16f, 1.0f },   // erInternalInfill
                { 0.59f, 0.33f, 0.80f, 1.0f },   // erSolidInfill
                { 0.90f, 0.70f, 0.70f, 1.0f },   // erFloatingVerticalShell
                { 0.94f, 0.25f, 0.25f, 1.0f },   // erTopSolidInfill
                { 0.40f, 0.36f, 0.78f, 1.0f },   // erBottomSurface
                { 1.00f, 0.55f, 0.41f, 1.0f },   // erIroning
                { 0.30f, 0.50f, 0.73f, 1.0f },   // erBridgeInfill
                { 1.00f, 1.00f, 1.00f, 1.0f },   // erGapFill
                { 0.00f, 0.53f, 0.43f, 1.0f },   // erSkirt
                { 0.00f, 0.23f, 0.43f, 1.0f },   // erBrim
                { 0.00f, 1.00f, 0.00f, 1.0f },   // erSupportMaterial
                { 0.00f, 0.50f, 0.00f, 1.0f },   // erSupportMaterialInterface
                { 0.00f, 0.25f, 0.00f, 1.0f },   // erSupportTransition
                { 0.60f, 1.00f, 0.60f, 1.0f },   // erSupportIroning
                { 0.70f, 0.89f, 0.67f, 1.0f },   // erWipeTower
                { 0.37f, 0.82f, 0.58f, 1.0f },    // erCustom
                { 0.85f, 0.65f, 0.95f, 1.0f },   // erFlush
                { 0.00f, 0.85f, 1.00f, 1.0f }    // erScarfSeam (Orio : cyan, en face de l'orange des parois)
            } };
            const std::vector<Color> BaseRenderer::Options_Colors{ {
                { 0.803f, 0.135f, 0.839f, 1.0f },   // Retractions
                { 0.287f, 0.679f, 0.810f, 1.0f },   // Unretractions
                { 0.900f, 0.900f, 0.900f, 1.0f },   // Seams
                { 0.758f, 0.744f, 0.389f, 1.0f },   // ToolChanges
                { 0.856f, 0.582f, 0.546f, 1.0f },   // ColorChanges
                { 0.322f, 0.942f, 0.512f, 1.0f },   // PausePrints
                { 0.886f, 0.825f, 0.262f, 1.0f }    // CustomGCodes
            } };
            const std::vector<Color> BaseRenderer::Travel_Colors{ {
                { 0.219f, 0.282f, 0.609f, 1.0f }, // Move
                { 0.112f, 0.422f, 0.103f, 1.0f }, // Extrude
                { 0.505f, 0.064f, 0.028f, 1.0f }  // Retract
            } };
            // Normal ranges
            // blue to red
            // Normal ranges
            const std::vector<ColorRGBA> BaseRenderer::Range_Colors{ {
                decode_color_to_float_array("#FF00FF"),  // bluish
                decode_color_to_float_array("#FF55A9"),
                decode_color_to_float_array("#FE8778"),
                decode_color_to_float_array("#FFB847"),
                decode_color_to_float_array("#FFD925"),
                decode_color_to_float_array("#FFFF00"),
                decode_color_to_float_array("#D8FF00"),
                decode_color_to_float_array("#ADFF04"),
                decode_color_to_float_array("#76FF01"),
                decode_color_to_float_array("#00FF00")    // reddish
            } };
            // Orio : vue "Fusion de couche" : rouge (rien dessous) vers le bleu Orio (pose sur toute sa largeur)
            const std::vector<ColorRGBA> BaseRenderer::Fusion_Colors{ {
                decode_color_to_float_array("#E5383B"),
                decode_color_to_float_array("#D9436F"),
                decode_color_to_float_array("#BE52A3"),
                decode_color_to_float_array("#9361CF"),
                decode_color_to_float_array("#5B6CE6"),
                decode_color_to_float_array("#0874E0")
            } };

            //const std::vector<LegacyRenderer::Color> LegacyRenderer::Range_Colors {{
            //    {0.043f, 0.173f, 0.478f, 1.0f}, // bluish
            //    {0.075f, 0.349f, 0.522f, 1.0f},
            //    {0.110f, 0.533f, 0.569f, 1.0f},
            //    {0.016f, 0.839f, 0.059f, 1.0f},
            //    {0.667f, 0.949f, 0.000f, 1.0f},
            //    {0.988f, 0.975f, 0.012f, 1.0f},
            //    {0.961f, 0.808f, 0.039f, 1.0f},
            //    //{0.890f, 0.533f, 0.125f, 1.0f},
            //    {0.820f, 0.408f, 0.188f, 1.0f},
            //    {0.761f, 0.322f, 0.235f, 1.0f},
            //    {0.581f, 0.149f, 0.087f, 1.0f} // reddish
            //}};
            const Color BaseRenderer::Wipe_Color = { 1.0f, 1.0f, 0.0f, 1.0f };
            const Color BaseRenderer::Neutral_Color = { 0.25f, 0.25f, 0.25f, 1.0f };

            // Orio : cle d'une valeur de la legende ("role.4.time", "fil.-1.total"...), commune a son
            // affichage (value_text) et a legend_value_cells(), pour comparer deux tranchages
            static std::string legend_cell_key(const char* group, int id, const char* field)
            {
                return std::string(group) + "." + std::to_string(id) + "." + field;
            }

            // Orio : ce que la legende affiche d'un tranchage, mis de cote pour le bouton
            // "tranchage precedent"
            struct SliceSnapshot
            {
                unsigned int result_id{ 0 };
                // heure a laquelle ce tranchage a ete affiche pour la premiere fois
                std::string time_label;
                // temps et filament par role / par extrudeur
                PrintEstimatedStatistics print_statistics;
                // totaux du plateau : filament, poids, cout
                PrintStatistics print_stats;
                std::vector<ExtrusionRole> roles;
                std::vector<unsigned char> extruder_ids;
                std::vector<float> filament_diameters;
                std::vector<float> filament_densities;
                ETools tools;
                Ranges ranges;
                double timelapse_time{ 0.0 };
            };

            struct SliceHistory
            {
                struct PlateSlices
                {
                    std::unique_ptr<SliceSnapshot> last;
                    std::unique_ptr<SliceSnapshot> previous;
                };
                // par indice de plateau : tranchage affiche et celui d'avant
                std::map<int, PlateSlices> plates;
                // bouton maintenu a l'image precedente : la legende de cette image montre le tranchage precedent
                bool held{ false };
                // bouton survole a l'image precedente : surbrillance des valeurs qui different
                bool hovered{ false };
                // position du bouton (repere fenetre) quand le tranchage courant est affiche
                ImVec2 button_pos{ 0.0f, 0.0f };
                // abscisse du bouton de repli avec le titre normal (repere fenetre), reprise sous le titre du
                // tranchage precedent pour ne pas elargir le panneau
                float fold_button_x{ 0.0f };
                // images a redessiner apres un appui ou un relachement (contenu puis taille de la fenetre)
                int extra_frames{ 0 };
            };

            // Orio : ce que la legende affiche pour l'objet clique dans l'Apercu (ses lignes du G-code, entre
            // "start / stop printing object"), et les totaux du plateau par type de ligne pour la colonne "Part du plateau"
            struct PickedStats
            {
                unsigned int result_id{ 0 };
                int label{ -1 };
                std::string name;
                PrintEstimatedStatistics stats;
                PrintStatistics ps;
                // part de la piece dans le temps du plateau, par type de ligne et pour les trajets
                std::map<ExtrusionRole, float> time_share;
                float travel_share{ 0.0f };
                // types de ligne de la piece, a la place de ceux du plateau dans la legende
                std::vector<ExtrusionRole> roles;
            };

            BaseRenderer::BaseRenderer()
            {
                m_moves_slider = new IMSlider(0, 0, 0, 100, wxSL_HORIZONTAL);
                m_layers_slider = new IMSlider(0, 0, 0, 100, wxSL_VERTICAL);
                // Orio : curseurs verticaux de coupe X et Y (mode simplifie)
                m_x_slider = new IMSlider(0, 0, 0, 100, wxSL_VERTICAL);
                m_x_slider->set_as_cut_slider("X");
                m_y_slider = new IMSlider(0, 0, 0, 100, wxSL_VERTICAL);
                m_y_slider->set_as_cut_slider("Y");
                m_p_extrusions = std::make_shared<Extrusions>();
                m_p_extrusions->reset_role_visibility_flags();
                if (GUI::wxGetApp().app_config->get_bool("enable_record_gcodeviewer_option_item")) {
                    auto back_gcodeviewer_option_item = wxGetApp().app_config->get("gcodeviewer_option_item");
                    if (!back_gcodeviewer_option_item.empty()) {
                        m_last_option_item = std::atoi(back_gcodeviewer_option_item.c_str());
                    }
                }
                //    m_sequential_view.skip_invisible_moves = true;
            }

            BaseRenderer::~BaseRenderer()
            {
                if (m_moves_slider) {
                    delete m_moves_slider;
                    m_moves_slider = nullptr;
                }
                if (m_layers_slider) {
                    delete m_layers_slider;
                    m_layers_slider = nullptr;
                }
                if (m_x_slider) {
                    delete m_x_slider;
                    m_x_slider = nullptr;
                }
                if (m_y_slider) {
                    delete m_y_slider;
                    m_y_slider = nullptr;
                }
            }

            void BaseRenderer::init(ConfigOptionMode mode, PresetBundle* preset_bundle)
            {
                // BBS initialzed view_type items
                m_user_mode = mode;
                update_by_mode(m_user_mode);
                // initializes tool marker
                init_tool_maker(preset_bundle);
                m_layers_slider->init_texture();

                if (preset_bundle)
                    m_nozzle_nums = preset_bundle->get_printer_extruder_count();
                // set to color print by default if use multi extruders
                update_default_view_type();
            }

            bool BaseRenderer::is_legend_enabled() const
            {
                return m_legend_enabled;
            }

            void BaseRenderer::enable_legend(bool enable)
            {
                m_legend_enabled = enable;
            }

            float BaseRenderer::get_legend_height() const
            {
                return m_legend_height;
            }

            const Shells& BaseRenderer::get_shells() const
            {
                return m_shells;
            }

            const GCodeCheckResult& BaseRenderer::get_gcode_check_result() const
            {
                return m_gcode_check_result;
            }

            const FilamentPrintableResult& BaseRenderer::get_filament_printable_result() const
            {
                return filament_printable_reuslt;
            }

            const ConflictResultOpt& BaseRenderer::get_conflict_result() const
            {
                return m_conflict_result;
            }

            size_t BaseRenderer::get_extruders_count() const
            {
                return m_extruders_count;
            }

            bool BaseRenderer::has_data() const
            {
                return !m_roles.empty();
            }

            const float BaseRenderer::get_max_print_height() const
            {
                return m_max_print_height;
            }

            const BoundingBoxf3& BaseRenderer::get_paths_bounding_box() const
            {
                return m_paths_bounding_box;
            }

            const BoundingBoxf3& BaseRenderer::get_max_bounding_box() const
            {
                return m_max_bounding_box;
            }

            const BoundingBoxf3& BaseRenderer::get_shell_bounding_box() const
            {
                return m_shell_bounding_box;
            }

            bool BaseRenderer::is_contained_in_bed() const
            {
                return m_contained_in_bed;
            }

            EViewType BaseRenderer::get_view_type() const
            {
                return m_view_type;
            }

            std::vector<CustomGCode::Item>& BaseRenderer::get_custom_gcode_per_print_z()
            {
                return m_custom_gcode_per_print_z;
            }

            void BaseRenderer::update_shells_color_by_extruder(const DynamicPrintConfig* config)
            {
                if (config != nullptr) {
                    m_config = config;
                    m_shells.volumes.update_colors_by_extruder(config, false);
                }
            }

            void BaseRenderer::load_shells(const Print& print, bool initialized, bool force_previewing)
            {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": initialized=%1%, force_previewing=%2%") % initialized % force_previewing;
                if ((print.id().id == m_shells.print_id) && (print.get_modified_count() == m_shells.print_modify_count)) {
                    //BBS: update force previewing logic
                    if (force_previewing)
                        m_shells.previewing = force_previewing;
                    //already loaded
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": already loaded, print=%1% print_id=%2%, print_modify_count=%3%, force_previewing %4%") % (&print) % m_shells.print_id % m_shells.print_modify_count % force_previewing;
                    return;
                }
                //reset shell firstly
                reset_shell();
                //BBS: move behind of reset_shell, to clear previous shell for empty plate
                if (print.objects().empty()) {
                    // no shells, return
                    return;
                }
                // adds objects' volumes
                // BBS: fix the issue that object_idx is not assigned as index of Model.objects array
                int object_count = 0;
                const ModelObjectPtrs& model_objs = wxGetApp().model().objects;
                bool enable_lod = GUI::wxGetApp().app_config->get("enable_lod") == "true";
                for (const PrintObject* obj : print.objects()) {
                    const ModelObject* model_obj = obj->model_object();
                    int object_idx = -1;
                    for (int idx = 0; idx < model_objs.size(); idx++) {
                        if (model_objs[idx]->id() == model_obj->id()) {
                            object_idx = idx;
                            break;
                        }
                    }
                    // BBS: object may be deleted when this method is called when deleting an object
                    if (object_idx == -1)
                        continue;
                    std::vector<int> instance_ids(model_obj->instances.size());
                    //BBS: only add the printable instance
                    int instance_index = 0;
                    for (int i = 0; i < (int)model_obj->instances.size(); ++i) {
                        //BBS: only add the printable instance
                        if (model_obj->instances[i]->is_printable())
                            instance_ids[instance_index++] = i;
                    }
                    instance_ids.resize(instance_index);
                    size_t current_volumes_count = m_shells.volumes.volumes.size();
                    m_shells.volumes.load_object(model_obj, object_idx, instance_ids, "object", initialized, enable_lod);
                    // adjust shells' z if raft is present
                    const SlicingParameters& slicing_parameters = obj->slicing_parameters();
                    if (slicing_parameters.object_print_z_min != 0.0) {
                        const Vec3d z_offset = slicing_parameters.object_print_z_min * Vec3d::UnitZ();
                        for (size_t i = current_volumes_count; i < m_shells.volumes.volumes.size(); ++i) {
                            GLVolume* v = m_shells.volumes.volumes[i];
                            auto offset = v->get_instance_transformation().get_matrix_no_offset().inverse() * z_offset;
                            v->set_volume_offset(v->get_volume_offset() + offset);
                        }
                    }
                    object_count++;
                }
                if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF) {
                    // BBS: adds wipe tower's volume
                    std::vector<unsigned int> print_extruders = print.extruders(true);
                    int extruders_count = print_extruders.size();
                    const double max_z = print.objects()[0]->model_object()->get_model()->bounding_box().max(2);
                    const PrintConfig& config = print.config();
                    if (config.enable_prime_tower &&
                        (print.enable_timelapse_print() || (extruders_count > 1 && (config.print_sequence == PrintSequence::ByLayer)))) {
                        const float depth = print.wipe_tower_data(extruders_count).depth;
                        const float brim_width = print.wipe_tower_data(extruders_count).brim_width;
                        int plate_idx = print.get_plate_index();
                        Vec3d plate_origin = print.get_plate_origin();
                        double wipe_tower_x = config.wipe_tower_x.get_at(plate_idx) + plate_origin(0);
                        double wipe_tower_y = config.wipe_tower_y.get_at(plate_idx) + plate_origin(1);
                        m_shells.volumes.load_wipe_tower_preview(1000, wipe_tower_x, wipe_tower_y, config.prime_tower_width, depth, max_z, config.wipe_tower_rotation_angle,
                            !print.is_step_done(psWipeTower), brim_width, initialized);
                    }
                }
                // remove modifiers
                while (true) {
                    GLVolumePtrs::iterator it = std::find_if(m_shells.volumes.volumes.begin(), m_shells.volumes.volumes.end(), [](GLVolume* volume) { return volume->is_modifier; });
                    if (it != m_shells.volumes.volumes.end()) {
                        m_shells.volumes.release_volume(*it);
                        delete (*it);
                        m_shells.volumes.volumes.erase(it);
                    }
                    else
                        break;
                }
                for (GLVolume* volume : m_shells.volumes.volumes) {
                    volume->zoom_to_volumes = false;
                    volume->color[3] = 0.5f;
                    volume->force_native_color = true;
                    volume->set_render_color();
                    //BBS: add shell bounding box logic
                    m_shell_bounding_box.merge(volume->transformed_bounding_box());
                }
                //BBS: always load shell when preview
                m_shells.print_id = print.id().id;
                m_shells.print_modify_count = print.get_modified_count();
                m_shells.previewing = true;
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": shell loaded, id change to %1%, modify_count %2%, object count %3%, glvolume count %4%")
                    % m_shells.print_id % m_shells.print_modify_count % object_count % m_shells.volumes.volumes.size();
            }

            void BaseRenderer::set_shells_on_preview(bool is_previewing)
            {
                if (is_previewing) {
                    delete_wipe_tower();
                }
                m_shells.previewing = is_previewing;
            }

            const std::array<unsigned int, 2>& BaseRenderer::get_layers_z_range() const
            {
                return m_layers_z_range;
            }

            void BaseRenderer::toggle_gcode_window_visibility()
            {
                const auto& p_sequential_view = get_sequential_view();
                if (p_sequential_view) {
                    p_sequential_view->gcode_window.toggle_visibility();
                }
            }

            const std::shared_ptr<SequentialView>& BaseRenderer::get_sequential_view() const
            {
                if (!m_p_sequential_view) {
                    m_p_sequential_view = std::make_shared<SequentialView>();
                }
                return m_p_sequential_view;
            }

            void BaseRenderer::on_change_color_mode(bool is_dark)
            {
                m_is_dark = is_dark;
                const auto& p_sequential_view = get_sequential_view();
                if (p_sequential_view) {
                    p_sequential_view->marker.on_change_color_mode(m_is_dark);
                    p_sequential_view->gcode_window.on_change_color_mode(m_is_dark);
                }
            }

            void BaseRenderer::set_scale(float scale)
            {
                if (m_scale != scale)m_scale = scale;
                const auto& p_sequential_view = get_sequential_view();
                if (p_sequential_view && p_sequential_view->m_scale != scale) {
                    p_sequential_view->m_scale = scale;
                    p_sequential_view->marker.m_scale = scale;
                }
            }

            void BaseRenderer::update_marker_curr_move()
            {
                const auto& p_sequential_view = get_sequential_view();
                if (!p_sequential_view) {
                    return;
                }
                if ((int)m_last_result_id != -1) {
                    auto it = std::find_if(m_gcode_result->moves.begin(), m_gcode_result->moves.end(), [this, &p_sequential_view](auto move) {
                        if (p_sequential_view->current.last < p_sequential_view->gcode_ids.size() && p_sequential_view->current.last >= 0) {
                            return move.gcode_id == static_cast<uint64_t>(p_sequential_view->gcode_ids[p_sequential_view->current.last]);
                        }
                        return false;
                        });
                    if (it != m_gcode_result->moves.end())
                        p_sequential_view->marker.update_curr_move(*it);
                }
            }

            void BaseRenderer::load(const GCodeProcessorResult& gcode_result, const Print& print, const BuildVolume& build_volume, const std::vector<BoundingBoxf3>& exclude_bounding_box, bool initialized, ConfigOptionMode mode, bool only_gcode)
            {
                // avoid processing if called with the same gcode_result
                if (m_last_result_id == gcode_result.id) {
                    //BBS: add logs
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": the same id %1%, return directly, result %2% ") % m_last_result_id % (&gcode_result);
                    return;
                }
                //BBS: add logs
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": gcode result %1%, new id %2%, gcode file %3% ") % (&gcode_result) % m_last_result_id % PathSanitizer::sanitize(gcode_result.filename);
                // release gpu memory, if used
                reset();
                //BBS: add mutex for protection of gcode result
                wxGetApp().plater()->suppress_background_process(true);
                gcode_result.lock();
                //BBS: add safe check
                if (gcode_result.moves.size() == 0) {
                    //result cleaned before slicing ,should return here
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result reset before, return directly!");
                    gcode_result.unlock();
                    wxGetApp().plater()->schedule_background_process();
                    return;
                }
                //BBS: move the id to the end of reset
                m_last_result_id = gcode_result.id;
                m_gcode_result = &gcode_result;
                m_only_gcode_in_preview = only_gcode;
                const auto& p_sequential_view = get_sequential_view();
                if (p_sequential_view) {
                    p_sequential_view->gcode_window.load_gcode(gcode_result.filename, gcode_result.lines_ends);
                }
                //BBS: add only gcode mode
                //if (wxGetApp().is_gcode_viewer())
                if (m_only_gcode_in_preview)
                    m_custom_gcode_per_print_z = gcode_result.custom_gcode_per_print_z;
                m_max_print_height = gcode_result.printable_height;
                bool rt = load_toolpaths(gcode_result, build_volume, exclude_bounding_box);
                //BBS: add mutex for protection of gcode result
                if (!rt) {
                    gcode_result.unlock();
                    wxGetApp().plater()->schedule_background_process();
                    return;
                }
                // BBS: data for rendering color arrangement recommendation
                m_nozzle_nums = print.config().option<ConfigOptionFloatsNullable>("nozzle_diameter")->values.size();
                std::vector<int>         filament_maps = print.get_filament_maps();
                std::vector<std::string> color_opt = print.config().option<ConfigOptionStrings>("filament_colour")->values;
                std::vector<std::string> type_opt = print.config().option<ConfigOptionStrings>("filament_type")->values;
                std::vector<unsigned char> support_filament_opt = print.config().option<ConfigOptionBools>("filament_is_support")->values;
                for (auto extruder_id : m_extruder_ids) {
                    if (filament_maps[extruder_id] == 1) {
                        m_left_extruder_filament.push_back({ type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id]) });
                    }
                    else {
                        m_right_extruder_filament.push_back({ type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id]) });
                    }
                }
                m_settings_ids = gcode_result.settings_ids;
                m_filament_diameters = gcode_result.filament_diameters;
                m_filament_densities = gcode_result.filament_densities;

                //BBS: add only gcode mode
                if (m_only_gcode_in_preview) {
                    Pointfs printable_area;
                    //BBS: add bed exclude area
                    Pointfs bed_exclude_area = Pointfs();
                    Pointfs wrapping_exclude_area = Pointfs();
                    std::vector<Pointfs> extruder_areas;
                    std::vector<double> extruder_heights;
                    std::string texture;
                    std::string model;
                    if (!gcode_result.printable_area.empty()) {
                        // bed shape detected in the gcode
                        printable_area = gcode_result.printable_area;
                        const auto bundle = wxGetApp().preset_bundle;
                        if (bundle != nullptr && !m_settings_ids.printer.empty()) {
                            const Preset* preset = bundle->printers.find_preset(m_settings_ids.printer);
                            if (preset != nullptr) {
                                model = PresetUtils::system_printer_bed_model(*preset);
                                texture = PresetUtils::system_printer_bed_texture(*preset);
                            }
                        }
                        //BBS: add bed exclude area
                        if (!gcode_result.bed_exclude_area.empty())
                            bed_exclude_area = gcode_result.bed_exclude_area;
                        if (!gcode_result.wrapping_exclude_area.empty())
                            wrapping_exclude_area = gcode_result.wrapping_exclude_area;
                        if (!gcode_result.extruder_areas.empty())
                            extruder_areas = gcode_result.extruder_areas;
                        if (!gcode_result.extruder_heights.empty())
                            extruder_heights = gcode_result.extruder_heights;
                        wxGetApp().plater()->set_bed_shape(printable_area, bed_exclude_area, wrapping_exclude_area, gcode_result.printable_height, extruder_areas, extruder_heights, texture, model, gcode_result.printable_area.empty());
                    }
                    /*else {
                        // adjust printbed size in dependence of toolpaths bbox
                        const double margin = 10.0;
                        const Vec2d min(m_paths_bounding_box.min.x() - margin, m_paths_bounding_box.min.y() - margin);
                        const Vec2d max(m_paths_bounding_box.max.x() + margin, m_paths_bounding_box.max.y() + margin);
                        const Vec2d size = max - min;
                        printable_area = {
                            { min.x(), min.y() },
                            { max.x(), min.y() },
                            { max.x(), min.y() + 0.442265 * size.y()},
                            { max.x() - 10.0, min.y() + 0.4711325 * size.y()},
                            { max.x() + 10.0, min.y() + 0.5288675 * size.y()},
                            { max.x(), min.y() + 0.557735 * size.y()},
                            { max.x(), max.y() },
                            { min.x() + 0.557735 * size.x(), max.y()},
                            { min.x() + 0.5288675 * size.x(), max.y() - 10.0},
                            { min.x() + 0.4711325 * size.x(), max.y() + 10.0},
                            { min.x() + 0.442265 * size.x(), max.y()},
                            { min.x(), max.y() } };
                    }*/
                }
                else if (&wxGetApp() && wxGetApp().app_config->get_bool("show_shells_in_preview")) { // BBS: load shells
                    load_shells(print, initialized,true);
                    update_shells_color_by_extruder(m_config);
                }
                m_print_statistics = gcode_result.print_statistics;
                if (m_time_estimate_mode != PrintEstimatedStatistics::ETimeMode::Normal) {
                    const float time = m_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)].time;
                    if (time == 0.0f ||
                        short_time(get_time_dhms(time)) == short_time(get_time_dhms(m_print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time)))
                        m_time_estimate_mode = PrintEstimatedStatistics::ETimeMode::Normal;
                }
                if (&wxGetApp() && !wxGetApp().app_config->get_bool("use_last_fold_state_gcodeview_option_panel")) {
                    m_fold = false;
                }

                bool only_gcode_3mf = false;
                PartPlate* current_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
                bool current_has_print_instances = current_plate->has_printable_instances();
                if (current_plate->is_slice_result_valid() && wxGetApp().model().objects.empty() && !current_has_print_instances)
                    only_gcode_3mf = true;
                m_layers_slider->set_menu_enable(!(only_gcode || only_gcode_3mf));
                m_layers_slider->set_as_dirty();
                m_moves_slider->set_as_dirty();
                //BBS
                m_gcode_check_result = gcode_result.gcode_check_result;
                filament_printable_reuslt = gcode_result.filament_printable_reuslt;
                //BBS: add mutex for protection of gcode result
                gcode_result.unlock();
                wxGetApp().plater()->schedule_background_process();
            }

            void BaseRenderer::set_view_type(EViewType type, bool reset_feature_type_visible)
            {
                if (type == EViewType::Count)
                    type = EViewType::FeatureType;
                do_set_view_type(type);
                if (reset_feature_type_visible && type == EViewType::ColorPrint) {
                    reset_visible(EViewType::FeatureType);
                }
            }

            void BaseRenderer::reset_visible(EViewType type)
            {
                if (!m_p_extrusions) {
                    return;
                }
                if (type == EViewType::FeatureType) {
                    for (size_t i = 0; i < m_roles.size(); ++i) {
                        set_extrusion_role_visible(m_roles[i], true);
                    }
                }
                else if (type == EViewType::ColorPrint) {
                    for (auto item : m_tools.m_tool_visibles) item = true;
                }
            }

            bool BaseRenderer::is_only_gcode_in_preview() const
            {
                return m_only_gcode_in_preview;
            }

            IMSlider* BaseRenderer::get_moves_slider()
            {
                return m_moves_slider;
            }

            IMSlider* BaseRenderer::get_layers_slider()
            {
                return m_layers_slider;
            }

            IMSlider* BaseRenderer::get_x_slider()
            {
                return m_x_slider;
            }

            IMSlider* BaseRenderer::get_y_slider()
            {
                return m_y_slider;
            }

            // Orio : renseigne les bornes des curseurs de coupe X/Y depuis l'etendue
            // (bounding box) des trajectoires, et y reprend la derniere coupe choisie a la main.
            void BaseRenderer::update_xy_sliders()
            {
                if (!m_x_slider || !m_y_slider)
                    return;

                // Cotes affichees en coordonnees du plateau courant, comme la fenetre de position de
                // l'outil : les trajets sont en coordonnees monde (origine du plateau incluse, voir
                // GCodeProcessor::set_xy_offset dans BackgroundSlicingProcess).
                Vec3d plate_origin = Vec3d::Zero();
                if (PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate())
                    plate_origin = plate->get_origin();
                m_x_slider->set_cut_label_offset(plate_origin.x());
                m_y_slider->set_cut_label_offset(plate_origin.y());

                const BoundingBoxf3& bb = m_paths_bounding_box;
                const bool valid = bb.defined && bb.max.x() > bb.min.x() && bb.max.y() > bb.min.y();
                if (!valid)
                    return;

                // Ne reinitialiser (et effacer une coupe en cours) que si l'etendue change vraiment,
                // afin de preserver le reglage de l'utilisateur lors d'un simple changement de vue.
                const bool extent_changed = !m_xy_extent_valid
                    || std::abs(m_xy_extent_x[0] - bb.min.x()) > 1.0e-3
                    || std::abs(m_xy_extent_x[1] - bb.max.x()) > 1.0e-3
                    || std::abs(m_xy_extent_y[0] - bb.min.y()) > 1.0e-3
                    || std::abs(m_xy_extent_y[1] - bb.max.y()) > 1.0e-3;
                if (!extent_changed)
                    return;

                // axis : 0 = X, 1 = Y ; la coupe memorisee (coordonnees plateau) est ramenee en monde
                const XYCutMemory& memory = current_xy_cut_memory();
                auto setup = [&memory](IMSlider* slider, int axis, double lo, double hi, double origin) {
                    if (!(hi > lo))
                        hi = lo + 1.0;
                    // resolution ~0.1 mm, bornee pour rester raisonnable
                    int steps = (int) std::round((hi - lo) / 0.1);
                    steps = std::max(50, std::min(steps, 4000));
                    std::vector<double> values(steps + 1);
                    for (int i = 0; i <= steps; ++i)
                        values[i] = lo + (hi - lo) * (double) i / (double) steps;
                    slider->SetSliderValues(values);
                    slider->SetMaxValue(steps);
                    // Coupe memorisee : chaque borne n'est reprise que si elle tombe strictement dans la
                    // nouvelle etendue (sinon ce cote reste en bout de course, sans oublier la memoire).
                    auto index_of = [lo, hi, steps](double v) {
                        return std::max(0, std::min(steps, (int) std::round((v - lo) / (hi - lo) * (double) steps)));
                    };
                    int idx_low  = 0;
                    int idx_high = steps;
                    const double mem_low  = memory.mm[2 * axis] + origin;
                    const double mem_high = memory.mm[2 * axis + 1] + origin;
                    if (memory.active[2 * axis] && mem_low > lo && mem_low < hi)
                        idx_low = index_of(mem_low);
                    if (memory.active[2 * axis + 1] && mem_high > lo && mem_high < hi)
                        idx_high = index_of(mem_high);
                    if (idx_low > idx_high) {
                        idx_low  = 0;
                        idx_high = steps;
                    }
                    slider->SetSelectionSpan(idx_low, idx_high);
                    slider->set_as_dirty();
                };

                setup(m_x_slider, 0, bb.min.x(), bb.max.x(), plate_origin.x());
                setup(m_y_slider, 1, bb.min.y(), bb.max.y(), plate_origin.y());
                m_xy_extent_x = { bb.min.x(), bb.max.x() };
                m_xy_extent_y = { bb.min.y(), bb.max.y() };
                m_xy_extent_valid = true;
                // La coupe n'est appliquee qu'au rendu suivant : les curseurs marques « modifies » font
                // appeler set_xy_clip_range() par GLCanvas3D::_render_gcode, avec la coupe reprise.
                m_clip_xy = { -CLIP_XY_FAR, CLIP_XY_FAR, -CLIP_XY_FAR, CLIP_XY_FAR };
                m_clip_xy_mm = { bb.min.x(), bb.max.x(), bb.min.y(), bb.max.y() };
                m_clip_bound_active = { false, false, false, false };
                m_clip_xy_enabled = false;
            }

            // Orio : memorise la plage de coupe X/Y appliquee ensuite au rendu.
            void BaseRenderer::set_xy_clip_range(double x_min, double x_max, double y_min, double y_max, bool enabled)
            {
                m_clip_xy_mm = { x_min, x_max, y_min, y_max };
                // Une borne ne coupe que si son curseur a quitte le bout de l'etendue des trajectoires.
                // Les autres sont repoussees tres loin cote shader : elles ne rejettent rien (pas meme
                // la demi-largeur des parois exterieures, hors de la boite des lignes medianes).
                const double eps = 1.0e-4;
                m_clip_bound_active = {
                    m_xy_extent_valid && x_min > m_xy_extent_x[0] + eps,
                    m_xy_extent_valid && x_max < m_xy_extent_x[1] - eps,
                    m_xy_extent_valid && y_min > m_xy_extent_y[0] + eps,
                    m_xy_extent_valid && y_max < m_xy_extent_y[1] - eps };
                m_clip_xy = {
                    m_clip_bound_active[0] ? (float) x_min : -CLIP_XY_FAR,
                    m_clip_bound_active[1] ? (float) x_max : CLIP_XY_FAR,
                    m_clip_bound_active[2] ? (float) y_min : -CLIP_XY_FAR,
                    m_clip_bound_active[3] ? (float) y_max : CLIP_XY_FAR };
                const bool any_active = m_clip_bound_active[0] || m_clip_bound_active[1] || m_clip_bound_active[2] || m_clip_bound_active[3];
                m_clip_xy_enabled = enabled && any_active;
            }

            // Orio : transmet l'etat de coupe X/Y au shader courant.
            // Sans effet si le shader ne declare pas ces uniforms (set_uniform ignore alors l'appel).
            void BaseRenderer::apply_xy_clip_uniforms(GLShaderProgram& shader) const
            {
                shader.set_uniform("u_clip_xy_enabled", m_clip_xy_enabled ? 1.0f : 0.0f);
                shader.set_uniform("u_clip_xy", m_clip_xy);
                // gouraud_light : les trajectoires Legacy sont deja en coordonnees monde (les coques,
                // elles, recoivent leur matrice volume par volume dans GLVolumeCollection::render).
                shader.set_uniform("volume_world_matrix", Transform3d::Identity());
                shader.set_uniform("u_clip_object", float(layer_fusion_clip_label() + 1));
                shader.set_uniform("u_clip_others_only", 0.0f);
            }

            int BaseRenderer::layer_fusion_clip_label() const
            {
                if (m_view_type != EViewType::LayerFusion || m_picked_label < 0)
                    return -1;
                const ModelObjectPtrs& objects = wxGetApp().plater()->model().objects;
                for (const GLVolume* v : m_shells.volumes.volumes) {
                    const int o = v->object_idx(), i = v->instance_idx();
                    if (!v->is_wipe_tower && o >= 0 && o < int(objects.size()) && i >= 0 && i < int(objects[o]->instances.size()) &&
                        int(objects[o]->instances[i]->get_labeled_id()) == m_picked_label)
                        return m_picked_label;
                }
                return -1;
            }

            void BaseRenderer::reset_shell()
            {
                m_shells.reset();
                m_shell_bounding_box.reset();
            }

            //BBS
            void BaseRenderer::render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show) const
            {
                if (!show)
                    return;
                for (auto gcode_result : gcode_result_list) {
                    if (gcode_result->moves.size() == 0)
                        return;
                }
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0, 10.0 * m_scale));
                ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(340.f * m_scale * imgui.scaled(1.0f / 15.0f), 0));
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), 0, ImVec2(0.5f, 0.5f));
                ImGui::Begin(_L("Statistics of All Plates").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                std::vector<float> filament_diameters = gcode_result_list.front()->filament_diameters;
                std::vector<float> filament_densities = gcode_result_list.front()->filament_densities;
                std::vector<Color> filament_colors = ::decode_colors(wxGetApp().plater()->get_extruder_colors_from_plater_config(gcode_result_list.back()));
                for (int i = 0; i < filament_colors.size(); i++) {
                    filament_colors[i] = adjust_color_for_rendering(filament_colors[i]);
                }
                bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
                float window_padding = 4.0f * m_scale;
                const float icon_size = ImGui::GetTextLineHeight() * 0.7;
                std::map<std::string, float> fil_table_offsets;
                std::map<std::string, float> time_est_table_offsets;
                std::map<int, double> model_volume_of_extruders_all_plates; // map<extruder_idx, volume>
                std::map<int, double> flushed_volume_of_extruders_all_plates; // map<extruder_idx, flushed volume>
                std::map<int, double> wipe_tower_volume_of_extruders_all_plates; // map<extruder_idx, flushed volume>
                std::map<int, double> support_volume_of_extruders_all_plates; // map<extruder_idx, flushed volume>
                std::map<int, double> plate_time; // map<plate_idx, time>
                std::vector<double> model_used_filaments_m_all_plates;
                std::vector<double> model_used_filaments_g_all_plates;
                std::vector<double> flushed_filaments_m_all_plates;
                std::vector<double> flushed_filaments_g_all_plates;
                std::vector<double> wipe_tower_used_filaments_m_all_plates;
                std::vector<double> wipe_tower_used_filaments_g_all_plates;
                std::vector<double> support_used_filaments_m_all_plates;
                std::vector<double> support_used_filaments_g_all_plates;
                float total_time_all_plates = 0.0f;
                float total_cost_all_plates = 0.0f;
                double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1.0;
                struct ColumnData {
                    enum {
                        Model = 1,
                        Flushed = 2,
                        WipeTower = 4,
                        Support = 1 << 3,
                    };
                };
                int displayed_columns = 0;
                auto max_width = [](const std::vector<std::string>& items, const std::string& title, float extra_size = 0.0f) {
                    float ret = ImGui::CalcTextSize(title.c_str()).x;
                    for (const std::string& item : items) {
                        ret = std::max(ret, extra_size + ImGui::CalcTextSize(item.c_str()).x);
                    }
                    return ret;
                    };
                auto calculate_offsets = [max_width, window_padding](const std::vector<std::pair<std::string, std::vector<::string>>>& title_columns, float extra_size = 0.0f) {
                    const ImGuiStyle& style = ImGui::GetStyle();
                    std::vector<float> offsets;
                    offsets.push_back(max_width(title_columns[0].second, title_columns[0].first, extra_size) + 3.0f * style.ItemSpacing.x + style.WindowPadding.x);
                    for (size_t i = 1; i < title_columns.size() - 1; i++)
                        offsets.push_back(offsets.back() + max_width(title_columns[i].second, title_columns[i].first) + style.ItemSpacing.x);
                    if (title_columns.back().first == _u8L("Display"))
                        offsets.back() = ImGui::GetWindowWidth() - ImGui::CalcTextSize(_u8L("Display").c_str()).x - ImGui::GetFrameHeight() / 2 - 2 * window_padding;
                    float average_col_width = ImGui::GetWindowWidth() / static_cast<float>(title_columns.size());
                    std::vector<float> ret;
                    ret.push_back(0);
                    for (size_t i = 1; i < title_columns.size(); i++) {
                        ret.push_back(std::max(offsets[i - 1], i * average_col_width));
                    }
                    return ret;
                    };
                auto append_item = [icon_size, &imgui, imperial_units, &window_padding, &draw_list, this](bool draw_icon, const Color& color, const std::vector<std::pair<std::string, float>>& columns_offsets)
                    {
                        // render icon
                        ImVec2 pos = ImVec2(ImGui::GetCursorScreenPos().x + window_padding * 3, ImGui::GetCursorScreenPos().y);
                        if (draw_icon)
                            draw_list->AddRectFilled({ pos.x + 1.0f * m_scale, pos.y + 3.0f * m_scale }, { pos.x + icon_size - 1.0f * m_scale, pos.y + icon_size + 1.0f * m_scale },
                                ImGui::GetColorU32({ color[0], color[1], color[2], 1.0f }));
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20.0 * m_scale, 6.0 * m_scale));
                        // render selectable
                        ImGui::Dummy({ 0.0, 0.0 });
                        ImGui::SameLine();
                        // render column item
                        {
                            float dummy_size = draw_icon ? ImGui::GetStyle().ItemSpacing.x + icon_size : window_padding * 3;
                            ImGui::SameLine(dummy_size);
                            imgui.text(columns_offsets[0].first);
                            for (auto i = 1; i < columns_offsets.size(); i++) {
                                ImGui::SameLine(columns_offsets[i].second);
                                imgui.text(columns_offsets[i].first);
                            }
                        }
                        ImGui::PopStyleVar(1);
                    };
                auto append_headers = [&imgui](const std::vector<std::pair<std::string, float>>& title_offsets) {
                    for (size_t i = 0; i < title_offsets.size(); i++) {
                        ImGui::SameLine(title_offsets[i].second);
                        imgui.bold_text(title_offsets[i].first);
                    }
                    ImGui::Separator();
                    };
                auto get_used_filament_from_volume = [this, imperial_units, &filament_diameters, &filament_densities](double volume, int extruder_id) {
                    double koef = imperial_units ? 1.0 / GizmoObjectManipulation::in_to_mm : 0.001;
                    std::pair<double, double> ret = { koef * volume / (PI * sqr(0.5 * filament_diameters[extruder_id])),
                                                        volume * filament_densities[extruder_id] * 0.001 };
                    return ret;
                    };
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::SameLine();
                // title and item data
                {
                    PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
                    for (auto plate : plate_list.get_nonempty_plate_list())
                    {
                        auto plate_print_statistics = plate->get_slice_result()->print_statistics;
                        auto plate_extruders = plate->get_extruders(true);
                        auto max_extruders_colors   = wxGetApp().plater()->get_extruders_colors().size();
                        for (size_t extruder_id : plate_extruders) {
                            extruder_id -= 1;
                            if (extruder_id >= max_extruders_colors) {
                                continue;
                            }
                            if (plate_print_statistics.model_volumes_per_extruder.find(extruder_id) == plate_print_statistics.model_volumes_per_extruder.end())
                                model_volume_of_extruders_all_plates[extruder_id] += 0;
                            else {
                                double model_volume = plate_print_statistics.model_volumes_per_extruder.at(extruder_id);
                                model_volume_of_extruders_all_plates[extruder_id] += model_volume;
                            }
                            if (plate_print_statistics.flush_per_filament.find(extruder_id) == plate_print_statistics.flush_per_filament.end())
                                flushed_volume_of_extruders_all_plates[extruder_id] += 0;
                            else {
                                double flushed_volume = plate_print_statistics.flush_per_filament.at(extruder_id);
                                flushed_volume_of_extruders_all_plates[extruder_id] += flushed_volume;
                            }
                            if (plate_print_statistics.wipe_tower_volumes_per_extruder.find(extruder_id) == plate_print_statistics.wipe_tower_volumes_per_extruder.end())
                                wipe_tower_volume_of_extruders_all_plates[extruder_id] += 0;
                            else {
                                double wipe_tower_volume = plate_print_statistics.wipe_tower_volumes_per_extruder.at(extruder_id);
                                wipe_tower_volume_of_extruders_all_plates[extruder_id] += wipe_tower_volume;
                            }
                            if (plate_print_statistics.support_volumes_per_extruder.find(extruder_id) == plate_print_statistics.support_volumes_per_extruder.end())
                                support_volume_of_extruders_all_plates[extruder_id] += 0;
                            else {
                                double support_volume = plate_print_statistics.support_volumes_per_extruder.at(extruder_id);
                                support_volume_of_extruders_all_plates[extruder_id] += support_volume;
                            }
                        }
                        const PrintEstimatedStatistics::Mode& plate_time_mode = plate_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)];
                        plate_time.insert_or_assign(plate->get_index(), plate_time_mode.time);
                        total_time_all_plates += plate_time_mode.time;
                        Print* print;
                        plate->get_print((PrintBase**)&print, nullptr, nullptr);
                        total_cost_all_plates += print->print_statistics().total_cost;
                    }
                    for (auto it = model_volume_of_extruders_all_plates.begin(); it != model_volume_of_extruders_all_plates.end(); it++) {
                        auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(it->second, it->first);
                        if (model_used_filament_m != 0.0 || model_used_filament_g != 0.0)
                            displayed_columns |= ColumnData::Model;
                        model_used_filaments_m_all_plates.push_back(model_used_filament_m);
                        model_used_filaments_g_all_plates.push_back(model_used_filament_g);
                    }
                    for (auto it = flushed_volume_of_extruders_all_plates.begin(); it != flushed_volume_of_extruders_all_plates.end(); it++) {
                        auto [flushed_filament_m, flushed_filament_g] = get_used_filament_from_volume(it->second, it->first);
                        if (flushed_filament_m != 0.0 || flushed_filament_g != 0.0)
                            displayed_columns |= ColumnData::Flushed;
                        flushed_filaments_m_all_plates.push_back(flushed_filament_m);
                        flushed_filaments_g_all_plates.push_back(flushed_filament_g);
                    }
                    for (auto it = wipe_tower_volume_of_extruders_all_plates.begin(); it != wipe_tower_volume_of_extruders_all_plates.end(); it++) {
                        auto [wipe_tower_filament_m, wipe_tower_filament_g] = get_used_filament_from_volume(it->second, it->first);
                        if (wipe_tower_filament_m != 0.0 || wipe_tower_filament_g != 0.0)
                            displayed_columns |= ColumnData::WipeTower;
                        wipe_tower_used_filaments_m_all_plates.push_back(wipe_tower_filament_m);
                        wipe_tower_used_filaments_g_all_plates.push_back(wipe_tower_filament_g);
                    }
                    for (auto it = support_volume_of_extruders_all_plates.begin(); it != support_volume_of_extruders_all_plates.end(); it++) {
                        auto [support_filament_m, support_filament_g] = get_used_filament_from_volume(it->second, it->first);
                        if (support_filament_m != 0.0 || support_filament_g != 0.0)
                            displayed_columns |= ColumnData::Support;
                        support_used_filaments_m_all_plates.push_back(support_filament_m);
                        support_used_filaments_g_all_plates.push_back(support_filament_g);
                    }
                    char buff[64];
                    double longest_str = 0.0;
                    for (auto i : model_used_filaments_g_all_plates) {
                        longest_str += i;
                    }
                    ::sprintf(buff, imperial_units ? "%.2f oz" : "%.2f g", longest_str / unit_conver);
                    std::vector<std::pair<std::string, std::vector<::string>>> title_columns;
                    if (displayed_columns & ColumnData::Model) {
                        title_columns.push_back({ _u8L("Filament"), {""} });
                        title_columns.push_back({ _u8L("Model"), {buff} });
                    }
                    if (displayed_columns & ColumnData::Support) {
                        title_columns.push_back({ _u8L("Support"), {buff} });
                    }
                    if (displayed_columns & ColumnData::Flushed) {
                        title_columns.push_back({ _u8L("Flushed"), {buff} });
                    }
                    if (displayed_columns & ColumnData::WipeTower) {
                        title_columns.push_back({ _u8L("Tower"), {buff} });
                    }
                    if ((displayed_columns & ~ColumnData::Model) > 0) {
                        title_columns.push_back({ _u8L("Total"), {buff} });
                    }
                    auto offsets_ = calculate_offsets(title_columns, icon_size);
                    std::vector<std::pair<std::string, float>> title_offsets;
                    for (int i = 0; i < offsets_.size(); i++) {
                        title_offsets.push_back({ title_columns[i].first, offsets_[i] });
                        fil_table_offsets[title_columns[i].first] = offsets_[i];
                    }
                    append_headers(title_offsets);
                }
                // item
                {
                    size_t i = 0;
                    for (auto it = model_volume_of_extruders_all_plates.begin(); it != model_volume_of_extruders_all_plates.end(); it++) {
                        if (i < model_used_filaments_m_all_plates.size() && i < model_used_filaments_g_all_plates.size()) {
                            std::vector<std::pair<std::string, float>> columns_offsets;
                            columns_offsets.push_back({ std::to_string(it->first + 1), fil_table_offsets[_u8L("Filament")] });
                            char buf[64];
                            float column_sum_m = 0.0f;
                            float column_sum_g = 0.0f;
                            if (displayed_columns & ColumnData::Model) {
                                if ((displayed_columns & ~ColumnData::Model) > 0)
                                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", model_used_filaments_m_all_plates[i], model_used_filaments_g_all_plates[i] / unit_conver);
                                else
                                    ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", model_used_filaments_m_all_plates[i], model_used_filaments_g_all_plates[i] / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Model")] });
                                column_sum_m += model_used_filaments_m_all_plates[i];
                                column_sum_g += model_used_filaments_g_all_plates[i];
                            }
                            if (displayed_columns & ColumnData::Support) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", support_used_filaments_m_all_plates[i], support_used_filaments_g_all_plates[i] / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Support")] });
                                column_sum_m += support_used_filaments_m_all_plates[i];
                                column_sum_g += support_used_filaments_g_all_plates[i];
                            }
                            if (displayed_columns & ColumnData::Flushed) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", flushed_filaments_m_all_plates[i], flushed_filaments_g_all_plates[i] / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Flushed")] });
                                column_sum_m += flushed_filaments_m_all_plates[i];
                                column_sum_g += flushed_filaments_g_all_plates[i];
                            }
                            if (displayed_columns & ColumnData::WipeTower) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", wipe_tower_used_filaments_m_all_plates[i], wipe_tower_used_filaments_g_all_plates[i] / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Tower")] });
                                column_sum_m += wipe_tower_used_filaments_m_all_plates[i];
                                column_sum_g += wipe_tower_used_filaments_g_all_plates[i];
                            }
                            if ((displayed_columns & ~ColumnData::Model) > 0) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", column_sum_m, column_sum_g / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Total")] });
                            }
                            append_item(true, filament_colors[it->first], columns_offsets);
                        }
                        i++;
                    }
                    // Sum of all rows
                    char buf[64];
                    if (model_volume_of_extruders_all_plates.size() > 1) {
                        // Separator
                        ImGuiWindow* window = ImGui::GetCurrentWindow();
                        const ImRect separator(ImVec2(window->Pos.x + window_padding * 3, window->DC.CursorPos.y),
                            ImVec2(window->Pos.x + window->Size.x - window_padding * 3, window->DC.CursorPos.y + 1.0f));
                        ImGui::ItemSize(ImVec2(0.0f, 0.0f));
                        const bool item_visible = ImGui::ItemAdd(separator, 0);
                        window->DrawList->AddLine(separator.Min, ImVec2(separator.Max.x, separator.Min.y), ImGui::GetColorU32(ImGuiCol_Separator));
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({ _u8L("Total"), fil_table_offsets[_u8L("Filament")] });
                        double total_model_used_filament_m = 0;
                        double total_model_used_filament_g = 0;
                        double total_support_used_filament_m = 0;
                        double total_support_used_filament_g = 0;
                        double total_flushed_filament_m = 0;
                        double total_flushed_filament_g = 0;
                        double total_wipe_tower_used_filament_m = 0;
                        double total_wipe_tower_used_filament_g = 0;
                        if (displayed_columns & ColumnData::Model) {
                            std::for_each(model_used_filaments_m_all_plates.begin(), model_used_filaments_m_all_plates.end(), [&total_model_used_filament_m](double value) {
                                total_model_used_filament_m += value;
                                });
                            std::for_each(model_used_filaments_g_all_plates.begin(), model_used_filaments_g_all_plates.end(), [&total_model_used_filament_g](double value) {
                                total_model_used_filament_g += value;
                                });
                            if ((displayed_columns & ~ColumnData::Model) > 0)
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                            else
                                ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Model")] });
                        }
                        if (displayed_columns & ColumnData::Support) {
                            std::for_each(support_used_filaments_m_all_plates.begin(), support_used_filaments_m_all_plates.end(), [&total_support_used_filament_m](double value) {
                                total_support_used_filament_m += value;
                                });
                            std::for_each(support_used_filaments_g_all_plates.begin(), support_used_filaments_g_all_plates.end(), [&total_support_used_filament_g](double value) {
                                total_support_used_filament_g += value;
                                });
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_support_used_filament_m, total_support_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Support")] });
                        }
                        if (displayed_columns & ColumnData::Flushed) {
                            std::for_each(flushed_filaments_m_all_plates.begin(), flushed_filaments_m_all_plates.end(), [&total_flushed_filament_m](double value) {
                                total_flushed_filament_m += value;
                                });
                            std::for_each(flushed_filaments_g_all_plates.begin(), flushed_filaments_g_all_plates.end(), [&total_flushed_filament_g](double value) {
                                total_flushed_filament_g += value;
                                });
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_flushed_filament_m, total_flushed_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Flushed")] });
                        }
                        if (displayed_columns & ColumnData::WipeTower) {
                            std::for_each(wipe_tower_used_filaments_m_all_plates.begin(), wipe_tower_used_filaments_m_all_plates.end(), [&total_wipe_tower_used_filament_m](double value) {
                                total_wipe_tower_used_filament_m += value;
                                });
                            std::for_each(wipe_tower_used_filaments_g_all_plates.begin(), wipe_tower_used_filaments_g_all_plates.end(), [&total_wipe_tower_used_filament_g](double value) {
                                total_wipe_tower_used_filament_g += value;
                                });
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_wipe_tower_used_filament_m, total_wipe_tower_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Tower")] });
                        }
                        if ((displayed_columns & ~ColumnData::Model) > 0) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g",
                                total_model_used_filament_m + total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m,
                                (total_model_used_filament_g + total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g) / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Total")] });
                        }
                        append_item(false, m_tools.m_tool_colors[0], columns_offsets);
                    }
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Total cost") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, "%.2f", total_cost_all_plates);
                    imgui.text(buf);

                    // Calculating Column Offsets for the Time Estimation Table
                    {
                        std::vector<std::string>                                   time_labels;
                        std::vector<std::string>                                   time_values;
                        std::vector<std::pair<std::string, std::vector<::string>>> time_columns;

                        for (auto it = plate_time.begin(); it != plate_time.end(); it++) {
                            time_labels.push_back(_u8L("Plate") + " " + std::to_string(it->first + 1));
                            time_values.push_back(short_time(get_time_dhms(it->second)));
                        }
                        if (plate_time.size() > 1) {
                            time_labels.push_back(_u8L("Total"));
                            time_values.push_back(short_time(get_time_dhms(total_time_all_plates)));
                        }

                        time_columns.push_back({_u8L("Plate"), time_labels});
                        time_columns.push_back({_u8L("Time"), time_values});

                        auto time_offsets = calculate_offsets(time_columns);
                        for (int i = 0; i < time_offsets.size(); i++) { time_est_table_offsets[time_columns[i].first] = time_offsets[i]; }
                    }

                    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.title(_u8L("Time Estimation"));
                    for (auto it = plate_time.begin(); it != plate_time.end(); it++) {
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({_u8L("Plate") + " " + std::to_string(it->first + 1), time_est_table_offsets[_u8L("Plate")]});
                        columns_offsets.push_back({short_time(get_time_dhms(it->second)), time_est_table_offsets[_u8L("Time")]});
                        append_item(false, m_tools.m_tool_colors[0], columns_offsets);
                    }
                    if (plate_time.size() > 1) {
                        // Separator
                        ImGuiWindow* window = ImGui::GetCurrentWindow();
                        const ImRect separator(ImVec2(window->Pos.x + window_padding * 3, window->DC.CursorPos.y),
                            ImVec2(window->Pos.x + window->Size.x - window_padding * 3, window->DC.CursorPos.y + 1.0f));
                        ImGui::ItemSize(ImVec2(0.0f, 0.0f));
                        const bool item_visible = ImGui::ItemAdd(separator, 0);
                        window->DrawList->AddLine(separator.Min, ImVec2(separator.Max.x, separator.Min.y), ImGui::GetColorU32(ImGuiCol_Separator));
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({_u8L("Total"), time_est_table_offsets[_u8L("Plate")]});
                        columns_offsets.push_back({short_time(get_time_dhms(total_time_all_plates)), time_est_table_offsets[_u8L("Time")]});
                        append_item(false, m_tools.m_tool_colors[0], columns_offsets);
                    }
                }
                ImGui::End();
                ImGui::PopStyleColor(6);
                ImGui::PopStyleVar(3);
                return;
            }

            void BaseRenderer::reset()
            {
                //BBS: should also reset the result id
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": current result id %1% ") % m_last_result_id;
                m_last_result_id = -1;
                m_layer_fusion.reset();
                m_layer_fusion_ready = false;
                m_conflict_result.reset();
                m_gcode_check_result.reset();
                filament_printable_reuslt.reset();
                m_legend_enabled = true;
                m_legend_height = 0.0f;
                m_extruders_count = 0;
                m_roles.clear();
                m_max_print_height = 0.0f;
                m_paths_bounding_box.reset();
                m_max_bounding_box.reset();
                m_custom_gcode_per_print_z.clear();
                m_layers_z_range = { 0, 0 };
                // Orio : reinitialise la coupe X/Y pour la prochaine piece
                m_clip_xy_enabled = false;
                m_clip_xy = { -CLIP_XY_FAR, CLIP_XY_FAR, -CLIP_XY_FAR, CLIP_XY_FAR };
                m_clip_bound_active = { false, false, false, false };
                m_xy_extent_valid = false;
                // Orio : les objets masques le restent d'un tranchage a l'autre (elague au chargement)
                m_object_labels.clear();
                m_only_gcode_in_preview = false;
                if (m_p_sequential_view) {
                    m_p_sequential_view->gcode_window.reset();
                    m_p_sequential_view->gcode_ids.clear();
                }
                m_left_extruder_filament.clear();
                m_right_extruder_filament.clear();
                m_nozzle_nums = 0;
                m_extruder_ids.clear();
                m_filament_diameters.clear();
                m_filament_densities.clear();
                m_print_statistics.reset();
                m_ssid_to_moveid_map.clear();
                m_ssid_to_moveid_map.shrink_to_fit();
                m_plater_extruder.clear();
                m_contained_in_bed = true;
                m_config = nullptr;

                if (m_p_extrusions) {
                    m_p_extrusions->reset_ranges();
                }
                reset_shell();
            }

            void BaseRenderer::refresh(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors)
            {
#if ENABLE_GCODE_VIEWER_STATISTICS
                auto start_time = std::chrono::high_resolution_clock::now();
#endif // ENABLE_GCODE_VIEWER_STATISTICS
                //BBS: add mutex for protection of gcode result
                gcode_result.lock();
                //BBS: add safe check
                if (gcode_result.moves.size() == 0) {
                    //result cleaned before slicing ,should return here
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result reset before, return directly!");
                    gcode_result.unlock();
                    return;
                }
                const auto t_move_count = gcode_result.moves.size();
                //BBS: add mutex for protection of gcode result
                if (t_move_count == 0) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result m_moves_count is 0, return directly!");
                    gcode_result.unlock();
                    return;
                }
                wxBusyCursor busy;
                if (m_view_type == EViewType::Tool && !gcode_result.extruder_colors.empty()) {
                    // update tool colors from config stored in the gcode
                    m_tools.m_tool_colors = ::decode_colors(gcode_result.extruder_colors);
                    m_tools.m_tool_visibles = std::vector<bool>(m_tools.m_tool_colors.size());
                    for (auto item : m_tools.m_tool_visibles) item = true;
                }
                else {
                    // update tool colors
                    m_tools.m_tool_colors = ::decode_colors(str_tool_colors);
                    m_tools.m_tool_visibles = std::vector<bool>(m_tools.m_tool_colors.size());
                    for (auto item : m_tools.m_tool_visibles) item = true;
                }
                for (int i = 0; i < m_tools.m_tool_colors.size(); i++) {
                    m_tools.m_tool_colors[i] = adjust_color_for_rendering(m_tools.m_tool_colors[i]);
                }
                // ensure there are enough colors defined
                while (m_tools.m_tool_colors.size() < std::max(size_t(1), gcode_result.filaments_count)) {
                    m_tools.m_tool_colors.push_back(::decode_color("#FF8000"));
                    m_tools.m_tool_visibles.push_back(true);
                }
                // update ranges for coloring / legend
                if (m_p_extrusions) {
                    m_p_extrusions->reset_ranges();
                    for (size_t i = 0; i < t_move_count; ++i) {
                        // skip first vertex
                        if (i == 0)
                            continue;
                        const GCodeProcessorResult::MoveVertex& curr = gcode_result.moves[i];
                        switch (curr.type)
                        {
                        case EMoveType::Extrude:
                        {
                            if (curr.extrusion_role != ExtrusionRole::erCustom || curr.is_pa_line_calibration) {
                                m_p_extrusions->ranges.height.update_from(round_to_bin(curr.height));
                                m_p_extrusions->ranges.width.update_from(round_to_bin(curr.width));
                            }// prevent the start code extrude extreme height/width and make the range deviate from the normal range
                            m_p_extrusions->ranges.fan_speed.update_from(curr.fan_speed);
                            m_p_extrusions->ranges.additional_fan_speed.update_from(curr.additional_fan_speed);
                            m_p_extrusions->ranges.temperature.update_from(curr.temperature);
                            if (curr.extrusion_role != erCustom || is_extrusion_role_visible(ExtrusionRole::erCustom))
                                m_p_extrusions->ranges.volumetric_rate.update_from(round_to_bin(curr.volumetric_rate()));
                            if (curr.layer_duration > 0.f) {
                                m_p_extrusions->ranges.layer_duration.update_from(curr.layer_duration);
                            }
                            [[fallthrough]];
                        }
                        case EMoveType::Travel:
                        {
                            if (is_move_type_visible(curr.type))
                                m_p_extrusions->ranges.feedrate.update_from(curr.feedrate);
                            break;
                        }
                        default: { break; }
                        }
                    }
                }
#if ENABLE_GCODE_VIEWER_STATISTICS
                m_statistics.refresh_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time).count();
#endif // ENABLE_GCODE_VIEWER_STATISTICS
                //BBS: add mutex for protection of gcode result
                gcode_result.unlock();
                update_default_view_type();
            }

            void Shells::reset()
            {
                volumes.clear();
                print_id = -1;
            }

            void BaseRenderer::init_tool_maker(PresetBundle* preset_bundle)
            {
                const auto& p_sequential_view = get_sequential_view();
                if (!p_sequential_view) {
                    return;
                }
                // initializes tool marker
                std::string filename;
                if (preset_bundle != nullptr) {
                    const Preset* curr = &preset_bundle->printers.get_selected_preset();
                    if (curr->is_system)
                        filename = PresetUtils::system_printer_hotend_model(*curr);
                    else {
                        auto* printer_model = curr->config.opt<ConfigOptionString>("printer_model");
                        if (printer_model != nullptr && !printer_model->value.empty()) {
                            filename = preset_bundle->get_hotend_model_for_printer_model(printer_model->value);
                        }
                        if (filename.empty()) {
                            filename = preset_bundle->get_hotend_model_for_printer_model(PresetBundle::BBL_DEFAULT_PRINTER_MODEL);
                        }
                    }
                }
                p_sequential_view->marker.init(filename);
            }

            void BaseRenderer::render_sequential_view(uint32_t canvas_width, uint32_t canvas_height, int right_margin)
            {
                const auto& p_sequential_view = get_sequential_view();
                if (!p_sequential_view) {
                    return;
                }
                //BBS fixed bottom_margin for space to render horiz slider
                int bottom_margin = 64;
                if (show_sequential_view()) {
                    p_sequential_view->marker.set_world_position(p_sequential_view->current_position);
                    p_sequential_view->marker.set_world_offset(p_sequential_view->current_offset);
                    //BBS fixed buttom margin. m_moves_slider.pos_y
                    p_sequential_view->render(m_legend_height, canvas_width, canvas_height - bottom_margin * m_scale, right_margin * m_scale, m_view_type);
                }
                // Orio : case "Afficher le G-code" cochee : la fenetre G-code aussi hors du curseur des mouvements, sur
                // le dernier mouvement affiche
                else if (wxGetApp().app_config->get_bool("show_gcode_window") && p_sequential_view->current.last < p_sequential_view->gcode_ids.size())
                    p_sequential_view->gcode_window.render(m_legend_height, (float)canvas_height - bottom_margin * m_scale, (float)canvas_width - right_margin * m_scale,
                        static_cast<uint64_t>(p_sequential_view->gcode_ids[p_sequential_view->current.last]));
            }

            // Orio : ligne "Coque" de la legende : masque la silhouette pale des objets dans l'Apercu
            static bool s_shells_hidden = false;
            // Orio : lignes de la legende selectionnees (clic, Maj + clic pour en ajouter), affichees ou
            // masquees ensemble au clic sur l'une d'elles ou avec [V]. s_legend_apply : 0 masquer, 1 afficher,
            // -1 rien, applique par chaque ligne selectionnee pendant le rendu de la legende.
            static std::set<std::string> s_legend_selection;
            static int  s_legend_apply = -1;
            static int  s_legend_next_apply = -1;
            static bool s_legend_any_visible = false;
            static bool s_legend_toggle_request = false;
            // Orio : la legende a le focus apres un clic sur elle (perdu au clic ailleurs dans la scene) ;
            // Ctrl+A y selectionne alors toutes les lignes a case
            static bool s_legend_focused = false;
            static bool s_legend_select_all_request = false;
            static bool s_legend_select_all = false;

            // Orio : carte d'une ligne de la legende Type de ligne, ouverte par son (i) (survole 1,5 s, ou clique :
            // elle reste alors jusqu'au clic suivant sur ce (i) ou sur sa croix) : ce qu'est la ligne et ses reglages pour
            // toute la piece, ouverts dans la colonne d'un clic. Ids : les ExtrusionRole, puis les lignes d'options.
            enum : int { CardTravel = 100, CardSeams, CardRetract, CardUnretract, CardToolChange, CardWipe, CardShells };
            static int  s_card_id = -1;
            static bool s_card_pinned = false;
            static int  s_card_hover_id = -1;
            static std::chrono::steady_clock::time_point s_card_hover_start;
            static bool s_card_any_hovered = false;
            static bool s_card_row_drawn = false;
            static ImVec2 s_card_anchor;
            static Color s_card_color;
            static std::string s_card_name;

            struct LineCardInfo
            {
                const char* text;
                std::vector<std::pair<const char*, Preset::Type>> options;
            };
            static LineCardInfo line_card_info(int id)
            {
                const Preset::Type P = Preset::TYPE_PRINT, M = Preset::TYPE_PRINTER;
                switch (id) {
                case erPerimeter: return { L("The walls just inside the outer wall. They make the sides of the part solid: one per wall loop, minus the outer one."),
                    { {"wall_loops", P}, {"inner_wall_line_width", P}, {"inner_wall_speed", P} } };
                case erExternalPerimeter: return { L("The visible outside of the part, around each outline of every layer. It is printed slowly for a clean look."),
                    { {"outer_wall_line_width", P}, {"outer_wall_speed", P}, {"seam_position", P} } };
                case erOverhangPerimeter: return { L("Walls printed in the air, partly or fully over nothing. They appear where the part leans outwards and are slowed down so they sag less."),
                    { {"detect_overhang_wall", P}, {"enable_overhang_speed", P}, {"overhang_1_4_speed", P}, {"overhang_2_4_speed", P}, {"overhang_3_4_speed", P},
                      {"overhang_4_4_speed", P}, {"overhang_totally_speed", P}, {"wave_overhang_print_speed", P}, {"enable_support", P} } };
                case erInternalInfill: return { L("The hollow pattern filling the inside of the part. It saves filament and time; its density sets how strong the part is."),
                    { {"sparse_infill_density", P}, {"sparse_infill_pattern", P}, {"sparse_infill_speed", P} } };
                case erFloatingVerticalShell: return { L("Solid walls printed inside, under a wall that would otherwise rest on sparse infill. They hold up what is printed above."),
                    { {"detect_floating_vertical_shell", P} } };
                case erSolidInfill: return { L("Fully filled layers inside the part. They appear under the top surfaces and above the bottom ones (the shell layers), and where an area is too small for sparse infill."),
                    { {"minimum_sparse_infill_area", P}, {"top_shell_layers", P}, {"bottom_shell_layers", P}, {"internal_solid_infill_pattern", P}, {"internal_solid_infill_speed", P} } };
                case erTopSolidInfill: return { L("The last layer of each flat area on top of the part, the one you see and touch. It is printed slowly to be smooth."),
                    { {"top_shell_layers", P}, {"top_shell_thickness", P}, {"top_surface_pattern", P}, {"top_surface_speed", P} } };
                case erBottomSurface: return { L("The first layer of each flat area under the part: on the plate, or over the support."),
                    { {"bottom_shell_layers", P}, {"bottom_shell_thickness", P}, {"bottom_surface_pattern", P}, {"initial_layer_speed", P} } };
                case erIroning: return { L("A second pass of the hot nozzle over the top surfaces, with very little filament, to make them smoother."),
                    { {"ironing_type", P}, {"ironing_speed", P}, {"ironing_flow", P}, {"ironing_spacing", P} } };
                case erSupportIroning: return { L("Ironing of the top of the support, so that the part resting on it gets a smoother underside."), {} };
                case erBridgeInfill: return { L("Lines stretched in the air between two edges, over a hole or a gap. Their speed and flow are adjusted so they do not sag."),
                    { {"bridge_speed", P}, {"bridge_flow", P}, {"thick_bridges", P} } };
                case erGapFill: return { L("Thin lines filling the narrow gaps between walls, too narrow for a full line."),
                    { {"gap_infill_speed", P}, {"filter_out_gap_fill", P} } };
                case erSkirt: return { L("A loop around the parts, not touching them, printed first to get the filament flowing well."),
                    { {"skirt_loops", P}, {"skirt_distance", P} } };
                case erBrim: return { L("Flat rings around the bottom of the part, touching it, that help it stick to the plate. Removed after printing."),
                    { {"brim_type", P}, {"brim_width", P}, {"brim_object_gap", P} } };
                case erSupportMaterial: return { L("A temporary structure holding up the parts printed in the air. Removed after printing."),
                    { {"enable_support", P}, {"support_type", P}, {"support_threshold_angle", P}, {"support_speed", P} } };
                case erSupportMaterialInterface: return { L("The dense layers at the top and bottom of the support, touching the part. They give it a cleaner underside."),
                    { {"support_interface_top_layers", P}, {"support_interface_bottom_layers", P}, {"support_interface_spacing", P}, {"support_interface_speed", P} } };
                case erSupportTransition: return { L("Layers between the body of the support and its interface."), { {"support_type", P} } };
                case erWipeTower: return { L("A small tower beside the parts where the nozzle is cleaned at each filament change."),
                    { {"enable_prime_tower", P}, {"prime_tower_width", P}, {"prime_volume", P} } };
                case erFlush: return { L("Filament pushed out at a filament change to clean the old color, put into the parts instead of being wasted."),
                    { {"flush_into_infill", P}, {"flush_into_support", P} } };
                case erScarfSeam: return { L("The start and end of a wall loop, ramped up and overlapped to hide the seam."),
                    { {"seam_slope_type", P}, {"seam_slope_start_height", P}, {"seam_slope_entire_loop", P} } };
                case erCustom: return { L("Moves written in the custom G-code of the printer (start, end, filament change)."), {} };
                case CardTravel: return { L("Moves of the nozzle from one place to another without extruding."),
                    { {"travel_speed", P}, {"reduce_crossing_wall", P}, {"z_hop", M} } };
                case CardSeams: return { L("Where each wall loop starts and ends. A small mark may remain there on the part."),
                    { {"seam_position", P}, {"seam_gap", P} } };
                case CardRetract: return { L("The filament is pulled back before a travel, so it does not ooze on the way."),
                    { {"retraction_length", M}, {"retraction_speed", M}, {"retract_when_changing_layer", M} } };
                case CardUnretract: return { L("The filament pulled back is pushed again before extruding."), { {"deretraction_speed", M} } };
                case CardToolChange: return { L("Where the printer changes filament."), { {"enable_prime_tower", P}, {"prime_volume", P} } };
                case CardWipe: return { L("The nozzle moves over the part while retracting, to wipe off the filament left on its tip."),
                    { {"wipe", M}, {"wipe_distance", M} } };
                case CardShells: return { L("The pale outline of the parts, to see where the toolpaths lie in them. It is not printed."), {} };
                default: return { "", {} };
                }
            }
            // Valeur d'un reglage du preset edite, comme la colonne l'affiche (premiere valeur des reglages par extrudeur)
            static std::string line_card_value(const std::string& key, Preset::Type type)
            {
                PresetBundle& bundle = *wxGetApp().preset_bundle;
                const DynamicPrintConfig& config = type == Preset::TYPE_PRINTER ? bundle.printers.get_edited_preset().config : bundle.prints.get_edited_preset().config;
                const ConfigOption* opt = config.option(key);
                const ConfigOptionDef* def = print_config_def.get(key);
                if (opt == nullptr || def == nullptr)
                    return "";
                std::string value = opt->serialize();
                value = value.substr(0, value.find(','));
                if (def->type == coBool || def->type == coBools)
                    return _u8L(value == "1" ? "Yes" : "No");
                for (size_t i = 0; i < def->enum_values.size() && i < def->enum_labels.size(); ++i)
                    if (def->enum_values[i] == value)
                        return _u8L(def->enum_labels[i]);
                return def->sidetext.empty() || value.find('%') != std::string::npos ? value : value + " " + _u8L(def->sidetext);
            }

            void BaseRenderer::toggle_selected_legend_items() { s_legend_toggle_request = true; }
            bool BaseRenderer::select_all_legend_items() { return s_legend_select_all_request = s_legend_focused; }

            void BaseRenderer::render_shells()
            {
                //BBS: add shell previewing logic
                // Orio : un objet masque par l'oeil garde sa coque, tres pale, meme coques cachees
                const bool shells_shown = !s_shells_hidden && (m_shells.previewing || m_shells.visible);
                if ((!shells_shown && m_hidden_object_labels.empty()) || m_shells.volumes.empty())
                    //if (!m_shells.visible || m_shells.volumes.empty())
                    return;
                const ModelObjectPtrs& model_objs = wxGetApp().model().objects;
                const auto is_hidden = [this, &model_objs](const GLVolume& v) {
                    const int o = v.object_idx(), i = v.instance_idx();
                    return !m_hidden_object_labels.empty() && !v.is_wipe_tower && 0 <= o && o < int(model_objs.size()) &&
                           0 <= i && i < int(model_objs[o]->instances.size()) && is_object_hidden(int(model_objs[o]->instances[i]->get_labeled_id()));
                };
                for (GLVolume* v : m_shells.volumes.volumes)
                    v->color[3] = is_hidden(*v) ? 0.1f : 0.5f;
                const auto& shader = wxGetApp().get_shader("gouraud_light");
                if (shader == nullptr)
                    return;
                // when the background processing is enabled, it may happen that the shells data have been loaded
                // before opengl has been initialized for the preview canvas.
                // when this happens, the volumes' data have not been sent to gpu yet.
                for (GLVolume* v : m_shells.volumes.volumes) {
                    if (!v->indexed_vertex_array->has_VBOs())
                        v->finalize_geometry(true);
                }
                glsafe(::glDepthMask(GL_FALSE));
                wxGetApp().bind_shader(shader);
                // Orio : les coques suivent la meme coupe X/Y que les trajectoires, sinon leur
                // enveloppe transparente recouvre la tranche. Leur position monde vient de
                // volume_world_matrix, fournie volume par volume par GLVolumeCollection::render.
                apply_xy_clip_uniforms(*shader);
                //BBS: reopen cul faces
                auto& camera = wxGetApp().plater()->get_camera();
                std::vector<std::array<float, 4>> colors = wxGetApp().plater()->get_extruders_colors();
                m_shells.volumes.render(GUI::ERenderPipelineStage::Normal, GLVolumeCollection::ERenderType::Transparent, false, camera, colors, wxGetApp().plater()->model(),
                    [shells_shown, &is_hidden](const GLVolume& v) { return shells_shown || is_hidden(v); });
                // gouraud_light est partage (gizmos, axes, marqueur...) : on y retire la coupe
                shader->set_uniform("u_clip_xy_enabled", 0.0f);
                wxGetApp().unbind_shader();
                glsafe(::glDepthMask(GL_TRUE));
            }

            void BaseRenderer::render_slider(int canvas_width, int canvas_height)
            {
                if (m_moves_slider) {
                    m_moves_slider->render(canvas_width, canvas_height);
                }
                // Orio : un seul curseur vertical visible a la fois. Le bouton lettre
                // (X/Y/Z) en haut permet de changer d'axe ; chaque axe conserve sa position car
                // ce sont des objets distincts (curseurs de coupe X/Y et curseur de couches Z).
                // Les coupes X et Y restent appliquees au rendu meme quand un autre axe est affiche.
                // Orio : a gauche de la vue, dans la vue reduite de set_vertical_sliders_view().
                if (m_vertical_sliders_view[0] > 0)
                    canvas_width = m_vertical_sliders_view[0];
                if (m_vertical_sliders_view[1] > 0)
                    canvas_height = m_vertical_sliders_view[1];
                render_cut_axis_toggle(canvas_width, canvas_height);
                // render_cut() ne renvoie vrai que si l'utilisateur a deplace une poignee : seule cette
                // coupe-la est memorisee pour les tranchages suivants.
                if (m_active_cut_axis == 0) {
                    if (m_x_slider && m_x_slider->render_cut(canvas_width, canvas_height, 0))
                        remember_xy_cut(0);
                }
                else if (m_active_cut_axis == 1) {
                    if (m_y_slider && m_y_slider->render_cut(canvas_width, canvas_height, 0))
                        remember_xy_cut(1);
                }
                else {
                    if (m_layers_slider) m_layers_slider->render(canvas_width, canvas_height);
                }
            }

            // Orio : memorise (en coordonnees du plateau) la coupe de l'axe que l'utilisateur vient
            // de deplacer ; une poignee ramenee en bout de course efface la memoire de ce cote.
            void BaseRenderer::remember_xy_cut(int axis)
            {
                IMSlider* slider = axis == 0 ? m_x_slider : m_y_slider;
                if (slider == nullptr || !m_xy_extent_valid)
                    return;
                const double origin = slider->get_cut_label_offset();
                XYCutMemory& memory = current_xy_cut_memory();
                memory.active[2 * axis]     = !slider->is_lower_at_min();
                memory.active[2 * axis + 1] = !slider->is_higher_at_max();
                memory.mm[2 * axis]         = slider->GetLowerValueD() - origin;
                memory.mm[2 * axis + 1]     = slider->GetHigherValueD() - origin;
            }

            // Orio : memoire des coupes X/Y du plateau affiche.
            // ponytail: cle = indice du plateau ; supprimer / reordonner des plateaux decale les memoires
            // (au pire une coupe reprise sur un autre plateau), cle par plateau si cela gene.
            BaseRenderer::XYCutMemory& BaseRenderer::current_xy_cut_memory()
            {
                return m_xy_cut_memory[wxGetApp().plater()->get_partplate_list().get_curr_plate_index()];
            }

            // Orio : curseur vertical actuellement affiche (voir render_slider()).
            IMSlider* BaseRenderer::get_active_vertical_slider()
            {
                if (m_active_cut_axis == 0)
                    return m_x_slider;
                if (m_active_cut_axis == 1)
                    return m_y_slider;
                return m_layers_slider;
            }

            // Orio : molette sur le curseur vertical affiche (avant, toujours sur le curseur Z, meme
            // cache : sa derniere fenetre est au meme endroit que celle du curseur X/Y). Une coupe X/Y
            // deplacee ainsi est memorisee comme un deplacement a la souris.
            void BaseRenderer::on_vertical_slider_mouse_wheel(wxMouseEvent& evt)
            {
                IMSlider* slider = get_active_vertical_slider();
                if (slider == nullptr)
                    return;
                const int lower  = slider->GetLowerValue();
                const int higher = slider->GetHigherValue();
                slider->on_mouse_wheel(evt);
                if (slider->is_cut_slider() && (slider->GetLowerValue() != lower || slider->GetHigherValue() != higher))
                    remember_xy_cut(m_active_cut_axis);
            }

            // Orio : fleches haut/bas sur le curseur vertical affiche. Meme logique que l'ancien code
            // de GLCanvas3D pour le curseur de couches (poignee selectionnee, mode une couche) ; sur un
            // curseur de coupe, un pas vaut ~1 mm au lieu d'une graduation de ~0,1 mm.
            void BaseRenderer::move_vertical_slider(int steps)
            {
                IMSlider* slider = get_active_vertical_slider();
                if (slider == nullptr)
                    return;
                if (slider->is_cut_slider())
                    steps *= slider->get_cut_step_ticks();
                if (slider->GetSelection() == ssHigher)
                    slider->SetHigherValue(slider->GetHigherValue() + steps);
                else if (slider->GetSelection() == ssLower)
                    slider->SetLowerValue(slider->GetLowerValue() + steps);
                if (slider->is_one_layer())
                    slider->SetLowerValue(slider->GetHigherValue());
                // BBS set as dirty, update in render_gcode()
                slider->set_as_dirty();
                if (slider->is_cut_slider())
                    remember_xy_cut(m_active_cut_axis);
            }

            // Orio : petit bouton lettre (X/Y/Z) en haut du curseur ; un clic passe a
            // l'axe suivant (X -> Y -> Z -> X). Toujours visible pour pouvoir revenir en arriere.
            // Au-dessus (ou a gauche si la place manque), la fleche des reglages, qui remet les trois
            // coupes a zero ; elle n'apparait que si une coupe est active. Boutons communs avec l'onglet
            // Preparation : render_cut_axis_buttons() (IMSlider.cpp).
            void BaseRenderer::render_cut_axis_toggle(int canvas_width, int canvas_height)
            {
                if (!m_x_slider || !m_y_slider || !m_layers_slider)
                    return;

                const bool anything_cut = !m_x_slider->is_full_span() || !m_y_slider->is_full_span() || !m_layers_slider->is_full_span();
                int axis = m_active_cut_axis;
                if (render_cut_axis_buttons("gcode", canvas_width, 0.0f, (float) canvas_height, m_layers_slider->m_scale, m_is_dark, axis, anything_cut))
                    reset_all_cuts();
                m_active_cut_axis = axis;
            }

            // Orio : bouton de remise a zero des coupes : les curseurs X, Y et Z reviennent en pleine
            // plage (poignee haute tout en haut, poignee basse tout en bas, mode une couche quitte), toute la
            // piece redevient visible ; la memoire des coupes X/Y est effacee, sinon le tranchage suivant les
            // remettrait. Les curseurs marques « modifies » sont appliques par GLCanvas3D::_render_gcode.
            void BaseRenderer::reset_all_cuts()
            {
                for (IMSlider* slider : { m_x_slider, m_y_slider, m_layers_slider }) {
                    if (slider != nullptr && !slider->is_full_span())
                        slider->SetSelectionSpan(slider->GetMinValue(), slider->GetMaxValue());
                }
                remember_xy_cut(0);
                remember_xy_cut(1);
            }

            // Orio : reperes des plans de coupe X/Y, pour voir ou passe la coupe et sur quel axe.
            // Un rectangle par borne qui coupe, a la hauteur des couches visibles (curseur Z), avec une
            // petite marge autour des trajectoires sur les cotes non coupes. X en rouge, Y en vert
            // (couleurs des axes) ; le plan de l'axe affiche sur le curseur est plus marque.
            // fill = remplissage translucide, dessine AVANT les trajectoires : elles le recouvrent, la
            // tranche reste donc nette et le plan ne se voit qu'autour. Sinon le cadre, dessine APRES.
            void BaseRenderer::render_cut_planes(bool fill)
            {
                if (!m_clip_xy_enabled || !m_xy_extent_valid)
                    return;
                const auto& shader = wxGetApp().get_shader("flat");
                if (shader == nullptr)
                    return;

                GLModel& model = fill ? m_cut_plane_fill : m_cut_plane_outline;
                if (!model.is_initialized()) {
                    // rectangle unitaire du plan (u, v), place ensuite par une matrice
                    GLModel::Geometry init_data;
                    init_data.format = { fill ? GLModel::PrimitiveType::Triangles : GLModel::PrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
                    init_data.reserve_vertices(4);
                    init_data.reserve_indices(fill ? 6 : 8);
                    init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
                    init_data.add_vertex(Vec3f(1.0f, 0.0f, 0.0f));
                    init_data.add_vertex(Vec3f(1.0f, 1.0f, 0.0f));
                    init_data.add_vertex(Vec3f(0.0f, 1.0f, 0.0f));
                    if (fill) {
                        init_data.add_triangle(0, 1, 2);
                        init_data.add_triangle(0, 2, 3);
                    }
                    else {
                        init_data.add_line(0, 1);
                        init_data.add_line(1, 2);
                        init_data.add_line(2, 3);
                        init_data.add_line(3, 0);
                    }
                    model.init_from(std::move(init_data));
                }

                // Hauteur : du dessous de la couche visible la plus basse au dessus de la plus haute.
                double z_min = 0.0;
                double z_max = m_paths_bounding_box.defined ? m_paths_bounding_box.max.z() : 0.0;
                if (m_layers_slider != nullptr && m_layers_slider->GetHigherValueD() > 0.0) {
                    z_max = m_layers_slider->GetHigherValueD();
                    if (!m_layers_slider->is_lower_at_min())
                        z_min = m_layers_slider->GetValueAtD(m_layers_slider->GetLowerValue() - 1);
                }
                // bas tres legerement releve pour ne pas scintiller avec le plateau ; marge au-dessus
                z_min += 0.02;
                z_max += CUT_PLANE_MARGIN_MM;
                if (!(z_max > z_min))
                    return;

                // Etendue de chaque plan sur l'autre axe : jusqu'a la coupe de cet axe s'il y en a une,
                // sinon jusqu'aux trajectoires plus la marge.
                const double margin = CUT_PLANE_MARGIN_MM;
                const double x_lo = m_clip_bound_active[0] ? m_clip_xy_mm[0] : m_xy_extent_x[0] - margin;
                const double x_hi = m_clip_bound_active[1] ? m_clip_xy_mm[1] : m_xy_extent_x[1] + margin;
                const double y_lo = m_clip_bound_active[2] ? m_clip_xy_mm[2] : m_xy_extent_y[0] - margin;
                const double y_hi = m_clip_bound_active[3] ? m_clip_xy_mm[3] : m_xy_extent_y[1] + margin;

                // couleurs des axes, plus vives en theme sombre
                const ColorRGBA axis_colors[2] = {
                    m_is_dark ? ColorRGBA(1.00f, 0.40f, 0.40f, 1.0f) : ColorRGBA(0.85f, 0.10f, 0.10f, 1.0f),
                    m_is_dark ? ColorRGBA(0.40f, 0.95f, 0.45f, 1.0f) : ColorRGBA(0.05f, 0.60f, 0.15f, 1.0f) };

                const GLboolean cull_enabled = ::glIsEnabled(GL_CULL_FACE);
                const GLboolean blend_enabled = ::glIsEnabled(GL_BLEND);
                glsafe(::glEnable(GL_DEPTH_TEST));
                glsafe(::glDepthMask(GL_FALSE));
                glsafe(::glDisable(GL_CULL_FACE));
                glsafe(::glEnable(GL_BLEND));
                glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
                const auto& ogl_manager = wxGetApp().get_opengl_manager();
                if (!fill && ogl_manager)
                    ogl_manager->set_line_width(2.0f * std::max(1.0f, m_scale));

                wxGetApp().bind_shader(shader);
                const Camera& camera = wxGetApp().plater()->get_camera();
                const Transform3d& view_matrix = camera.get_view_matrix();
                shader->set_uniform("projection_matrix", camera.get_projection_matrix());
                for (int bound = 0; bound < 4; ++bound) {
                    if (!m_clip_bound_active[bound])
                        continue;
                    const int axis = bound / 2; // 0 = X, 1 = Y
                    const double c = m_clip_xy_mm[bound];
                    // rectangle unitaire -> plan de coupe : u le long de l'autre axe, v le long de Z
                    Transform3d plane = Transform3d::Identity();
                    if (axis == 0) {
                        plane.linear().col(0) = Vec3d(0.0, y_hi - y_lo, 0.0);
                        plane.linear().col(1) = Vec3d(0.0, 0.0, z_max - z_min);
                        plane.linear().col(2) = Vec3d::UnitX();
                        plane.translation() = Vec3d(c, y_lo, z_min);
                    }
                    else {
                        plane.linear().col(0) = Vec3d(x_hi - x_lo, 0.0, 0.0);
                        plane.linear().col(1) = Vec3d(0.0, 0.0, z_max - z_min);
                        plane.linear().col(2) = Vec3d::UnitY();
                        plane.translation() = Vec3d(x_lo, c, z_min);
                    }
                    const bool emphasized = (m_active_cut_axis == axis);
                    ColorRGBA color = axis_colors[axis];
                    color.a(fill ? (emphasized ? 0.12f : 0.06f) : (emphasized ? 0.95f : 0.5f));
                    shader->set_uniform("view_model_matrix", view_matrix * plane);
                    model.set_color(color);
                    model.render_geometry();
                }
                wxGetApp().unbind_shader();

                if (!fill && ogl_manager)
                    ogl_manager->set_line_width(1.0f);
                glsafe(::glDepthMask(GL_TRUE));
                if (!blend_enabled)
                    glsafe(::glDisable(GL_BLEND));
                if (cull_enabled)
                    glsafe(::glEnable(GL_CULL_FACE));
            }

            void BaseRenderer::render_legend(float& legend_height, int canvas_width, int canvas_height, int right_margin)
            {
                if (!m_legend_enabled)
                    return;
                s_card_any_hovered = false;
                s_card_row_drawn = false;
                // Orio : bouton "tranchage precedent". S'il etait maintenu a l'image precedente, cette
                // legende montre le tranchage d'avant de ce plateau : ses donnees sont echangees avec celles du
                // tranchage courant le temps de ce rendu (avant tout calcul), puis remises en place en sortant.
                SliceSnapshot* prev_slice = update_slice_history();
                // Orio : objet clique dans l'Apercu : la legende montre ses statistiques (sans tranchage precedent)
                const bool show_picked = update_picked_stats();
                if (show_picked)
                    prev_slice = nullptr;
                const bool show_prev_slice = prev_slice != nullptr && m_slice_history->held && !m_fold;
                if (show_prev_slice)
                    swap_with_slice_snapshot(*prev_slice);
                struct PrevSliceRestore
                {
                    std::function<void()> restore;
                    ~PrevSliceRestore() { if (restore) restore(); }
                } prev_slice_restore{ show_prev_slice ? std::function<void()>([this, prev_slice]() { swap_with_slice_snapshot(*prev_slice); }) : std::function<void()>() };
                const auto swap_picked = [this]() {
                    std::swap(m_print_statistics, m_picked_stats->stats);
                    m_roles.swap(m_picked_stats->roles);
                };
                if (show_picked)
                    swap_picked();
                PrevSliceRestore picked_restore{ show_picked ? std::function<void()>(swap_picked) : std::function<void()>() };
                // Orio : la legende change de contenu au choix d'une piece : une image de plus pour qu'elle prenne sa
                // taille (sinon une image avec la taille d'avant, contenu coupe)
                static bool s_last_show_picked = false;
                if (s_last_show_picked != show_picked) {
                    s_last_show_picked = show_picked;
                    wxGetApp().imgui()->set_requires_extra_frame();
                }
                const ImVec4 prev_slice_text_color = m_is_dark ? ImVec4(1.00f, 0.71f, 0.20f, 1.00f) : ImVec4(0.80f, 0.42f, 0.00f, 1.00f);
                const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
                auto group_result = DevUtilBackend::GetNozzleGroupResult(wxGetApp().plater());
                bool is_support_dynamic_nozzle_map = group_result && group_result->is_support_dynamic_nozzle_map();
                bool is_show_left_right_result = is_support_dynamic_nozzle_map && wxGetApp().sidebar().is_fila_switch_ready();
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                // Orio : selection des lignes ; [V] bascule les lignes selectionnees (masquees si l'une est visible)
                static EViewType s_selection_view = m_view_type;
                if (s_selection_view != m_view_type) {
                    s_selection_view = m_view_type;
                    s_legend_selection.clear();
                }
                s_legend_apply = s_legend_next_apply;
                s_legend_next_apply = -1;
                if (s_legend_toggle_request && s_legend_apply < 0 && !s_legend_selection.empty())
                    s_legend_apply = s_legend_any_visible ? 0 : 1;
                s_legend_toggle_request = false;
                s_legend_any_visible = false;
                s_legend_select_all = s_legend_select_all_request;
                s_legend_select_all_request = false;
                //BBS: GUI refactor: move to the right
                // Orio : la legende se glisse a la souris ; elle garde sa distance au bord droit et au haut de la scene
                static ImVec2 s_legend_offset{ 0.0f, 0.0f };
                static bool   s_legend_dragging = false;
                const float   legend_right = float(canvas_width - right_margin * m_scale);
                imgui.set_next_window_pos(legend_right + s_legend_offset.x, s_legend_offset.y, ImGuiCond_Always, 1.0f, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0, 0.0));
                ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
                //ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.f, 1.f, 1.f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, { 1, 0, 0, 0 });
                ImGui::SetNextWindowBgAlpha(0.8f);
                const float max_height = 0.75f * static_cast<float>(cnv_size.get_height());
                const float child_height = 0.3333f * max_height;
                // Orio : la legende garde sa taille avec ou sans piece choisie (et tranchage precedent) : la plus
                // grande vue pour ce tranchage et cette vue (repliee : rien)
                static std::map<int, ImVec2> s_legend_min_size;
                static unsigned int s_legend_min_result = (unsigned int)-1;
                if (s_legend_min_result != m_last_result_id) {
                    s_legend_min_size.clear();
                    s_legend_min_result = m_last_result_id;
                }
                const ImVec2 legend_min = m_fold ? ImVec2(0.0f, 0.0f) : s_legend_min_size[int(m_view_type)];
                // (max.x negatif : ImGui ignorerait le minimum en largeur)
                ImGui::SetNextWindowSizeConstraints({ legend_min.x, std::min(legend_min.y, max_height) }, { FLT_MAX, max_height });
                // Orio : lisere bleu des fenetres d'outils (ImGuiWrapper::begin_tool_window), dessine par Begin
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(1.0f, 1.5f * m_scale));
                ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_BAMBU);
                imgui.begin(std::string("Legend"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                // Orio : glisser depuis un endroit de la legende sans bouton ni case la deplace, sans sortir de la scene
                const bool legend_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    s_legend_focused = legend_hovered;
                    if (legend_hovered && !ImGui::IsAnyItemHovered()) {
                        s_legend_dragging = true;
                        // Double clic sur la bande du titre (fond sombre dessine plus bas) : retour a la place d'origine.
                        // Chronometre a nous : IsMouseDoubleClicked suit g.Time, qui n'avance qu'aux images dessinees.
                        static std::chrono::steady_clock::time_point s_top_click;
                        const auto now = std::chrono::steady_clock::now();
                        if (ImGui::GetIO().MousePos.y < ImGui::GetWindowPos().y + ImGui::GetFrameHeight() + 10.0f * m_scale) {
                            if (now - s_top_click < std::chrono::milliseconds(500)) {
                                s_legend_offset = ImVec2(0.0f, 0.0f);
                                s_top_click = {};
                                imgui.set_requires_extra_frame();
                            } else
                                s_top_click = now;
                        }
                    }
                }
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    s_legend_dragging = false;
                if (s_legend_dragging) {
                    s_legend_offset.x += ImGui::GetIO().MouseDelta.x;
                    s_legend_offset.y += ImGui::GetIO().MouseDelta.y;
                }
                const ImVec2 legend_size = ImGui::GetCurrentWindow()->Size;
                s_legend_offset.x = std::clamp(s_legend_offset.x, std::min(0.0f, legend_size.x - legend_right), std::max(0.0f, float(canvas_width) - legend_right));
                s_legend_offset.y = std::clamp(s_legend_offset.y, 0.0f, std::max(0.0f, float(canvas_height) - legend_size.y));
                // Orio : tranchage precedent affiche -> tout le texte de la legende en ambre
                if (show_prev_slice)
                    ImGui::PushStyleColor(ImGuiCol_Text, prev_slice_text_color);
                enum class EItemType : unsigned char
                {
                    Rect,
                    Circle,
                    Hexagon,
                    Line,
                    None
                };
                const PrintEstimatedStatistics::Mode& time_mode = m_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)];
                //BBS
                /*bool show_estimated_time = time_mode.time > 0.0f && (m_view_type == EViewType::FeatureType ||
                    (m_view_type == EViewType::ColorPrint && !time_mode.custom_gcode_times.empty()));*/
                bool show_estimated = time_mode.time > 0.0f && (m_view_type == EViewType::FeatureType || m_view_type == EViewType::ColorPrint);
                const float icon_size = ImGui::GetTextLineHeight() * 0.7;
                // Orio : place du (i) des cartes, entre la pastille et le nom
                const float card_icon_w = 1.2f * icon_size + 6.0f * m_scale;
                //BBS GUI refactor
                //const float percent_bar_size = 2.0f * ImGui::GetTextLineHeight();
                const float percent_bar_size = 0;
                bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 pos_rect = ImGui::GetCursorScreenPos();
                float window_padding = 4.0f * m_scale;
                float checkbox_offset = 0.0f;
                draw_list->AddRectFilled(ImVec2(pos_rect.x, pos_rect.y - ImGui::GetStyle().WindowPadding.y),
                    ImVec2(pos_rect.x + ImGui::GetWindowWidth() + ImGui::GetFrameHeight(), pos_rect.y + ImGui::GetFrameHeight() + window_padding * 2.5),
                    ImGui::GetColorU32(ImVec4(0, 0, 0, 0.3)));
                // Orio : cles (legend_cell_key) des valeurs qui different entre les deux tranchages, remplies
                // sous le bouton "tranchage precedent" tant qu'il est survole ou maintenu. value_text() affiche une
                // valeur, en surbrillance (fond jaune, ambre pour le tranchage precedent) si sa cle y figure.
                std::set<std::string> changed_cells;
                const ImVec4 changed_cell_bg = show_prev_slice ? ImVec4(1.00f, 0.71f, 0.20f, 1.00f) : ImVec4(1.00f, 0.86f, 0.30f, 1.00f);
                auto value_text = [&imgui, &changed_cells, changed_cell_bg, this](const std::string& key, const std::string& text) {
                    if (key.empty() || changed_cells.count(key) == 0) {
                        imgui.text(text);
                        return;
                    }
                    // fond sous le texte seul (sans les espaces de tete), a la hauteur ou ImGui::Text le pose
                    const size_t first = std::min(text.find_first_not_of(' '), text.size());
                    const ImVec2 cursor = ImGui::GetCursorScreenPos();
                    const ImVec2 pos(cursor.x + ImGui::CalcTextSize(text.substr(0, first).c_str()).x, cursor.y + ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset);
                    const ImVec2 size = ImGui::CalcTextSize(text.c_str() + first);
                    const float pad = 2.0f * m_scale;
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x - pad, pos.y - 0.5f * pad), ImVec2(pos.x + size.x + pad, pos.y + size.y + 0.5f * pad),
                        ImGui::GetColorU32(changed_cell_bg), 2.0f * m_scale);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.08f, 1.00f));
                    imgui.text(text);
                    ImGui::PopStyleColor(1);
                    };
                auto append_item = [icon_size, card_icon_w, &imgui, imperial_units, &window_padding, &draw_list, &checkbox_offset, &value_text, this](
                    EItemType type,
                    const Color& color,
                    const std::vector<std::pair<std::string, float>>& columns_offsets,
                    bool checkbox = true,
                    bool visible = true,
                    std::function<void()> callback = nullptr,
                    // Orio : cle de chaque colonne pour la surbrillance des valeurs ("" = aucune)
                    const std::vector<std::string>& cell_keys = std::vector<std::string>(),
                    // Orio : carte de la ligne (line_card_info), -1 = pas de (i)
                    int card_id = -1)
                    {
                        // render icon
                        ImVec2 pos = ImVec2(ImGui::GetCursorScreenPos().x + window_padding * 3, ImGui::GetCursorScreenPos().y);
                        switch (type) {
                        default:
                        case EItemType::Rect: {
                            draw_list->AddRectFilled({ pos.x + 1.0f * m_scale, pos.y + 3.0f * m_scale }, { pos.x + icon_size - 1.0f * m_scale, pos.y + icon_size + 1.0f * m_scale },
                                ImGui::GetColorU32({ color[0], color[1], color[2], color[3] }));
                            break;
                        }
                        case EItemType::Circle: {
                            ImVec2 center(0.5f * (pos.x + pos.x + icon_size), 0.5f * (pos.y + pos.y + icon_size + 5.0f));
                            draw_list->AddCircleFilled(center, 0.5f * icon_size, ImGui::GetColorU32({ color[0], color[1], color[2], color[3] }), 16);
                            break;
                        }
                        case EItemType::Hexagon: {
                            ImVec2 center(0.5f * (pos.x + pos.x + icon_size), 0.5f * (pos.y + pos.y + icon_size + 5.0f));
                            draw_list->AddNgonFilled(center, 0.5f * icon_size, ImGui::GetColorU32({ color[0], color[1], color[2], color[3] }), 6);
                            break;
                        }
                        case EItemType::Line: {
                            draw_list->AddLine({ pos.x + 1, pos.y + icon_size + 2 }, { pos.x + icon_size - 1, pos.y + 4 }, ImGui::GetColorU32({ color[0], color[1], color[2], color[3] }), 3.0f);
                            break;
                        case EItemType::None:
                            break;
                        }
                        }
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20.0 * m_scale, 6.0 * m_scale));
                        // Orio : (i) a gauche du nom. Survole 1,5 s : carte de la ligne, cachee en le quittant ;
                        // clique : carte gardee jusqu'au clic suivant sur lui. Ni la selection ni l'affichage n'en changent.
                        bool on_card_icon = false;
                        if (card_id >= 0) {
                            const float r = 0.6f * icon_size;
                            const ImVec2 c(ImGui::GetWindowPos().x - ImGui::GetScrollX() + ImGui::GetStyle().ItemSpacing.x + icon_size + r, pos.y + 0.5f * (icon_size + 5.0f));
                            on_card_icon = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(c.x - r - 2.0f, c.y - r - 2.0f), ImVec2(c.x + r + 2.0f, c.y + r + 2.0f));
                            if (on_card_icon) {
                                s_card_any_hovered = true;
                                const auto now = std::chrono::steady_clock::now();
                                if (s_card_hover_id != card_id) {
                                    s_card_hover_id = card_id;
                                    s_card_hover_start = now;
                                }
                                // le rendu n'a lieu qu'aux evenements : une image de plus jusqu'aux 1,5 s
                                const auto waited = now - s_card_hover_start;
                                if (!s_card_pinned && waited >= std::chrono::milliseconds(1500))
                                    s_card_id = card_id;
                                else if (!s_card_pinned && waited >= std::chrono::milliseconds(0))
                                    imgui.set_requires_extra_frame();
                            }
                            if (s_card_id == card_id) {
                                s_card_row_drawn = true;
                                s_card_anchor = ImVec2(ImGui::GetCurrentWindow()->RootWindow->Pos.x, pos.y);
                                s_card_color = color;
                                s_card_name = columns_offsets[0].first;
                            }
                            const bool lit = on_card_icon || s_card_id == card_id;
                            const ImU32 accent = ImGui::GetColorU32(m_is_dark ? ImVec4(0.0f, 194.0f / 255.0f, 1.0f, 1.0f) : ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.0f));
                            if (lit)
                                draw_list->AddCircleFilled(c, r, accent, 16);
                            else
                                draw_list->AddCircle(c, r, ImGui::GetColorU32(ImGuiCol_Text, 0.7f), 16, std::max(1.0f, m_scale));
                            // Drawn, not written: a glyph sits off center in its line box and text is snapped to whole pixels.
                            // PathStroke rather than AddLine, which shifts by half a pixel away from the circle center.
                            const ImU32 i_color = lit ? IM_COL32_WHITE : ImGui::GetColorU32(ImGuiCol_Text);
                            draw_list->AddCircleFilled(ImVec2(c.x, c.y - 0.45f * r), 0.15f * r, i_color, 12);
                            draw_list->PathLineTo(ImVec2(c.x, c.y - 0.15f * r));
                            draw_list->PathLineTo(ImVec2(c.x, c.y + 0.6f * r));
                            draw_list->PathStroke(i_color, 0, std::max(1.0f, 0.24f * r));
                        }
                        // BBS render selectable
                        ImGui::Dummy({ 0.0, 0.0 });
                        ImGui::SameLine();
                        if (callback) {
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * m_scale);
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0 * m_scale, 0.0));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.00f, 0.68f, 0.26f, 0.0f));
                            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.00f, 0.68f, 0.26f, 0.0f));
                            ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.00f));
                            float max_height = 0.f;
                            for (auto column_offset : columns_offsets) {
                                if (ImGui::CalcTextSize(column_offset.first.c_str()).y > max_height)
                                    max_height = ImGui::CalcTextSize(column_offset.first.c_str()).y;
                            }
                            bool b_menu_item = ImGui::BBLMenuItem(("##" + columns_offsets[0].first).c_str(), nullptr, false, true, max_height);
                            ImGui::PopStyleVar(2);
                            ImGui::PopStyleColor(3);
                            if (b_menu_item && on_card_icon) {
                                const bool close = s_card_pinned && s_card_id == card_id;
                                s_card_pinned = !close;
                                s_card_id = close ? -1 : card_id;
                                // encore survole : pas de carte au survol avant d'en etre sorti
                                s_card_hover_start = std::chrono::steady_clock::now() + std::chrono::hours(1);
                                b_menu_item = false;
                            }
                            // Orio : sur une ligne a case, seule la case affiche / masque (toute la selection, a l'image
                            // suivante, si la ligne fait partie d'une selection multiple) ; ailleurs sur la ligne, clic :
                            // cette ligne seule selectionnee (tout deselectionne si elle l'etait), Maj + clic : ajout / retrait
                            const std::string& item_key = columns_offsets[0].first;
                            if (checkbox && s_legend_select_all)
                                s_legend_selection.insert(item_key);
                            const bool selected = checkbox && s_legend_selection.count(item_key) > 0;
                            if (selected) {
                                draw_list->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32({ 8.0f / 255.0f, 116.0f / 255.0f, 224.0f / 255.0f, 0.25f }));
                                if (s_legend_apply >= 0 && visible != (s_legend_apply == 1)) {
                                    callback();
                                    visible = !visible;
                                }
                                s_legend_any_visible |= visible;
                            }
                            // La case garde l'ID du menu : survolee, c'est lui qui recoit le clic
                            bool on_checkbox = false;
                            if (checkbox) {
                                ImGui::SameLine(checkbox_offset);
                                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0, 0.0));
                                ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.0f, 194.0f / 255.0f, 1.0f, 1.0f)); // Orio : ORION dans les deux thèmes
                                bool shown = visible;
                                ImGui::Checkbox(("##" + columns_offsets[0].first).c_str(), &shown);
                                on_checkbox = ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                                ImGui::PopStyleColor(1);
                                ImGui::PopStyleVar(1);
                            }
                            if (b_menu_item && checkbox && !on_checkbox) {
                                if (!wxGetKeyState(WXK_SHIFT))
                                    s_legend_selection = selected ? std::set<std::string>() : std::set<std::string>{ item_key };
                                else if (s_legend_selection.erase(item_key) == 0)
                                    s_legend_selection.insert(item_key);
                            }
                            else if (b_menu_item && selected && s_legend_selection.size() > 1) {
                                s_legend_next_apply = visible ? 0 : 1;
                                on_visibility_changed();
                            }
                            else if (b_menu_item)
                                callback();
                        }
                        // BBS render column item
                        {
                            if (callback && !checkbox && !visible)
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(172 / 255.0f, 172 / 255.0f, 172 / 255.0f, 1.00f));
                            float dummy_size = type == EItemType::None ? window_padding * 3 : ImGui::GetStyle().ItemSpacing.x + icon_size;
                            if (card_id >= 0)
                                dummy_size += card_icon_w;
                            ImGui::SameLine(dummy_size);
                            value_text(cell_keys.empty() ? std::string() : cell_keys[0], columns_offsets[0].first);
                            for (auto i = 1; i < columns_offsets.size(); i++) {
                                ImGui::SameLine(columns_offsets[i].second);
                                value_text(i < cell_keys.size() ? cell_keys[i] : std::string(), columns_offsets[i].first);
                            }
                            if (callback && !checkbox && !visible)
                                ImGui::PopStyleColor(1);
                        }
                        ImGui::PopStyleVar(1);
                    };
                auto append_range = [append_item](const Range& range, unsigned int decimals) {
                    auto append_range_item = [append_item, range](int i, float value, unsigned int decimals) {
                        char buf[1024];
                        ::sprintf(buf, "%.*f", decimals, value);
                        append_item(EItemType::Rect, range.range_colors[i].get_data(), { {buf, 0} }, true, true, nullptr, { legend_cell_key("range", i, "value") });
                        };
                    if (range.count == 1)
                        // single item use case
                        append_range_item(0, range.min, decimals);
                    else if (range.count == 2) {
                        append_range_item(static_cast<int>(range.range_colors.size()) - 1, range.max, decimals);
                        append_range_item(0, range.min, decimals);
                    }
                    else {
                        const float step_size = range.step_size();
                        for (int i = static_cast<int>(range.range_colors.size()) - 1; i >= 0; --i) {
                            append_range_item(i, range.get_value_at_step(i), decimals);
                        }
                    }
                    };
                auto append_headers = [&imgui, &window_padding](const std::vector<std::pair<std::string, float>>& title_offsets) {
                    for (size_t i = 0; i < title_offsets.size(); i++) {
                        ImGui::SameLine(title_offsets[i].second);
                        imgui.bold_text(title_offsets[i].first);
                        //Orio: a (?) says what the two values of the Percent column are, in a tooltip with a blue border
                        //(hard to tell from the legend behind it otherwise).
                        if (title_offsets[i].first == _u8L("Percent")) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(?)");
                            if (ImGui::IsItemHovered()) {
                                ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
                                ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_BAMBU);
                                imgui.tooltip(_u8L("time / material").c_str(), ImGui::GetFontSize() * 20.f);
                                ImGui::PopStyleColor(1);
                                ImGui::PopStyleVar(1);
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::Dummy({ window_padding, 0 });
                    ImGui::Separator();
                    };
                auto max_width = [](const std::vector<std::string>& items, const std::string& title, float extra_size = 0.0f) {
                    float ret = ImGui::CalcTextSize(title.c_str()).x;
                    for (const std::string& item : items) {
                        ret = std::max(ret, extra_size + ImGui::CalcTextSize(item.c_str()).x);
                    }
                    return ret;
                    };
                auto calculate_offsets = [max_width, window_padding, &checkbox_offset](const std::vector<std::pair<std::string, std::vector<::string>>>& title_columns, float extra_size = 0.0f) {
                    const ImGuiStyle& style = ImGui::GetStyle();
                    std::vector<float> offsets;
                    offsets.push_back(max_width(title_columns[0].second, title_columns[0].first, extra_size) + 3.0f * style.ItemSpacing.x);
                    for (size_t i = 2; i < title_columns.size(); i++) {
                        if (title_columns[i].first == "") {
                            offsets.push_back(offsets.back() + max_width(title_columns[i - 1].second, "") + style.ItemSpacing.x);
                        }
                        else if (title_columns[i].first == _u8L("Display")) {
                            float length = ImGui::CalcTextSize(title_columns[i - 2].first.c_str()).x;
                            float offset = offsets.back() + max_width(title_columns[i - 1].second, title_columns[i - 1].first);
                            size_t index = offsets.size() - 2;
                            if (index >= 0) {
                                offset = std::max(offset, length + offsets[index]);
                            }
                            offsets.push_back(offset + 2.0f * style.ItemSpacing.x);
                        }
                        else {
                            offsets.push_back(offsets.back() + max_width(title_columns[i - 1].second, title_columns[i - 1].first) + 2.0f * style.ItemSpacing.x);
                        }
                    }
                    float average_col_width = ImGui::GetWindowWidth() / static_cast<float>(title_columns.size());
                    std::vector<float> ret;
                    ret.push_back(0);
                    for (size_t i = 1; i < title_columns.size(); i++) {
                        ret.push_back(std::max(offsets[i - 1], i * average_col_width));
                    }
                    if (title_columns.back().first == _u8L("Display")) {
                        checkbox_offset = ret.back() + window_padding;
                    }
                    return ret;
                    };
                // BBS: no ColorChange type, use ToolChange
                //auto color_print_ranges = [this](unsigned char extruder_id, const std::vector<CustomGCode::Item>& custom_gcode_per_print_z) {
                //    std::vector<std::pair<Color, std::pair<double, double>>> ret;
                //    ret.reserve(custom_gcode_per_print_z.size());
                //    for (const auto& item : custom_gcode_per_print_z) {
                //        if (extruder_id + 1 != static_cast<unsigned char>(item.extruder))
                //            continue;
                //        if (item.type != ColorChange)
                //            continue;
                //        const std::vector<double> zs = m_layers.get_zs();
                //        auto lower_b = std::lower_bound(zs.begin(), zs.end(), item.print_z - epsilon());
                //        if (lower_b == zs.end())
                //            continue;
                //        const double current_z = *lower_b;
                //        const double previous_z = (lower_b == zs.begin()) ? 0.0 : *(--lower_b);
                //        // to avoid duplicate values, check adding values
                //        if (ret.empty() || !(ret.back().second.first == previous_z && ret.back().second.second == current_z))
                //            ret.push_back({ decode_color(item.color), { previous_z, current_z } });
                //    }
                //    return ret;
                //};
                auto upto_label = [](double z) {
                    char buf[64];
                    ::sprintf(buf, "%.2f", z);
                    return _u8L("up to") + " " + std::string(buf) + " " + _u8L("mm");
                    };
                auto above_label = [](double z) {
                    char buf[64];
                    ::sprintf(buf, "%.2f", z);
                    return _u8L("above") + " " + std::string(buf) + " " + _u8L("mm");
                    };
                auto fromto_label = [](double z1, double z2) {
                    char buf1[64];
                    ::sprintf(buf1, "%.2f", z1);
                    char buf2[64];
                    ::sprintf(buf2, "%.2f", z2);
                    return _u8L("from") + " " + std::string(buf1) + " " + _u8L("to") + " " + std::string(buf2) + " " + _u8L("mm");
                    };
                auto role_time_and_percent = [time_mode](ExtrusionRole role) {
                    auto it = std::find_if(time_mode.roles_times.begin(), time_mode.roles_times.end(), [role](const std::pair<ExtrusionRole, float>& item) { return role == item.first; });
                    return (it != time_mode.roles_times.end()) ? std::make_pair(it->second, it->second / time_mode.time) : std::make_pair(0.0f, 0.0f);
                    };
                auto move_time_and_percent = [time_mode](EMoveType move_type) {
                    auto it = std::find_if(time_mode.moves_times.begin(), time_mode.moves_times.end(), [move_type](const std::pair<EMoveType, float>& item) { return move_type == item.first; });
                    return (it != time_mode.moves_times.end()) ? std::make_pair(it->second, it->second / time_mode.time) : std::make_pair(0.0f, 0.0f);
                    };
                auto used_filament_per_role = [this, imperial_units](ExtrusionRole role) {
                    auto it = m_print_statistics.used_filaments_per_role.find(role);
                    if (it == m_print_statistics.used_filaments_per_role.end())
                        return std::make_pair(0.0, 0.0);
                    double koef = imperial_units ? 1000.0 / GizmoObjectManipulation::in_to_mm : 1.0;
                    return std::make_pair(it->second.first * koef, it->second.second);
                    };
                // get used filament (meters and grams) from used volume in respect to the active extruder
                auto get_used_filament_from_volume = [this, imperial_units](double volume, int extruder_id) {
                    double koef = imperial_units ? 1.0 / GizmoObjectManipulation::in_to_mm : 0.001;
                    std::pair<double, double> ret = { koef * volume / (PI * sqr(0.5 * m_filament_diameters[extruder_id])),
                                                      volume * m_filament_densities[extruder_id] * 0.001 };
                    return ret;
                    };
                auto get_nozzle_label = [](const std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> &group_result, int filament_id) -> std::string {
                    if (!group_result) {
                        return "";
                    }
                    auto nozzles = group_result->get_nozzles_for_filament(filament_id);
                    if (nozzles.empty()) {
                        return "";
                    }
                    Preset& cur_preset = wxGetApp().preset_bundle->printers.get_selected_preset();
                    auto extruder_max_nozzle_count = cur_preset.config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count")->values;
                    bool support_multi_nozzle      = std::any_of(extruder_max_nozzle_count.begin(), extruder_max_nozzle_count.end(), [](int val) { return val > 1; });
                    std::vector<std::string> nozzle_labels;
                    for (const auto& nozzle : nozzles) {
                        std::string label;
                        if (nozzle.extruder_id == 0) {
                            label = "L";
                        } else if (nozzle.extruder_id == 1) {
                            label = "R";
                            if (support_multi_nozzle) {
                                label += std::to_string(nozzle.group_id);
                            }
                        }
                        nozzle_labels.push_back(label);
                    }
                    std::string result;
                    for (int i = 0; i < nozzle_labels.size(); ++i) {
                        if (i > 0) result += ", ";
                        result += nozzle_labels[i];
                    }
                    return result;

                    };
                //BBS Slicing Result title
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::SameLine();
                ImVec2      title_start_pos = ImGui::GetCursorPos();
                std::string title = _u8L("Slicing Result");
                // Orio : tranchage precedent affiche -> son titre, avec l'heure ou il a ete affiche
                if (show_prev_slice)
                    title = _u8L("Previous slice") + " (" + prev_slice->time_label + ")";
                // Orio : piece choisie : "Resultat de la decoupe pour <piece>", nom raccourci pour tenir avec sa croix
                // dans la place du titre normal (devant le bouton de repli)
                if (show_picked) {
                    const float room = (imgui.calc_text_size("ABCD").x + 18.0f * m_scale) * 8.0f - 18.0f * m_scale -
                                       imgui.calc_text_size("\xC3\x97").x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x;
                    std::string name = m_picked_stats->name;
                    auto with_name = [](const std::string& n) { return (boost::format(_u8L("Slicing Result for %1%")) % n).str(); };
                    // gras plus large que la police normale mesuree : 10 % de marge
                    while (!name.empty() && imgui.calc_text_size(with_name(name + "...")).x * 1.1f > room) {
                        // un caractere UTF-8 entier : ses octets de suite puis son premier octet
                        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80)
                            name.pop_back();
                        if (!name.empty())
                            name.pop_back();
                    }
                    title = with_name(name.size() == m_picked_stats->name.size() ? name : name + "...");
                }
                imgui.bold_text(title);
                // Orio : piece choisie : une croix bleue rend la legende au plateau entier
                if (show_picked) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_BAMBU);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    const bool close = ImGui::SmallButton("\xC3\x97##picked_close");
                    ImGui::PopStyleColor(2);
                    if (close) {
                        m_picked_label = -1;
                        imgui.set_requires_extra_frame();
                    }
                    if (ImGui::IsItemHovered())
                        imgui.tooltip(_u8L("Back to the statistics of the whole plate (Esc)"), ImGui::GetFontSize() * 20.f);
                }
                // Orio : bord droit du titre affiche (repere fenetre)
                const float title_right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();

                // BBS Set the width of the 8 "ABCD" words minus the "sliced result" to the spacing between the buttons and the title
                float single_word_width = imgui.calc_text_size("ABCD").x;
                float title_width = imgui.calc_text_size(title).x;
                float spacing = 18.0f * m_scale;
                ImGui::SameLine(0, (single_word_width + spacing) * 8.0f - title_width);
                // Orio : l'ecart ci-dessus compte le titre en police normale alors qu'il est dessine en gras :
                // plus long, le titre "Tranchage precedent (hh:mm)" pousserait le bouton de repli et elargirait le
                // panneau. Le bouton de repli garde donc la place qu'il a avec le titre normal (sans chevaucher le titre).
                if (!show_prev_slice && !show_picked)
                    m_slice_history->fold_button_x = ImGui::GetCursorPosX();
                else if (m_slice_history->fold_button_x > 0.0f)
                    ImGui::SameLine(std::max(m_slice_history->fold_button_x, title_right + spacing));
                std::wstring btn_name;
                if (m_fold)
                    btn_name = ImGui::UnfoldButtonIcon + boost::nowide::widen(std::string(""));
                else
                    btn_name = ImGui::FoldButtonIcon + boost::nowide::widen(std::string(""));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 0.78f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
                float button_width = ImGui::CalcTextSize(into_u8(btn_name).c_str()).x;
                // Orio : chevron toujours contre le bord droit de sa ligne (largeur de la fenetre a l'image
                // precedente), au plus a gauche a sa place habituelle. Au-dela de cette place il ne compte pas dans la
                // largeur de la fenetre, sinon elle ne retrecirait plus (au repli par exemple).
                ImGuiWindow* legend_window = ImGui::GetCurrentWindow();
                const float fold_default_x = ImGui::GetCursorPosX();
                const float fold_max_x = std::max(legend_window->DC.CursorMaxPos.x, legend_window->Pos.x - legend_window->Scroll.x + fold_default_x + button_width);
                ImGui::SetCursorPosX(std::max(fold_default_x, ImGui::GetWindowContentRegionMax().x - button_width));
                ImGui::SetCursorPosY(8.f);
                const bool fold_clicked = ImGui::Button(into_u8(btn_name).c_str(), ImVec2(button_width, 0));
                legend_window->DC.CursorMaxPos.x = fold_max_x;
                if (fold_clicked) { m_fold = !m_fold; }
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(1);
                if (m_fold) {
                    // Orio : legende repliee, sans bouton "tranchage precedent" : ni maintenu ni survole
                    m_slice_history->held = false;
                    m_slice_history->hovered = false;
                    legend_height = ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() + window_padding * 2.5;
                    if (show_prev_slice)
                        ImGui::PopStyleColor(1);
                    imgui.end();
                    ImGui::PopStyleColor(7);
                    ImGui::PopStyleVar(2);
                    return;
                }
                //BBS display Color Scheme
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::SameLine();
                imgui.bold_text(_u8L("Color Scheme"));
                ImGui::SameLine();
                push_combo_style();

                auto       *preset_bundle             = wxGetApp().preset_bundle;
                const bool  is_bbl_vendor_preset      = preset_bundle != nullptr && preset_bundle->printers.get_edited_preset().is_bbl_vendor_preset(preset_bundle);
                const auto *printer_model             = wxGetApp().plater()->get_curr_printer_model();
                const bool  hide_additional_fan_speed = !is_bbl_vendor_preset || (printer_model != nullptr && printer_model->support_side_panel_fan == "false");
                if (!show_prev_slice && hide_additional_fan_speed && view_type_items[m_view_type_sel] == EViewType::AdditionalFanSpeed) {
                    for (int i = 0; i < view_type_items.size(); ++i) {
                        if (view_type_items[i] == EViewType::FeatureType) {
                            apply_view_type_selection(i, EViewType::FeatureType);
                            break;
                        }
                    }
                }
                ImGuiComboFlags flags = 0;
                const char* view_type_value = view_type_image_names[m_view_type_sel].option_name.c_str();
                if (ImGui::BBLBeginCombo("", view_type_value, flags)) {
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
                    for (int i = 0; i < view_type_image_names.size(); i++) {
                        const bool is_selected = (m_view_type_sel == i);
                        if (hide_additional_fan_speed && view_type_items[i] == EViewType::AdditionalFanSpeed) {
                            continue;
                        }
                        if (ImGui::BBLSelectable_LeftImage(view_type_image_names[i].option_name.c_str(), is_selected, view_type_image_names[i].texture_id)) {
                            m_fold = false;
                            apply_view_type_selection(i, view_type_items[i]);
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::PopStyleVar(1);
                    ImGui::EndCombo();
                }
                pop_combo_style();
                ImGui::SameLine();
                ImGui::Dummy({ window_padding, window_padding });
                // Orio : bouton "tranchage precedent" (dessine tout en bas, son etat est celui de l'image
                // precedente) survole ou maintenu : les valeurs qui different entre le tranchage affiche et l'autre
                // passent en surbrillance. Les deux jeux de valeurs viennent de legend_value_cells(), l'autre apres un
                // echange aller-retour des donnees (celles en place restent celles affichees).
                if (prev_slice != nullptr && (m_slice_history->hovered || m_slice_history->held)) {
                    const PrintStatistics& current_ps = wxGetApp().plater()->get_partplate_list().get_current_fff_print().print_statistics();
                    double current_timelapse = 0.0;
                    if (auto it = m_gcode_result->skippable_part_time.find(SkipType::stTimelapse); it != m_gcode_result->skippable_part_time.end())
                        current_timelapse = it->second;
                    const std::map<std::string, std::string> shown_cells = legend_value_cells(imperial_units,
                        show_prev_slice ? prev_slice->print_stats : current_ps, show_prev_slice ? prev_slice->timelapse_time : current_timelapse);
                    swap_with_slice_snapshot(*prev_slice);
                    const std::map<std::string, std::string> other_cells = legend_value_cells(imperial_units,
                        show_prev_slice ? current_ps : prev_slice->print_stats, show_prev_slice ? current_timelapse : prev_slice->timelapse_time);
                    swap_with_slice_snapshot(*prev_slice);
                    for (const auto& [key, text] : shown_cells) {
                        auto it = other_cells.find(key);
                        if (it == other_cells.end() || it->second != text)
                            changed_cells.insert(key);
                    }
                }
                // data used to properly align items in columns when showing time
                std::vector<float> offsets;
                std::vector<std::string> labels;
                std::vector<std::string> times;
                std::string travel_time;
                std::vector<std::string> percents;
                std::string travel_percent;
                std::vector<double> model_used_filaments_m;
                std::vector<double> model_used_filaments_g;
                std::vector<std::string> used_filaments_m;
                std::vector<std::string> used_filaments_g;
                double total_model_used_filament_m = 0, total_model_used_filament_g = 0;
                std::vector<double> flushed_filaments_m;
                std::vector<double> flushed_filaments_g;
                double total_flushed_filament_m = 0, total_flushed_filament_g = 0;
                std::vector<double> wipe_tower_used_filaments_m;
                std::vector<double> wipe_tower_used_filaments_g;
                double total_wipe_tower_used_filament_m = 0, total_wipe_tower_used_filament_g = 0;
                std::vector<double> support_used_filaments_m;
                std::vector<double> support_used_filaments_g;
                double total_support_used_filament_m = 0, total_support_used_filament_g = 0;
                struct ColumnData {
                    enum {
                        Model = 1,
                        Flushed = 2,
                        WipeTower = 4,
                        Support = 1 << 3,
                    };
                };
                int displayed_columns = 0;
                std::map<std::string, float> color_print_offsets;
                // Orio : totaux du tranchage precedent quand il est affiche
                const PrintStatistics& ps = show_prev_slice ? prev_slice->print_stats :
                                            show_picked ? m_picked_stats->ps : wxGetApp().plater()->get_partplate_list().get_current_fff_print().print_statistics();
                double koef = imperial_units ? GizmoObjectManipulation::in_to_mm : 1000.0;
                double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1;
                // used filament statistics
                for (size_t extruder_id : m_extruder_ids) {
                    if (m_print_statistics.model_volumes_per_extruder.find(extruder_id) == m_print_statistics.model_volumes_per_extruder.end()) {
                        model_used_filaments_m.push_back(0.0);
                        model_used_filaments_g.push_back(0.0);
                    }
                    else {
                        double volume = m_print_statistics.model_volumes_per_extruder.at(extruder_id);
                        auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        model_used_filaments_m.push_back(model_used_filament_m);
                        model_used_filaments_g.push_back(model_used_filament_g);
                        total_model_used_filament_m += model_used_filament_m;
                        total_model_used_filament_g += model_used_filament_g;
                        displayed_columns |= ColumnData::Model;
                    }
                }
                for (size_t extruder_id : m_extruder_ids) {
                    if (m_print_statistics.wipe_tower_volumes_per_extruder.find(extruder_id) == m_print_statistics.wipe_tower_volumes_per_extruder.end()) {
                        wipe_tower_used_filaments_m.push_back(0.0);
                        wipe_tower_used_filaments_g.push_back(0.0);
                    }
                    else {
                        double volume = m_print_statistics.wipe_tower_volumes_per_extruder.at(extruder_id);
                        auto [wipe_tower_used_filament_m, wipe_tower_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        wipe_tower_used_filaments_m.push_back(wipe_tower_used_filament_m);
                        wipe_tower_used_filaments_g.push_back(wipe_tower_used_filament_g);
                        total_wipe_tower_used_filament_m += wipe_tower_used_filament_m;
                        total_wipe_tower_used_filament_g += wipe_tower_used_filament_g;
                        displayed_columns |= ColumnData::WipeTower;
                    }
                }
                for (size_t extruder_id : m_extruder_ids) {
                    if (m_print_statistics.flush_per_filament.find(extruder_id) == m_print_statistics.flush_per_filament.end()) {
                        flushed_filaments_m.push_back(0.0);
                        flushed_filaments_g.push_back(0.0);
                    }
                    else {
                        double volume = m_print_statistics.flush_per_filament.at(extruder_id);
                        auto [flushed_filament_m, flushed_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        flushed_filaments_m.push_back(flushed_filament_m);
                        flushed_filaments_g.push_back(flushed_filament_g);
                        total_flushed_filament_m += flushed_filament_m;
                        total_flushed_filament_g += flushed_filament_g;
                        displayed_columns |= ColumnData::Flushed;
                    }
                }
                for (size_t extruder_id : m_extruder_ids) {
                    if (m_print_statistics.support_volumes_per_extruder.find(extruder_id) == m_print_statistics.support_volumes_per_extruder.end()) {
                        support_used_filaments_m.push_back(0.0);
                        support_used_filaments_g.push_back(0.0);
                    }
                    else {
                        double volume = m_print_statistics.support_volumes_per_extruder.at(extruder_id);
                        auto [used_filament_m, used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        support_used_filaments_m.push_back(used_filament_m);
                        support_used_filaments_g.push_back(used_filament_g);
                        total_support_used_filament_m += used_filament_m;
                        total_support_used_filament_g += used_filament_g;
                        displayed_columns |= ColumnData::Support;
                    }
                }
                // extrusion paths section -> title
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::SameLine();
                switch (m_view_type)
                {
                case EViewType::FeatureType:
                {
                    // calculate offsets to align time/percentage data
                    char buffer[64];
                    //Orio: the Percent column also gives the share of the filament (extrusions of the listed types).
                    double total_role_filament_m = 0.0;
                    for (ExtrusionRole role : m_roles)
                        if (role < erCount)
                            total_role_filament_m += used_filament_per_role(role).first;
                    for (size_t i = 0; i < m_roles.size(); ++i) {
                        ExtrusionRole role = m_roles[i];
                        if (role < erCount) {
                            labels.push_back(_u8L(ExtrusionEntity::role_to_string(role)));
                            auto [time, percent] = role_time_and_percent(role);
                            times.push_back((time > 0.0f) ? short_time(get_time_dhms(time)) : "");
                            if (percent == 0)
                                ::sprintf(buffer, "0%%");
                            else
                                percent > 0.001 ? ::sprintf(buffer, "%.1f%%", percent * 100) : ::sprintf(buffer, "<0.1%%");
                            auto [model_used_filament_m, model_used_filament_g] = used_filament_per_role(role);
                            if (model_used_filament_m > 0.0 && total_role_filament_m > 0.0) {
                                const double share = model_used_filament_m / total_role_filament_m;
                                char material[32];
                                share > 0.001 ? ::sprintf(material, " / %.1f%%", share * 100) : ::sprintf(material, " / <0.1%%");
                                ::strcat(buffer, material);
                            }
                            percents.push_back(buffer);
                            //model_used_filaments_m.push_back(model_used_filament_m);
                            //model_used_filaments_g.push_back(model_used_filament_g);
                            memset(&buffer, 0, sizeof(buffer));
                            ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", model_used_filament_m);
                            used_filaments_m.push_back(buffer);
                            memset(&buffer, 0, sizeof(buffer));
                            ::sprintf(buffer, "%.2f g", model_used_filament_g);
                            used_filaments_g.push_back(buffer);
                        }
                    }
                    //BBS: get travel time and percent
                    {
                        auto [time, percent] = move_time_and_percent(EMoveType::Travel);
                        travel_time = (time > 0.0f) ? short_time(get_time_dhms(time)) : "";
                        if (percent == 0)
                            ::sprintf(buffer, "0%%");
                        else
                            percent > 0.001 ? ::sprintf(buffer, "%.1f%%", percent * 100) : ::sprintf(buffer, "<0.1%%");
                        travel_percent = buffer;
                    }
                    // Orio : piece choisie : meme colonne Pourcentage, calculee sur la piece seule (stats echangees)
                    const std::string percent_title = _u8L("Percent");
                    // Orio : la colonne fait aussi la place du (?) du titre
                    std::vector<std::string> percent_cells = percents;
                    percent_cells.push_back(percent_title + " (?)");
                    offsets = calculate_offsets({ {_u8L("Line Type"), labels}, {_u8L("Time"), times}, {percent_title, percent_cells}, {_u8L("Used filament"), used_filaments_m}, {"", used_filaments_g}, {_u8L("Display"), {""}} }, icon_size + card_icon_w);
                    append_headers({ {_u8L("Line Type"), offsets[0]}, {_u8L("Time"), offsets[1]}, {percent_title, offsets[2]}, {_u8L("Used filament"), offsets[3]}, {"", offsets[4]}, {_u8L("Display"), offsets[5]} });
                    break;
                }
                case EViewType::Height: { imgui.title(_u8L("Layer Height (mm)")); break; }
                case EViewType::Width: { imgui.title(_u8L("Line Width (mm)")); break; }
                case EViewType::Feedrate: { imgui.title(_u8L("Speed (mm/s)")); break; }
                case EViewType::FanSpeed: { imgui.title(_u8L("Fan Speed (%)")); break; }
                case EViewType::AdditionalFanSpeed: { imgui.title(_u8L("AUX  Fan Speed (%)")); break; }
                case EViewType::Temperature: { imgui.title(_u8L("Temperature (°C)")); break; }
                case EViewType::VolumetricRate: { imgui.title(_u8L("Volumetric flow rate (mm³/s)")); break; }
                case EViewType::LayerTime: { imgui.title(_u8L("Layer Time")); break; }
                case EViewType::LayerFusion: { imgui.title(_u8L("Layer Fusion (% of the line resting on the layer below)")); break; }
                case EViewType::Tool:
                {
                    // calculate used filaments data
                    for (size_t extruder_id : m_extruder_ids) {
                        if (m_print_statistics.model_volumes_per_extruder.find(extruder_id) == m_print_statistics.model_volumes_per_extruder.end())
                            continue;
                        double volume = m_print_statistics.model_volumes_per_extruder.at(extruder_id);
                        auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        model_used_filaments_m.push_back(model_used_filament_m);
                        model_used_filaments_g.push_back(model_used_filament_g);
                    }
                    offsets = calculate_offsets({ { "Extruder NNN", {""}} }, icon_size);
                    append_headers({ {_u8L("Filament"), offsets[0]}, {_u8L("Used filament"), offsets[1]} });
                    break;
                }
                case EViewType::ColorPrint:
                {
                    std::vector<std::string> total_filaments;
                    char buffer[64];
                    ::sprintf(buffer, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", ps.total_used_filament / /*1000*/koef, ps.total_weight / unit_conver);
                    total_filaments.push_back(buffer);
                    std::vector<std::pair<std::string, std::vector<::string>>> title_columns;
                    if (displayed_columns & ColumnData::Model) {
                        title_columns.push_back({ _u8L("Filament"), {""} });
                        title_columns.push_back({ _u8L("Model"), total_filaments });
                    }
                    if (displayed_columns & ColumnData::Support) {
                        title_columns.push_back({ _u8L("Support"), total_filaments });
                    }
                    if (displayed_columns & ColumnData::Flushed) {
                        title_columns.push_back({ _u8L("Flushed"), total_filaments });
                    }
                    if (displayed_columns & ColumnData::WipeTower) {
                        title_columns.push_back({ _u8L("Tower"), total_filaments });
                    }
                    if ((displayed_columns & ~ColumnData::Model) > 0) {
                        title_columns.push_back({ _u8L("Total"), total_filaments });
                    }
                    if (is_show_left_right_result) {
                        title_columns.push_back({ _u8L("Nozzles"), {""} });
                    }
                    auto offsets_ = calculate_offsets(title_columns, icon_size);
                    std::vector<std::pair<std::string, float>> title_offsets;
                    for (int i = 0; i < offsets_.size(); i++) {
                        title_offsets.push_back({ title_columns[i].first, offsets_[i] });
                        color_print_offsets[title_columns[i].first] = offsets_[i];
                    }
                    append_headers(title_offsets);
                    break;
                }
                default: { break; }
                }
                auto append_option_item = [this, append_item](EMoveType type, std::vector<float> offsets) {
                    auto append_option_item_with_type = [this, offsets, append_item](EMoveType type, const Color& color, const std::string& label, bool visible) {
                        const int card = m_view_type != EViewType::FeatureType ? -1 : type == EMoveType::Seam ? CardSeams : type == EMoveType::Retract ? CardRetract :
                            type == EMoveType::Unretract ? CardUnretract : type == EMoveType::Tool_change ? CardToolChange : type == EMoveType::Wipe ? CardWipe : -1;
                        append_item(EItemType::Rect, color, { { label , offsets[0] } }, true, visible, [this, type, visible]() {
                            set_move_type_visible(type, !is_move_type_visible(type));
                            on_visibility_changed();
                            }, {}, card);
                        };
                    const bool visible = is_move_type_visible(type);
                    if (type == EMoveType::Travel) {
                        //BBS: only display travel time in FeatureType view
                        append_option_item_with_type(type, Travel_Colors[0], _u8L("Travel"), visible);
                    }
                    else if (type == EMoveType::Seam)
                        append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::Seams], _u8L("Seams"), visible);
                    else if (type == EMoveType::Retract)
                        append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::Retractions], _u8L("Retract"), visible);
                    else if (type == EMoveType::Unretract)
                        append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::Unretractions], _u8L("Unretract"), visible);
                    else if (type == EMoveType::Tool_change)
                        append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::ToolChanges], _u8L("Filament Changes"), visible);
                    else if (type == EMoveType::Wipe)
                        append_option_item_with_type(type, Wipe_Color, _u8L("Wipe"), visible);
                    };
                // extrusion paths section -> items
                switch (m_view_type)
                {
                case EViewType::FeatureType:
                {
                    for (size_t i = 0; i < m_roles.size(); ++i) {
                        ExtrusionRole role = m_roles[i];
                        if (role >= erCount)
                            continue;
                        const bool visible = is_extrusion_role_visible(role);
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({ labels[i], offsets[0] });
                        columns_offsets.push_back({ times[i], offsets[1] });
                        columns_offsets.push_back({ percents[i], offsets[2] });
                        columns_offsets.push_back({ used_filaments_m[i], offsets[3] });
                        columns_offsets.push_back({ used_filaments_g[i], offsets[4] });
                        append_item(EItemType::Rect, Extrusion_Role_Colors[static_cast<unsigned int>(role)], columns_offsets,
                            true, visible, [this, role, visible]() {
                                set_extrusion_role_visible(role, !visible);
                                on_visibility_changed();
                            },
                            { "", legend_cell_key("role", int(role), "time"), legend_cell_key("role", int(role), "pct"),
                              legend_cell_key("role", int(role), "m"), legend_cell_key("role", int(role), "g") }, int(role));
                    }
                    for (auto item : options_items) {
                        if (item != EMoveType::Travel) {
                            append_option_item(item, offsets);
                        }
                        else {
                            //BBS: show travel time in FeatureType view
                            const bool visible = is_move_type_visible(item);
                            std::vector<std::pair<std::string, float>> columns_offsets;
                            columns_offsets.push_back({ _u8L("Travel"), offsets[0] });
                            columns_offsets.push_back({ travel_time, offsets[1] });
                            columns_offsets.push_back({ travel_percent, offsets[2] });
                            append_item(EItemType::Rect, Travel_Colors[0], columns_offsets, true, visible, [this, item, visible]() {
                                set_move_type_visible(item, !visible);
                                on_visibility_changed();
                                }, { "", legend_cell_key("travel", 0, "time"), legend_cell_key("travel", 0, "pct") }, CardTravel);
                        }
                    }
                    // Orio : ligne "Coque" : affiche / masque la silhouette pale des objets
                    if (!m_shells.volumes.empty() && (m_shells.previewing || m_shells.visible))
                        append_item(EItemType::Rect, { 0.6f, 0.6f, 0.6f, 1.0f }, { { _u8L("Shells"), offsets[0] } }, true, !s_shells_hidden, [this]() {
                            s_shells_hidden = !s_shells_hidden;
                            on_visibility_changed();
                            }, {}, CardShells);
                    break;
                }
                case EViewType::Height: { append_range(m_p_extrusions->ranges.height, 2); break; }
                case EViewType::Width: { append_range(m_p_extrusions->ranges.width, 2); break; }
                case EViewType::Feedrate: {
                    append_range(m_p_extrusions->ranges.feedrate, 0);
                    ImGui::Spacing();
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    offsets = calculate_offsets({ { _u8L("Options"), { _u8L("Travel")}}, { _u8L("Display"), {""}} }, icon_size);
                    append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
                    const bool travel_visible = is_move_type_visible(EMoveType::Travel);
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
                    append_item(EItemType::None, Travel_Colors[0], { {_u8L("travel"), offsets[0] } }, true, travel_visible, [this, travel_visible]() {
                        set_move_type_visible(EMoveType::Travel, !travel_visible);
                        // update buffers' render paths, and update m_tools.m_tool_colors and m_extrusions.ranges
                        refresh(*m_gcode_result, wxGetApp().plater()->get_extruder_colors_from_plater_config(m_gcode_result));
                        on_visibility_changed();
                        });
                    ImGui::PopStyleVar(1);
                    break;
                }
                case EViewType::FanSpeed: { append_range(m_p_extrusions->ranges.fan_speed, 0); break; }
                case EViewType::AdditionalFanSpeed: { append_range(m_p_extrusions->ranges.additional_fan_speed, 0); break; }
                case EViewType::Temperature: { append_range(m_p_extrusions->ranges.temperature, 0); break; }
                case EViewType::LayerTime: { append_range(m_p_extrusions->ranges.layer_duration, 1); break; }
                case EViewType::VolumetricRate: { append_range(m_p_extrusions->ranges.volumetric_rate, 2); break; }
                case EViewType::LayerFusion:
                {
                    Range fusion_range(Fusion_Colors);
                    fusion_range.min = 0.f;
                    fusion_range.max = 100.f;
                    fusion_range.count = 100;
                    append_range(fusion_range, 0);
                    append_item(EItemType::Rect, { 0.5f, 0.5f, 0.5f, 1.0f }, { { _u8L("Nothing below to compare with (first layer, supports...)"), 0 } }, true, true, nullptr);
                    render_layer_fusion_stats(window_padding);
                    break;
                }
                case EViewType::Tool:
                {
                    // shows only extruders actually used
                    char buf[64];
                    size_t i = 0;
                    for (unsigned char extruder_id : m_extruder_ids) {
                        ::sprintf(buf, imperial_units ? "%.2f in    %.2f g" : "%.2f m    %.2f g", model_used_filaments_m[i], model_used_filaments_g[i]);
                        append_item(EItemType::Rect, m_tools.m_tool_colors[extruder_id], { { _u8L("Extruder") + " " + std::to_string(extruder_id + 1), offsets[0]}, {buf, offsets[1]} },
                            true, true, nullptr, { "", legend_cell_key("tool", extruder_id, "used") });
                        i++;
                    }
                    break;
                }
                case EViewType::Summary:
                {
                    char buf[64];
                    imgui.text(_u8L("Total") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, imperial_units ? "%.2f in / %.2f oz" : "%.2f m / %.2f g", ps.total_used_filament / koef, ps.total_weight / unit_conver);
                    value_text(legend_cell_key("sum", 0, "total"), buf);
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Cost") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, "%.2f", ps.total_cost);
                    value_text(legend_cell_key("ps", 0, "cost"), buf);
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Total time") + ":");
                    ImGui::SameLine();
                    value_text(legend_cell_key("time", 0, "total"), short_time(get_time_dhms(time_mode.time)));
                    break;
                }
                case EViewType::ColorPrint:
                {
                    //BBS: replace model custom gcode with current plate custom gcode
                    const std::vector<CustomGCode::Item>& custom_gcode_per_print_z = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_custom_gcode_per_print_z;
                    size_t total_items = 1;
                    // BBS: no ColorChange type, use ToolChange
                    //for (size_t extruder_id : m_extruder_ids) {
                    //    total_items += color_print_ranges(extruder_id, custom_gcode_per_print_z).size();
                    //}
                    const bool need_scrollable = static_cast<float>(total_items) * (icon_size + ImGui::GetStyle().ItemSpacing.y) > child_height;
                    // add scrollable region, if needed
                    if (need_scrollable)
                        ImGui::BeginChild("color_prints", { -1.0f, child_height }, false);
                    // shows only extruders actually used
                    size_t i = 0;
                    for (auto extruder_idx : m_extruder_ids) {
                        const bool filament_visible = m_tools.m_tool_visibles[extruder_idx];
                        if (i < model_used_filaments_m.size() && i < model_used_filaments_g.size()) {
                            std::vector<std::pair<std::string, float>> columns_offsets;
                            columns_offsets.push_back({ std::to_string(extruder_idx + 1), color_print_offsets[_u8L("Filament")] });
                            // Orio : cle de chaque colonne pour la surbrillance des valeurs
                            std::vector<std::string> cell_keys{ "" };
                            char buf[64];
                            float column_sum_m = 0.0f;
                            float column_sum_g = 0.0f;
                            if (displayed_columns & ColumnData::Model) {
                                if ((displayed_columns & ~ColumnData::Model) > 0)
                                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", model_used_filaments_m[i], model_used_filaments_g[i] / unit_conver);
                                else
                                    ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", model_used_filaments_m[i], model_used_filaments_g[i] / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Model")] });
                                cell_keys.push_back(legend_cell_key("fil", extruder_idx, "model"));
                                column_sum_m += model_used_filaments_m[i];
                                column_sum_g += model_used_filaments_g[i];
                            }
                            if (displayed_columns & ColumnData::Support) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", support_used_filaments_m[i], support_used_filaments_g[i] / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Support")] });
                                cell_keys.push_back(legend_cell_key("fil", extruder_idx, "support"));
                                column_sum_m += support_used_filaments_m[i];
                                column_sum_g += support_used_filaments_g[i];
                            }
                            if (displayed_columns & ColumnData::Flushed) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", flushed_filaments_m[i], flushed_filaments_g[i] / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Flushed")] });
                                cell_keys.push_back(legend_cell_key("fil", extruder_idx, "flushed"));
                                column_sum_m += flushed_filaments_m[i];
                                column_sum_g += flushed_filaments_g[i];
                            }
                            if (displayed_columns & ColumnData::WipeTower) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", wipe_tower_used_filaments_m[i], wipe_tower_used_filaments_g[i] / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Tower")] });
                                cell_keys.push_back(legend_cell_key("fil", extruder_idx, "tower"));
                                column_sum_m += wipe_tower_used_filaments_m[i];
                                column_sum_g += wipe_tower_used_filaments_g[i];
                            }
                            if ((displayed_columns & ~ColumnData::Model) > 0) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", column_sum_m, column_sum_g / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Total")] });
                                cell_keys.push_back(legend_cell_key("fil", extruder_idx, "total"));
                            }
                            if (is_show_left_right_result) {
                                std::string nozzle_str   = get_nozzle_label(group_result, static_cast<int>(extruder_idx));
                                ::sprintf(buf, "%s", nozzle_str.c_str());
                                columns_offsets.push_back({buf, color_print_offsets[_u8L("Nozzles")]});
                                cell_keys.push_back("");
                            }
                            append_item(EItemType::Rect, m_tools.m_tool_colors[extruder_idx], columns_offsets, false, filament_visible, [this, extruder_idx]() {
                                m_tools.m_tool_visibles[extruder_idx] = !m_tools.m_tool_visibles[extruder_idx];
                                on_visibility_changed();
                                }, cell_keys);
                        }
                        i++;
                    }
                    if (need_scrollable)
                        ImGui::EndChild();
                    // Sum of all rows
                    char buf[64];
                    if (m_extruder_ids.size() > 1) {
                        // Separator
                        ImGuiWindow* window = ImGui::GetCurrentWindow();
                        const ImRect separator(ImVec2(window->Pos.x + window_padding * 3, window->DC.CursorPos.y), ImVec2(window->Pos.x + window->Size.x - window_padding * 3, window->DC.CursorPos.y + 1.0f));
                        ImGui::ItemSize(ImVec2(0.0f, 0.0f));
                        const bool item_visible = ImGui::ItemAdd(separator, 0);
                        window->DrawList->AddLine(separator.Min, ImVec2(separator.Max.x, separator.Min.y), ImGui::GetColorU32(ImGuiCol_Separator));
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({ _u8L("Total"), color_print_offsets[_u8L("Filament")] });
                        // Orio : cle de chaque colonne pour la surbrillance (ligne des totaux : filament -1)
                        std::vector<std::string> cell_keys{ "" };
                        if (displayed_columns & ColumnData::Model) {
                            if ((displayed_columns & ~ColumnData::Model) > 0)
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                            else
                                ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Model")] });
                            cell_keys.push_back(legend_cell_key("fil", -1, "model"));
                        }
                        if (displayed_columns & ColumnData::Support) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_support_used_filament_m, total_support_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Support")] });
                            cell_keys.push_back(legend_cell_key("fil", -1, "support"));
                        }
                        if (displayed_columns & ColumnData::Flushed) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_flushed_filament_m, total_flushed_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Flushed")] });
                            cell_keys.push_back(legend_cell_key("fil", -1, "flushed"));
                        }
                        if (displayed_columns & ColumnData::WipeTower) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_wipe_tower_used_filament_m, total_wipe_tower_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Tower")] });
                            cell_keys.push_back(legend_cell_key("fil", -1, "tower"));
                        }
                        if ((displayed_columns & ~ColumnData::Model) > 0) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_model_used_filament_m + total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m,
                                (total_model_used_filament_g + total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g) / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Total")] });
                            cell_keys.push_back(legend_cell_key("fil", -1, "total"));
                        }
                        append_item(EItemType::None, m_tools.m_tool_colors[0], columns_offsets, true, true, nullptr, cell_keys);
                    }
                    //BBS display filament change times
                    if (!show_picked) { // Orio : pas compte par objet
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Filament change times") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, "%d", m_print_statistics.total_filament_changes);
                    value_text(legend_cell_key("fil", -1, "changes"), buf);
                    }
                    //BBS display cost
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Cost") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, "%.2f", ps.total_cost);
                    value_text(legend_cell_key("ps", 0, "cost"), buf);
                    break;
                }
                default: { break; }
                }
                // partial estimated printing time section
                // Orio : pas avec le tranchage precedent (il croiserait ses temps et les G-codes
                // personnalises actuels du plateau) ; ce calcul n'est de toute facon plus affiche
                if (m_view_type == EViewType::ColorPrint && !show_prev_slice) {
                    using Times = std::pair<float, float>;
                    using TimesList = std::vector<std::pair<CustomGCode::Type, Times>>;
                    // helper structure containig the data needed to render the time items
                    struct PartialTime
                    {
                        enum class EType : unsigned char
                        {
                            Print,
                            ColorChange,
                            Pause
                        };
                        EType type;
                        int extruder_id;
                        Color color1;
                        Color color2;
                        Times times;
                        std::pair<double, double> used_filament{ 0.0f, 0.0f };
                    };
                    using PartialTimes = std::vector<PartialTime>;
                    auto generate_partial_times = [this, get_used_filament_from_volume](const TimesList& times, const std::vector<double>& used_filaments) {
                        PartialTimes items;
                        //BBS: replace model custom gcode with current plate custom gcode
                        std::vector<CustomGCode::Item> custom_gcode_per_print_z = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_custom_gcode_per_print_z;
                        std::vector<Color> last_color(m_extruders_count);
                        for (size_t i = 0; i < m_extruders_count; ++i) {
                            last_color[i] = m_tools.m_tool_colors[i];
                        }
                        int last_extruder_id = 1;
                        int color_change_idx = 0;
                        for (const auto& time_rec : times) {
                            switch (time_rec.first)
                            {
                            case CustomGCode::PausePrint: {
                                auto it = std::find_if(custom_gcode_per_print_z.begin(), custom_gcode_per_print_z.end(), [time_rec](const CustomGCode::Item& item) { return item.type == time_rec.first; });
                                if (it != custom_gcode_per_print_z.end()) {
                                    items.push_back({ PartialTime::EType::Print, it->extruder, last_color[it->extruder - 1], Color(), time_rec.second });
                                    items.push_back({ PartialTime::EType::Pause, it->extruder, Color(), Color(), time_rec.second });
                                    custom_gcode_per_print_z.erase(it);
                                }
                                break;
                            }
                            case CustomGCode::ColorChange: {
                                auto it = std::find_if(custom_gcode_per_print_z.begin(), custom_gcode_per_print_z.end(), [time_rec](const CustomGCode::Item& item) { return item.type == time_rec.first; });
                                if (it != custom_gcode_per_print_z.end()) {
                                    items.push_back({ PartialTime::EType::Print, it->extruder, last_color[it->extruder - 1], Color(), time_rec.second, get_used_filament_from_volume(used_filaments[color_change_idx++], it->extruder - 1) });
                                    items.push_back({ PartialTime::EType::ColorChange, it->extruder, last_color[it->extruder - 1], ::decode_color(it->color), time_rec.second });
                                    last_color[it->extruder - 1] = ::decode_color(it->color);
                                    last_extruder_id = it->extruder;
                                    custom_gcode_per_print_z.erase(it);
                                }
                                else
                                    items.push_back({ PartialTime::EType::Print, last_extruder_id, last_color[last_extruder_id - 1], Color(), time_rec.second, get_used_filament_from_volume(used_filaments[color_change_idx++], last_extruder_id - 1) });
                                break;
                            }
                            default: { break; }
                            }
                        }
                        return items;
                        };
                    auto append_color_change = [&imgui](const Color& color1, const Color& color2, const std::array<float, 4>& offsets, const Times& times) {
                        imgui.text(_u8L("Color change"));
                        ImGui::SameLine();
                        float icon_size = ImGui::GetTextLineHeight();
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        pos.x -= 0.5f * ImGui::GetStyle().ItemSpacing.x;
                        draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                            ImGui::GetColorU32({ color1[0], color1[1], color1[2], 1.0f }));
                        pos.x += icon_size;
                        draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                            ImGui::GetColorU32({ color2[0], color2[1], color2[2], 1.0f }));
                        ImGui::SameLine(offsets[0]);
                        imgui.text(short_time(get_time_dhms(times.second - times.first)));
                        };
                    auto append_print = [&imgui, imperial_units](const Color& color, const std::array<float, 4>& offsets, const Times& times, std::pair<double, double> used_filament) {
                        imgui.text(_u8L("Print"));
                        ImGui::SameLine();
                        float icon_size = ImGui::GetTextLineHeight();
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        pos.x -= 0.5f * ImGui::GetStyle().ItemSpacing.x;
                        draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                            ImGui::GetColorU32({ color[0], color[1], color[2], 1.0f }));
                        ImGui::SameLine(offsets[0]);
                        imgui.text(short_time(get_time_dhms(times.second)));
                        ImGui::SameLine(offsets[1]);
                        imgui.text(short_time(get_time_dhms(times.first)));
                        if (used_filament.first > 0.0f) {
                            char buffer[64];
                            ImGui::SameLine(offsets[2]);
                            ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", used_filament.first);
                            imgui.text(buffer);
                            ImGui::SameLine(offsets[3]);
                            ::sprintf(buffer, "%.2f g", used_filament.second);
                            imgui.text(buffer);
                        }
                        };
                    PartialTimes partial_times = generate_partial_times(time_mode.custom_gcode_times, m_print_statistics.volumes_per_color_change);
                    if (!partial_times.empty()) {
                        labels.clear();
                        times.clear();
                        for (const PartialTime& item : partial_times) {
                            switch (item.type)
                            {
                            case PartialTime::EType::Print: { labels.push_back(_u8L("Print")); break; }
                            case PartialTime::EType::Pause: { labels.push_back(_u8L("Pause")); break; }
                            case PartialTime::EType::ColorChange: { labels.push_back(_u8L("Color change")); break; }
                            }
                            times.push_back(short_time(get_time_dhms(item.times.second)));
                        }
                        std::string longest_used_filament_string;
                        for (const PartialTime& item : partial_times) {
                            if (item.used_filament.first > 0.0f) {
                                char buffer[64];
                                ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", item.used_filament.first);
                                if (::strlen(buffer) > longest_used_filament_string.length())
                                    longest_used_filament_string = buffer;
                            }
                        }
                        //offsets = calculate_offsets(labels, times, { _u8L("Event"), _u8L("Remaining time"), _u8L("Duration"), longest_used_filament_string }, 2.0f * icon_size);
                        //ImGui::Spacing();
                        //append_headers({ _u8L("Event"), _u8L("Remaining time"), _u8L("Duration"), _u8L("Used filament") }, offsets);
                        //const bool need_scrollable = static_cast<float>(partial_times.size()) * (icon_size + ImGui::GetStyle().ItemSpacing.y) > child_height;
                        //if (need_scrollable)
                        //    // add scrollable region
                        //    ImGui::BeginChild("events", { -1.0f, child_height }, false);
                        //for (const PartialTime& item : partial_times) {
                        //    switch (item.type)
                        //    {
                        //    case PartialTime::EType::Print: {
                        //        append_print(item.color1, offsets, item.times, item.used_filament);
                        //        break;
                        //    }
                        //    case PartialTime::EType::Pause: {
                        //        imgui.text(_u8L("Pause"));
                        //        ImGui::SameLine(offsets[0]);
                        //        imgui.text(short_time(get_time_dhms(item.times.second - item.times.first)));
                        //        break;
                        //    }
                        //    case PartialTime::EType::ColorChange: {
                        //        append_color_change(item.color1, item.color2, offsets, item.times);
                        //        break;
                        //    }
                        //    }
                        //}
                        //if (need_scrollable)
                        //    ImGui::EndChild();
                    }
                }
                // travel paths section
                if (is_move_type_visible(EMoveType::Travel)) {
                    switch (m_view_type)
                    {
                    case EViewType::Feedrate:
                    case EViewType::Tool:
                    case EViewType::ColorPrint: {
                        break;
                    }
                    default: {
                        // BBS GUI:refactor
                        // title
                        //ImGui::Spacing();
                        //imgui.title(_u8L("Travel"));
                        //// items
                        //append_item(EItemType::Line, Travel_Colors[0], _u8L("Movement"));
                        //append_item(EItemType::Line, Travel_Colors[1], _u8L("Extrusion"));
                        //append_item(EItemType::Line, Travel_Colors[2], _u8L("Retraction"));
                        break;
                    }
                    }
                }
                // wipe paths section
                //if (m_buffers[buffer_id(EMoveType::Wipe)].visible) {
                //    switch (m_view_type)
                //    {
                //    case EViewType::Feedrate:
                //    case EViewType::Tool:
                //    case EViewType::ColorPrint: { break; }
                //    default: {
                //        // title
                //        ImGui::Spacing();
                //        ImGui::Dummy({ window_padding, window_padding });
                //        ImGui::SameLine();
                //        imgui.title(_u8L("Wipe"));
                //        // items
                //        append_item(EItemType::Line, Wipe_Color, { {_u8L("Wipe"), 0} });
                //        break;
                //    }
                //    }
                //}
                //auto any_option_available = [this]() {
                //    auto available = [this](EMoveType type) {
                //        const TBuffer& buffer = m_buffers[buffer_id(type)];
                //        return buffer.visible && buffer.has_data();
                //        };
                //    return available(EMoveType::Color_change) ||
                //        available(EMoveType::Custom_GCode) ||
                //        available(EMoveType::Pause_Print) ||
                //        available(EMoveType::Retract) ||
                //        available(EMoveType::Tool_change) ||
                //        available(EMoveType::Unretract) ||
                //        available(EMoveType::Seam);
                //    };
                //auto add_option = [this, append_item](EMoveType move_type, EOptionsColors color, const std::string& text) {
                //    const TBuffer& buffer = m_buffers[buffer_id(move_type)];
                //    if (buffer.visible && buffer.has_data())
                //        append_item(EItemType::Circle, Options_Colors[static_cast<unsigned int>(color)], text);
                //};
                /* BBS GUI refactor */
                // options section
                //if (any_option_available()) {
                //    // title
                //    ImGui::Spacing();
                //    imgui.title(_u8L("Options"));
                //    // items
                //    add_option(EMoveType::Retract, EOptionsColors::Retractions, _u8L("Retractions"));
                //    add_option(EMoveType::Unretract, EOptionsColors::Unretractions, _u8L("Deretractions"));
                //    add_option(EMoveType::Seam, EOptionsColors::Seams, _u8L("Seams"));
                //    add_option(EMoveType::Tool_change, EOptionsColors::ToolChanges, _u8L("Tool changes"));
                //    add_option(EMoveType::Color_change, EOptionsColors::ColorChanges, _u8L("Color changes"));
                //    add_option(EMoveType::Pause_Print, EOptionsColors::PausePrints, _u8L("Print pauses"));
                //    add_option(EMoveType::Custom_GCode, EOptionsColors::CustomGCodes, _u8L("Custom G-codes"));
                //}
                // settings section
                bool has_settings = false;
                has_settings |= !m_settings_ids.print.empty();
                has_settings |= !m_settings_ids.printer.empty();
                bool has_filament_settings = true;
                has_filament_settings &= !m_settings_ids.filament.empty();
                for (const std::string& fs : m_settings_ids.filament) {
                    has_filament_settings &= !fs.empty();
                }
                has_settings |= has_filament_settings;
                //BBS: add only gcode mode
                bool show_settings = m_only_gcode_in_preview; //wxGetApp().is_gcode_viewer();
                show_settings &= (m_view_type == EViewType::FeatureType || m_view_type == EViewType::Tool);
                show_settings &= has_settings;
                if (show_settings) {
                    auto calc_offset = [this]() {
                        float ret = 0.0f;
                        if (!m_settings_ids.printer.empty())
                            ret = std::max(ret, ImGui::CalcTextSize((_u8L("Printer") + std::string(":")).c_str()).x);
                        if (!m_settings_ids.print.empty())
                            ret = std::max(ret, ImGui::CalcTextSize((_u8L("Print settings") + std::string(":")).c_str()).x);
                        if (!m_settings_ids.filament.empty()) {
                            for (unsigned char i : m_extruder_ids) {
                                ret = std::max(ret, ImGui::CalcTextSize((_u8L("Filament") + " " + std::to_string(i + 1) + ":").c_str()).x);
                            }
                        }
                        if (ret > 0.0f)
                            ret += 2.0f * ImGui::GetStyle().ItemSpacing.x;
                        return ret;
                        };
                    ImGui::Spacing();
                    imgui.title(_u8L("Settings"));
                    float offset = calc_offset();
                    if (!m_settings_ids.printer.empty()) {
                        imgui.text(_u8L("Printer") + ":");
                        ImGui::SameLine(offset);
                        imgui.text(m_settings_ids.printer);
                    }
                    if (!m_settings_ids.print.empty()) {
                        imgui.text(_u8L("Print settings") + ":");
                        ImGui::SameLine(offset);
                        imgui.text(m_settings_ids.print);
                    }
                    if (!m_settings_ids.filament.empty()) {
                        for (unsigned char i : m_extruder_ids) {
                            if (i < static_cast<unsigned char>(m_settings_ids.filament.size()) && !m_settings_ids.filament[i].empty()) {
                                std::string txt = _u8L("Filament");
                                txt += (m_extruder_ids.size() == 1) ? ":" : " " + std::to_string(i + 1);
                                imgui.text(txt);
                                ImGui::SameLine(offset);
                                imgui.text(m_settings_ids.filament[i]);
                            }
                        }
                    }
                }
                // total estimated printing time section
                if (show_estimated) {
                    ImGui::Spacing();
                    std::string time_title = m_view_type == EViewType::FeatureType ? _u8L("Total Estimation") : _u8L("Time Estimation");
                    auto can_show_mode_button = [this](PrintEstimatedStatistics::ETimeMode mode) {
                        bool show = false;
                        if (m_print_statistics.modes.size() > 1 && m_print_statistics.modes[static_cast<size_t>(mode)].roles_times.size() > 0) {
                            for (size_t i = 0; i < m_print_statistics.modes.size(); ++i) {
                                if (i != static_cast<size_t>(mode) &&
                                    m_print_statistics.modes[i].time > 0.0f &&
                                    short_time(get_time_dhms(m_print_statistics.modes[static_cast<size_t>(mode)].time)) != short_time(get_time_dhms(m_print_statistics.modes[i].time))) {
                                    show = true;
                                    break;
                                }
                            }
                        }
                        return show;
                        };
                    if (can_show_mode_button(m_time_estimate_mode)) {
                        switch (m_time_estimate_mode)
                        {
                        case PrintEstimatedStatistics::ETimeMode::Normal: { time_title += " [" + _u8L("Normal mode") + "]"; break; }
                        default: { assert(false); break; }
                        }
                    }
                    double timelapse_time = 0.f;
                    if (show_prev_slice) // Orio : tranchage precedent
                        timelapse_time = prev_slice->timelapse_time;
                    else if (auto timelapse_time_iter = m_gcode_result->skippable_part_time.find(SkipType::stTimelapse); !show_picked && timelapse_time_iter != m_gcode_result->skippable_part_time.end()) {
                        timelapse_time = timelapse_time_iter->second;
                    }
                    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.title(time_title);
                    std::string total_filament_str = _u8L("Total Filament");
                    std::string model_filament_str = _u8L("Model Filament");
                    std::string cost_str = _u8L("Cost");
                    std::string prepare_str;
                    if (timelapse_time != 0.0f)
                        prepare_str = _u8L("Prepare and timelapse time");
                    else
                        prepare_str = _u8L("Prepare time");
                    std::string print_str = _u8L("Model printing time");
                    std::string total_str = _u8L("Total time");
                    float max_len = window_padding + 2 * ImGui::GetStyle().ItemSpacing.x;
                    if (time_mode.layers_times.empty())
                        max_len += ImGui::CalcTextSize(total_str.c_str()).x;
                    else {
                        if (m_view_type == EViewType::FeatureType)
                            max_len += std::max(ImGui::CalcTextSize(cost_str.c_str()).x,
                                std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                                    std::max(std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x),
                                        std::max(ImGui::CalcTextSize(total_filament_str.c_str()).x, ImGui::CalcTextSize(model_filament_str.c_str()).x))));
                        else
                            max_len += std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                                (std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x)));
                    }
                    if (m_view_type == EViewType::FeatureType) {
                        //BBS display filament cost
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                        imgui.text(total_filament_str + ":");
                        ImGui::SameLine(max_len);
                        //BBS: use current plater's print statistics
                        bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
                        char buf[64];
                        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef);
                        value_text(legend_cell_key("est", 0, "total_m"), buf);
                        ImGui::SameLine();
                        ::sprintf(buf, imperial_units ? "  %.2f oz" : "  %.2f g", ps.total_weight / unit_conver);
                        value_text(legend_cell_key("est", 0, "total_g"), buf);
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                        imgui.text(model_filament_str + ":");
                        ImGui::SameLine(max_len);
                        auto exlude_m = total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m;
                        auto exlude_g = total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g;
                        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef - exlude_m);
                        value_text(legend_cell_key("est", 0, "model_m"), buf);
                        ImGui::SameLine();
                        ::sprintf(buf, imperial_units ? "  %.2f oz" : "  %.2f g", (ps.total_weight - exlude_g) / unit_conver);
                        value_text(legend_cell_key("est", 0, "model_g"), buf);
                        //BBS: display cost of filaments
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                        imgui.text(cost_str + ":");
                        ImGui::SameLine(max_len);
                        ::sprintf(buf, "%.2f", ps.total_cost);
                        value_text(legend_cell_key("ps", 0, "cost"), buf);
                    }
                    auto role_time = [time_mode](ExtrusionRole role) {
                        auto it = std::find_if(time_mode.roles_times.begin(), time_mode.roles_times.end(), [role](const std::pair<ExtrusionRole, float>& item) { return role == item.first; });
                        return (it != time_mode.roles_times.end()) ? it->second : 0.0f;
                        };
                    //BBS: start gcode is mostly same with prepeare time
                    if (time_mode.prepare_time != 0.0f) {
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                        imgui.text(prepare_str + ":");
                        ImGui::SameLine(max_len);
                        if (timelapse_time != 0.0f)
                            value_text(legend_cell_key("time", 0, "prepare"), short_time(get_time_dhms(time_mode.prepare_time)) + " + " + short_time(get_time_dhms(timelapse_time)));
                        else
                            value_text(legend_cell_key("time", 0, "prepare"), short_time(get_time_dhms(time_mode.prepare_time)));
                    }
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(print_str + ":");
                    ImGui::SameLine(max_len);
                    value_text(legend_cell_key("time", 0, "print"), short_time(get_time_dhms(time_mode.time - time_mode.prepare_time - timelapse_time)));
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(total_str + ":");
                    ImGui::SameLine(max_len);
                    value_text(legend_cell_key("time", 0, "total"), short_time(get_time_dhms(time_mode.time)));
                    // Orio : pas de bouton "Passer en mode silencieux" (estimation avec les limites silencieuses de la
                    // machine, demande de l'utilisateur) : temps du mode normal seulement
                }
                if (m_view_type == EViewType::ColorPrint) {
                    ImGui::Spacing();
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    offsets = calculate_offsets({ { _u8L("Options"), { ""}}, { _u8L("Display"), {""}} }, icon_size);
                    append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
                    for (auto item : options_items)
                        append_option_item(item, offsets);
                }
                // Orio : piece choisie : ce qui n'est pas compte pour elle, en liste
                if (show_picked) {
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text_colored(prev_slice_text_color, _u8L("Not counted for this part:"));
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered())
                        imgui.tooltip(_u8L("These expenses are shared by the whole plate. For this part, the travels count only from one of its lines to another "
                                           "of its lines in the same layer.").c_str(),
                                      ImGui::GetFontSize() * 20.f);
                    for (const std::string& item : { _u8L("travels between parts"), _u8L("prime tower"), _u8L("purges"), _u8L("start of the print") }) {
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                        imgui.text_colored(prev_slice_text_color, "- " + item);
                    }
                }
                ImGui::Dummy({ window_padding, window_padding });
                if (m_nozzle_nums > 1)
                    render_legend_color_arr_recommen(window_padding, is_show_left_right_result);
                // Orio : derniere ligne, case "Afficher le G-code" (fenetre G-code du curseur des mouvements)
                // a gauche et bouton "tranchage precedent" a droite ; puis la marge du bas. Tranchage precedent
                // affiche : la ligne reste a la hauteur du bouton (voir render_prev_slice_button)
                const float last_line_y = show_prev_slice ? std::max(ImGui::GetCursorPosY(), m_slice_history->button_pos.y) : ImGui::GetCursorPosY();
                const float prev_button_size = ImGui::GetFrameHeight();
                ImGui::SetCursorPos(ImVec2(window_padding * 3, last_line_y + ImGui::GetStyle().FramePadding.y));
                bool show_gcode_window = wxGetApp().app_config->get_bool("show_gcode_window");
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0, 0.0));
                ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.0f, 194.0f / 255.0f, 1.0f, 1.0f)); // Orio : ORION dans les deux thèmes
                if (ImGui::Checkbox((_u8L("Show G-code") + "##show_gcode_window").c_str(), &show_gcode_window)) {
                    wxGetApp().app_config->set_bool("show_gcode_window", show_gcode_window);
                    wxGetApp().app_config->save();
                }
                // Orio : case "Auto-slice" juste a gauche du bouton "tranchage precedent" (meme calcul de
                // position que render_prev_slice_button) : retranche 1 s apres un changement de reglage fait dans
                // l'Apercu (Plater::priv::auto_slice_wanted)
                {
                    bool auto_slice = wxGetApp().app_config->get_bool("auto_slice_preview");
                    const std::string label = _u8L("Auto-slice");
                    const float width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label.c_str()).x;
                    ImGuiWindow* window = ImGui::GetCurrentWindow();
                    const float content_right = window->DC.CursorMaxPos.x - window->Pos.x + window->Scroll.x;
                    float prev_button_x = content_right - window_padding * 3 - prev_button_size;
                    if (show_prev_slice)
                        prev_button_x = std::max(prev_button_x, m_slice_history->button_pos.x);
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), prev_button_x - ImGui::GetStyle().ItemSpacing.x - width));
                    if (ImGui::Checkbox((label + "##auto_slice_preview").c_str(), &auto_slice)) {
                        wxGetApp().app_config->set_bool("auto_slice_preview", auto_slice);
                        wxGetApp().app_config->save();
                    }
                    if (ImGui::IsItemHovered())
                        imgui.tooltip(_u8L("Slice again automatically, 1 s after a change of settings made while the Preview is shown."),
                            ImGui::GetFontSize() * 20.0f);
                }
                ImGui::PopStyleColor(1);
                ImGui::PopStyleVar(1);
                ImGui::SameLine();
                ImGui::SetCursorPosY(last_line_y);
                render_prev_slice_button(prev_slice, show_prev_slice, window_padding * 3);
                ImGui::Dummy({ window_padding, window_padding });
                legend_height = ImGui::GetCurrentWindow()->Size.y;
                if (!m_fold) {
                    ImVec2& seen = s_legend_min_size[int(m_view_type)];
                    seen = ImVec2(std::max(seen.x, ImGui::GetCurrentWindow()->Size.x), std::max(seen.y, ImGui::GetCurrentWindow()->Size.y));
                }
                if (show_prev_slice)
                    ImGui::PopStyleColor(1);
                imgui.end();
                ImGui::PopStyleColor(7);
                ImGui::PopStyleVar(2);
                // Orio : carte de la ligne dont le (i) est survole depuis 1,5 s ou a ete clique, a gauche de la legende
                if (!s_card_any_hovered) {
                    s_card_hover_id = -1;
                    if (!s_card_pinned)
                        s_card_id = -1;
                }
                if (s_card_id >= 0 && s_card_row_drawn) {
                    const LineCardInfo info = line_card_info(s_card_id);
                    const float card_w = 22.0f * ImGui::GetFontSize();
                    ImGui::SetNextWindowPos(ImVec2(s_card_anchor.x - 8.0f * m_scale, s_card_anchor.y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
                    ImGui::SetNextWindowSizeConstraints(ImVec2(card_w, 0.0f), ImVec2(card_w, FLT_MAX));
                    ImGui::SetNextWindowBgAlpha(0.95f);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * m_scale);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * m_scale, 10.0f * m_scale));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(1.0f, 1.5f * m_scale));
                    ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_BAMBU);
                    imgui.begin(std::string("##LineTypeCard"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
                    ImDrawList* card_draw = ImGui::GetWindowDrawList();
                    const float line_h = ImGui::GetTextLineHeight();
                    const ImVec2 swatch = ImGui::GetCursorScreenPos();
                    card_draw->AddRectFilled(ImVec2(swatch.x, swatch.y + 0.15f * line_h), ImVec2(swatch.x + 0.7f * line_h, swatch.y + 0.85f * line_h),
                        ImGui::GetColorU32({ s_card_color[0], s_card_color[1], s_card_color[2], 1.0f }), 2.0f * m_scale);
                    ImGui::Dummy(ImVec2(0.7f * line_h, line_h));
                    ImGui::SameLine();
                    imgui.bold_text(s_card_name);
                    // croix en haut a droite
                    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - line_h);
                    if (ImGui::InvisibleButton("##close_line_card", ImVec2(line_h, line_h))) {
                        s_card_id = -1;
                        s_card_pinned = false;
                    }
                    const ImVec2 x0 = ImGui::GetItemRectMin(), x1 = ImGui::GetItemRectMax();
                    const float inset = 0.25f * line_h;
                    const ImU32 x_color = ImGui::IsItemHovered() ? ImGui::GetColorU32(m_is_dark ? ImVec4(0.0f, 194.0f / 255.0f, 1.0f, 1.0f) : ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.0f)) :
                        ImGui::GetColorU32(ImGuiCol_Text);
                    card_draw->AddLine(ImVec2(x0.x + inset, x0.y + inset), ImVec2(x1.x - inset, x1.y - inset), x_color, 1.5f * m_scale);
                    card_draw->AddLine(ImVec2(x1.x - inset, x0.y + inset), ImVec2(x0.x + inset, x1.y - inset), x_color, 1.5f * m_scale);
                    if (*info.text != 0)
                        ImGui::TextWrapped("%s", _u8L(info.text).c_str());
                    ImGui::Separator();
                    imgui.bold_text(_u8L("For the whole part"));
                    if (info.options.empty())
                        ImGui::TextWrapped("%s", _u8L("No setting for this line type.").c_str());
                    for (const auto& [key, type] : info.options) {
                        const ConfigOptionDef* def = print_config_def.get(key);
                        if (def == nullptr)
                            continue;
                        // Its line in the settings tab only shows with the wave overhangs on: nothing to open without them.
                        if (std::string(key) == "wave_overhang_print_speed") {
                            const auto* waves = wxGetApp().preset_bundle->prints.get_edited_preset().config.option<ConfigOptionBool>("wave_overhangs");
                            if (waves == nullptr || !waves->value)
                                continue;
                        }
                        std::string label = _u8L(def->full_label.empty() ? def->label : def->full_label);
                        // Its group in the settings tab in front ("Line width > Outer wall", "Other layers speed > Outer wall"):
                        // the same label is in several groups. Keys of the searcher: "<type>;<key>", "#0" for the vector ones.
                        Search::OptionsSearcher& searcher = wxGetApp().sidebar().get_searcher();
                        const std::string search_key = std::to_string(int(type)) + ";" + key;
                        // The tab names the fields of the vector ones "<key>#0": open it with that name and its page, as the search
                        // does, or its field is not found and does not blink.
                        std::string jump_key = key;
                        auto group_and_category = searcher.get_group_and_category(search_key);
                        if (group_and_category.group.IsEmpty()) {
                            auto indexed = searcher.get_group_and_category(search_key + "#0");
                            if (!indexed.group.IsEmpty()) {
                                group_and_category = indexed;
                                jump_key += "#0";
                            }
                        }
                        const wxString& group = group_and_category.group;
                        // The overhang speeds are only "10%", "25%"...: the name of their line in the tab says more than their group.
                        const bool overhang_speed = std::string(key).rfind("overhang_", 0) == 0;
                        if (overhang_speed)
                            label = _u8L("Overhang speed") + " > " + label;
                        else if (!group.IsEmpty() && into_u8(_(group)) != label)
                            label = into_u8(_(group)) + " > " + label;
                        if (ImGui::Selectable((label + ": " + line_card_value(key, type) + "##" + key).c_str())) {
                            const Preset::Type opt_type = type;
                            const std::wstring category = group_and_category.category.ToStdWstring();
                            wxGetApp().CallAfter([jump_key, opt_type, category]() { wxGetApp().sidebar().jump_to_option(jump_key, opt_type, category); });
                        }
                        if (ImGui::IsItemHovered())
                            imgui.tooltip((overhang_speed ? _u8L("This is the speed for various overhang degrees. Overhang degrees are expressed as a percentage of line width. 0 speed means no slowing down for the overhang degree range and wall speed is used") + "\n\n" : std::string()) +
                                _u8L("Open this setting in the settings column"), ImGui::GetFontSize() * 20.0f);
                    }
                    imgui.end();
                    ImGui::PopStyleColor(1);
                    ImGui::PopStyleVar(3);
                }
            }

            // Orio : memorise le tranchage affiche, plateau par plateau. Un nouveau resultat sur un plateau
            // (nouvel identifiant) = nouveau tranchage : celui qui etait affiche devient le precedent. Revenir sur
            // un plateau deja vu (meme identifiant) ne change rien. L'instantane est pris au premier affichage, une
            // fois load() et refresh() passes (plages de couleurs calculees, totaux du plateau definitifs).
            SliceSnapshot* BaseRenderer::update_slice_history()
            {
                if (!m_slice_history)
                    m_slice_history = std::make_shared<SliceHistory>();
                // rien de charge (apercu reinitialise ou resultat vide)
                if (m_gcode_result == nullptr || (int)m_last_result_id == -1)
                    return nullptr;
                const int plate_idx = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
                SliceHistory::PlateSlices& plate = m_slice_history->plates[plate_idx];
                if (!plate.last || plate.last->result_id != m_last_result_id) {
                    if (plate.last)
                        plate.previous = std::move(plate.last);
                    auto snapshot = std::make_unique<SliceSnapshot>();
                    snapshot->result_id = m_last_result_id;
                    snapshot->time_label = into_u8(wxDateTime::Now().Format("%H:%M"));
                    snapshot->print_statistics = m_print_statistics;
                    snapshot->print_stats = wxGetApp().plater()->get_partplate_list().get_current_fff_print().print_statistics();
                    snapshot->roles = m_roles;
                    snapshot->extruder_ids = m_extruder_ids;
                    snapshot->filament_diameters = m_filament_diameters;
                    snapshot->filament_densities = m_filament_densities;
                    snapshot->tools = m_tools;
                    if (m_p_extrusions)
                        snapshot->ranges = m_p_extrusions->ranges;
                    if (auto it = m_gcode_result->skippable_part_time.find(SkipType::stTimelapse); it != m_gcode_result->skippable_part_time.end())
                        snapshot->timelapse_time = it->second;
                    plate.last = std::move(snapshot);
                }
                return plate.previous.get();
            }

            // Orio : statistiques de l'objet clique, en un passage sur les mouvements du G-code. Ses extrusions sont
            // celles de son label ; ses trajets, ceux entre deux de ses extrusions de la meme couche (les trajets d'une
            // piece a l'autre, la tour de purge, les purges et le debut ne sont a personne). Temps : les MoveVertex::time
            // sont des secondes entieres (inutilisables mouvement par mouvement) ; chaque mouvement compte pour sa
            // longueur / sa vitesse, et sa part ainsi estimee dans un type de ligne repartit le temps du plateau pour ce
            // type (celui du processeur, affiche par la legende normale).
            bool BaseRenderer::update_picked_stats()
            {
                if (m_picked_label < 0 || m_gcode_result == nullptr || (int)m_last_result_id == -1 || !can_hide_object(m_picked_label))
                    return false;
                if (m_picked_stats && m_picked_stats->result_id == m_last_result_id && m_picked_stats->label == m_picked_label)
                    return true;
                auto s = std::make_shared<PickedStats>();
                s->result_id = m_last_result_id;
                s->label = m_picked_label;
                const ModelObjectPtrs& objects = wxGetApp().plater()->model().objects;
                for (const GLVolume* v : m_shells.volumes.volumes) {
                    const int o = v->object_idx(), i = v->instance_idx();
                    if (!v->is_wipe_tower && o >= 0 && o < int(objects.size()) && i >= 0 && i < int(objects[o]->instances.size()) &&
                        int(objects[o]->instances[i]->get_labeled_id()) == m_picked_label)
                        s->name = objects[o]->name;
                }
                s->ps = wxGetApp().plater()->get_partplate_list().get_current_fff_print().print_statistics();
                s->ps.total_used_filament = s->ps.total_weight = s->ps.total_cost = 0.0;
                PrintEstimatedStatistics& st = s->stats;
                std::map<ExtrusionRole, double> plate_est, part_est;
                double plate_travel_est = 0.0, part_travel_est = 0.0, pending = 0.0;
                int last_label = -1;
                float last_z = -1.0f;
                const std::vector<GCodeProcessorResult::MoveVertex>& moves = m_gcode_result->moves;
                for (size_t i = 1; i < moves.size(); ++i) {
                    const GCodeProcessorResult::MoveVertex& m = moves[i];
                    const double est = m.feedrate > 0.0f ? (m.position - moves[i - 1].position).norm() / m.feedrate : 0.0;
                    if (m.type == EMoveType::Travel) {
                        plate_travel_est += est;
                        pending += est;
                        continue;
                    }
                    if (m.type != EMoveType::Extrude)
                        continue;
                    const bool mine = m.object_label_id == m_picked_label;
                    if (mine && last_label == m_picked_label && std::abs(m.print_z - last_z) < EPSILON)
                        part_travel_est += pending;
                    pending = 0.0;
                    last_label = m.object_label_id;
                    last_z = m.print_z;
                    const ExtrusionRole role = m.extrusion_role;
                    plate_est[role] += est;
                    const size_t f = m.extruder_id;
                    if (!mine || f >= m_filament_diameters.size() || f >= m_filament_densities.size())
                        continue;
                    part_est[role] += est;
                    const double volume = m.delta_extruder * PI * sqr(0.5 * m_filament_diameters[f]);
                    const double grams = volume * m_filament_densities[f] * 0.001;
                    std::pair<double, double>& used = st.used_filaments_per_role[role];
                    used.first += m.delta_extruder * 0.001;
                    used.second += grams;
                    (is_support(role) ? st.support_volumes_per_extruder : st.model_volumes_per_extruder)[f] += volume;
                    s->ps.total_used_filament += m.delta_extruder;
                    s->ps.total_weight += grams;
                    if (f < m_gcode_result->filament_costs.size())
                        s->ps.total_cost += grams * m_gcode_result->filament_costs[f] * 0.001;
                }
                for (const auto& [role, est] : part_est)
                    s->time_share[role] = plate_est[role] > 0.0 ? float(est / plate_est[role]) : 0.0f;
                s->travel_share = plate_travel_est > 0.0 ? float(part_travel_est / plate_travel_est) : 0.0f;
                for (size_t k = 0; k < 2; ++k) {
                    const PrintEstimatedStatistics::Mode& plate = m_print_statistics.modes[k];
                    PrintEstimatedStatistics::Mode& mode = st.modes[k];
                    mode.time = 0.0f;
                    for (const auto& [role, time] : plate.roles_times)
                        if (auto it = s->time_share.find(role); it != s->time_share.end() && it->second > 0.0f) {
                            mode.roles_times.push_back({ role, time * it->second });
                            mode.time += time * it->second;
                        }
                    float travel = 0.0f;
                    for (const auto& [type, time] : plate.moves_times)
                        if (type == EMoveType::Travel)
                            travel = time * s->travel_share;
                    mode.moves_times = { { EMoveType::Travel, travel } };
                    mode.time += travel;
                    // seulement pour la mise en page des temps du bas de la legende
                    mode.layers_times = plate.layers_times;
                }
                // Orio : seulement les types de ligne de la piece (temps ou filament), comme la legende normale
                for (ExtrusionRole role : m_roles)
                    if (part_est.count(role) > 0 || st.used_filaments_per_role.count(role) > 0)
                        s->roles.push_back(role);
                m_picked_stats = s;
                return true;
            }

            // Orio : echange tout ce que lit la legende (hors totaux du plateau et temps de timelapse, lus
            // directement dans l'instantane) ; appele une 2e fois, remet chaque donnee a sa place
            void BaseRenderer::swap_with_slice_snapshot(SliceSnapshot& snapshot)
            {
                std::swap(m_print_statistics, snapshot.print_statistics);
                m_roles.swap(snapshot.roles);
                m_extruder_ids.swap(snapshot.extruder_ids);
                m_filament_diameters.swap(snapshot.filament_diameters);
                m_filament_densities.swap(snapshot.filament_densities);
                std::swap(m_tools, snapshot.tools);
                if (m_p_extrusions)
                    std::swap(m_p_extrusions->ranges, snapshot.ranges);
            }

            // Orio : valeurs de la legende pour les donnees en place, par cle (legend_cell_key), a la
            // precision de l'affichage : une cle dont la valeur differe d'un tranchage a l'autre a un affichage
            // different. Toutes les vues sauf les plages (seulement celle de la vue courante). Vue Filament : un
            // format unique "m g", quelles que soient les colonnes affichees (elles peuvent differer).
            std::map<std::string, std::string> BaseRenderer::legend_value_cells(bool imperial_units, const PrintStatistics& ps, double timelapse_time) const
            {
                std::map<std::string, std::string> cells;
                char buf[128];
                const PrintEstimatedStatistics::Mode& time_mode = m_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)];
                auto time_text = [](float time) { return time > 0.0f ? short_time(get_time_dhms(time)) : std::string(); };
                auto percent_text = [](float percent) {
                    char b[64];
                    if (percent == 0)
                        ::sprintf(b, "0%%");
                    else
                        percent > 0.001 ? ::sprintf(b, "%.1f%%", percent * 100) : ::sprintf(b, "<0.1%%");
                    return std::string(b);
                };

                // vue Type de ligne : temps, pourcentage et filament de chaque role, puis deplacements
                for (ExtrusionRole role : m_roles) {
                    if (role >= erCount)
                        continue;
                    auto it = std::find_if(time_mode.roles_times.begin(), time_mode.roles_times.end(), [role](const std::pair<ExtrusionRole, float>& item) { return role == item.first; });
                    const float time = it != time_mode.roles_times.end() ? it->second : 0.0f;
                    const float percent = it != time_mode.roles_times.end() ? it->second / time_mode.time : 0.0f;
                    cells[legend_cell_key("role", int(role), "time")] = time_text(time);
                    cells[legend_cell_key("role", int(role), "pct")] = percent_text(percent);
                    double used_m = 0.0, used_g = 0.0;
                    if (auto used = m_print_statistics.used_filaments_per_role.find(role); used != m_print_statistics.used_filaments_per_role.end()) {
                        used_m = used->second.first * (imperial_units ? 1000.0 / GizmoObjectManipulation::in_to_mm : 1.0);
                        used_g = used->second.second;
                    }
                    ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", used_m);
                    cells[legend_cell_key("role", int(role), "m")] = buf;
                    ::sprintf(buf, "%.2f g", used_g);
                    cells[legend_cell_key("role", int(role), "g")] = buf;
                }
                {
                    auto it = std::find_if(time_mode.moves_times.begin(), time_mode.moves_times.end(), [](const std::pair<EMoveType, float>& item) { return item.first == EMoveType::Travel; });
                    const float time = it != time_mode.moves_times.end() ? it->second : 0.0f;
                    const float percent = it != time_mode.moves_times.end() ? it->second / time_mode.time : 0.0f;
                    cells[legend_cell_key("travel", 0, "time")] = time_text(time);
                    cells[legend_cell_key("travel", 0, "pct")] = percent_text(percent);
                }

                // vues Filament et Outil : filament par extrudeur (modele, supports, purge, tour) et totaux
                const double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1;
                auto used_from_volume = [this, imperial_units](double volume, int extruder_id) {
                    const double koef = imperial_units ? 1.0 / GizmoObjectManipulation::in_to_mm : 0.001;
                    return std::make_pair(koef * volume / (PI * sqr(0.5 * m_filament_diameters[extruder_id])), volume * m_filament_densities[extruder_id] * 0.001);
                };
                const char* column_names[4] = { "model", "support", "flushed", "tower" };
                const std::map<size_t, double>* column_volumes[4] = { &m_print_statistics.model_volumes_per_extruder, &m_print_statistics.support_volumes_per_extruder,
                    &m_print_statistics.flush_per_filament, &m_print_statistics.wipe_tower_volumes_per_extruder };
                std::array<std::pair<double, double>, 4> column_totals{};
                for (unsigned char extruder_id : m_extruder_ids) {
                    double sum_m = 0.0, sum_g = 0.0;
                    for (int c = 0; c < 4; ++c) {
                        std::pair<double, double> used{ 0.0, 0.0 };
                        if (auto it = column_volumes[c]->find(extruder_id); it != column_volumes[c]->end())
                            used = used_from_volume(it->second, extruder_id);
                        ::sprintf(buf, "%.2f %.2f", used.first, used.second / unit_conver);
                        cells[legend_cell_key("fil", extruder_id, column_names[c])] = buf;
                        if (c == 0) {
                            ::sprintf(buf, "%.2f %.2f", used.first, used.second);
                            cells[legend_cell_key("tool", extruder_id, "used")] = buf;
                        }
                        sum_m += used.first;
                        sum_g += used.second;
                        column_totals[c].first += used.first;
                        column_totals[c].second += used.second;
                    }
                    ::sprintf(buf, "%.2f %.2f", sum_m, sum_g / unit_conver);
                    cells[legend_cell_key("fil", extruder_id, "total")] = buf;
                }
                double all_m = 0.0, all_g = 0.0;
                for (int c = 0; c < 4; ++c) {
                    ::sprintf(buf, "%.2f %.2f", column_totals[c].first, column_totals[c].second / unit_conver);
                    cells[legend_cell_key("fil", -1, column_names[c])] = buf;
                    all_m += column_totals[c].first;
                    all_g += column_totals[c].second;
                }
                ::sprintf(buf, "%.2f %.2f", all_m, all_g / unit_conver);
                cells[legend_cell_key("fil", -1, "total")] = buf;
                ::sprintf(buf, "%d", m_print_statistics.total_filament_changes);
                cells[legend_cell_key("fil", -1, "changes")] = buf;

                // totaux du plateau et temps (estimation totale, resume)
                const double koef = imperial_units ? GizmoObjectManipulation::in_to_mm : 1000.0;
                const double exclude_m = column_totals[1].first + column_totals[2].first + column_totals[3].first;
                const double exclude_g = column_totals[1].second + column_totals[2].second + column_totals[3].second;
                ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef);
                cells[legend_cell_key("est", 0, "total_m")] = buf;
                ::sprintf(buf, imperial_units ? "  %.2f oz" : "  %.2f g", ps.total_weight / unit_conver);
                cells[legend_cell_key("est", 0, "total_g")] = buf;
                ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef - exclude_m);
                cells[legend_cell_key("est", 0, "model_m")] = buf;
                ::sprintf(buf, imperial_units ? "  %.2f oz" : "  %.2f g", (ps.total_weight - exclude_g) / unit_conver);
                cells[legend_cell_key("est", 0, "model_g")] = buf;
                ::sprintf(buf, imperial_units ? "%.2f in / %.2f oz" : "%.2f m / %.2f g", ps.total_used_filament / koef, ps.total_weight / unit_conver);
                cells[legend_cell_key("sum", 0, "total")] = buf;
                ::sprintf(buf, "%.2f", ps.total_cost);
                cells[legend_cell_key("ps", 0, "cost")] = buf;
                cells[legend_cell_key("time", 0, "prepare")] = timelapse_time != 0.0 ?
                    short_time(get_time_dhms(time_mode.prepare_time)) + " + " + short_time(get_time_dhms(timelapse_time)) :
                    short_time(get_time_dhms(time_mode.prepare_time));
                cells[legend_cell_key("time", 0, "print")] = short_time(get_time_dhms(time_mode.time - time_mode.prepare_time - timelapse_time));
                cells[legend_cell_key("time", 0, "total")] = short_time(get_time_dhms(time_mode.time));

                // vue a plage de couleurs : valeur de chaque pas, cle = indice de couleur (comme append_range)
                const Range* range = nullptr;
                unsigned int decimals = 0;
                if (m_p_extrusions) {
                    const Ranges& ranges = m_p_extrusions->ranges;
                    switch (m_view_type) {
                    case EViewType::Height: { range = &ranges.height; decimals = 2; break; }
                    case EViewType::Width: { range = &ranges.width; decimals = 2; break; }
                    case EViewType::Feedrate: { range = &ranges.feedrate; break; }
                    case EViewType::FanSpeed: { range = &ranges.fan_speed; break; }
                    case EViewType::AdditionalFanSpeed: { range = &ranges.additional_fan_speed; break; }
                    case EViewType::Temperature: { range = &ranges.temperature; break; }
                    case EViewType::LayerTime: { range = &ranges.layer_duration; decimals = 1; break; }
                    case EViewType::VolumetricRate: { range = &ranges.volumetric_rate; decimals = 2; break; }
                    default: { break; }
                    }
                }
                if (range != nullptr) {
                    auto add_step = [&cells, &buf, decimals](int i, float value) {
                        ::sprintf(buf, "%.*f", decimals, value);
                        cells[legend_cell_key("range", i, "value")] = buf;
                    };
                    const int colors = static_cast<int>(range->range_colors.size());
                    if (range->count == 1)
                        add_step(0, range->min);
                    else if (range->count == 2) {
                        add_step(colors - 1, range->max);
                        add_step(0, range->min);
                    }
                    else {
                        for (int i = colors - 1; i >= 0; --i)
                            add_step(i, range->get_value_at_step(i));
                    }
                }
                return cells;
            }

            // Orio : bouton "tranchage precedent" (icone historique). Tant qu'il est maintenu, la legende
            // affiche le tranchage d'avant de ce plateau ; grise tant qu'il n'y en a pas. Dessine en dernier, sur une
            // ligne neuve : son etat (survol, appui) sert a l'image suivante.
            void BaseRenderer::render_prev_slice_button(const SliceSnapshot* prev_slice, bool showing_prev, float right_margin)
            {
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                SliceHistory& history = *m_slice_history;
                const bool was_held = history.held;
                const bool was_hovered = history.hovered;
                const bool enabled = prev_slice != nullptr;
                const float size = ImGui::GetFrameHeight();
                // A droite : bord droit a right_margin du contenu le plus large de cette image. Pas du bord de la
                // fenetre : elle s'ajuste a son contenu et ne pourrait plus retrecir si le bouton le touchait.
                ImGuiWindow* window = ImGui::GetCurrentWindow();
                const float content_right = window->DC.CursorMaxPos.x - window->Pos.x + window->Scroll.x;
                ImVec2 pos(std::max(ImGui::GetCursorPosX(), content_right - right_margin - size), ImGui::GetCursorPosY());
                // Tranchage precedent affiche : si sa legende est plus courte ou plus etroite, le bouton reste ou il
                // etait au lieu de remonter ou de reculer sous le doigt (la fenetre garde alors sa taille)
                if (showing_prev) {
                    pos.x = std::max(pos.x, history.button_pos.x);
                    pos.y = std::max(pos.y, history.button_pos.y);
                }
                else
                    history.button_pos = pos;
                ImGui::SetCursorPos(pos);
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 p1(p0.x + size, p0.y + size);
                ImGui::InvisibleButton("##prev_slice", ImVec2(size, size));
                const bool hovered = ImGui::IsItemHovered();
                history.held = enabled && ImGui::IsItemActive();
                history.hovered = enabled && hovered;

                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImU32 icon_color = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                if (history.held) {
                    draw_list->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(1.00f, 0.71f, 0.20f, 1.00f)), 3.0f * m_scale);
                    icon_color = ImGui::GetColorU32(ImVec4(0.12f, 0.12f, 0.12f, 1.00f));
                }
                else if (hovered && enabled) {
                    draw_list->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.00f)), 3.0f * m_scale);
                    icon_color = ImGui::GetColorU32(ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
                }
                // cercle ouvert entre 8 h et 10 h, parcouru par le haut ; pointe de fleche a 10 h tournee vers la
                // breche (sens inverse des aiguilles d'une montre = retour en arriere) ; deux aiguilles d'horloge
                const ImVec2 center(0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y));
                const float radius = 0.30f * size;
                const float thickness = std::max(1.0f, 1.5f * m_scale);
                const float a_start = float(PI) + 0.55f;
                const float a_end = float(PI) - 0.45f + 2.0f * float(PI);
                draw_list->PathArcTo(center, radius, a_start, a_end, 24);
                draw_list->PathStroke(icon_color, 0, thickness);
                const ImVec2 radial(std::cos(a_start), std::sin(a_start));
                const ImVec2 tangent(radial.y, -radial.x);
                const ImVec2 arrow_base(center.x + radius * radial.x, center.y + radius * radial.y);
                const float arrow = 0.16f * size;
                draw_list->AddTriangleFilled(ImVec2(arrow_base.x + arrow * tangent.x, arrow_base.y + arrow * tangent.y),
                    ImVec2(arrow_base.x + 0.8f * arrow * radial.x, arrow_base.y + 0.8f * arrow * radial.y),
                    ImVec2(arrow_base.x - 0.8f * arrow * radial.x, arrow_base.y - 0.8f * arrow * radial.y), icon_color);
                draw_list->AddLine(center, ImVec2(center.x, center.y - 0.55f * radius), icon_color, thickness);
                draw_list->AddLine(center, ImVec2(center.x + 0.45f * radius, center.y + 0.30f * radius), icon_color, thickness);

                if (hovered && !history.held)
                    imgui.tooltip(enabled ? _u8L("Hold to show the previous slice of this plate. Values that differ are highlighted.") : _u8L("No previous slice for this plate yet"),
                        ImGui::GetFontSize() * 20.0f);

                // appui et relachement changent le contenu de la legende, puis sa taille : deux images de plus ;
                // survol pris ou perdu : la surbrillance apparait ou disparait, une image de plus
                if (history.held != was_held)
                    history.extra_frames = 2;
                else if (history.hovered != was_hovered)
                    history.extra_frames = std::max(history.extra_frames, 1);
                if (history.extra_frames > 0) {
                    --history.extra_frames;
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                    imgui.set_requires_extra_frame();
#else
                    wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                    wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                }
            }

            void BaseRenderer::delete_wipe_tower()
            {
                size_t current_volumes_count = m_shells.volumes.volumes.size();
                if (current_volumes_count >= 1) {
                    for (size_t i = current_volumes_count - 1; i > 0; i--) {
                        GLVolume* v = m_shells.volumes.volumes[i];
                        if (v->is_wipe_tower) {
                            m_shells.volumes.release_volume(v);
                            delete v;
                            m_shells.volumes.volumes.erase(m_shells.volumes.volumes.begin() + i);
                            break;
                        }
                    }
                }
            }

            void BaseRenderer::render_legend_color_arr_recommen(float window_padding, bool is_show_left_right_result)
            {
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                auto link_text = [&](const std::string& label) {
                    ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());
                    ImColor HyperColor = ImColor(8, 119, 232, 255).Value;
                    ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
                    imgui.text(label.c_str());
                    ImGui::PopStyleColor();
                    // underline
                    ImVec2 lineEnd = ImGui::GetItemRectMax();
                    lineEnd.y -= 2.0f;
                    ImVec2 lineStart = lineEnd;
                    lineStart.x = ImGui::GetItemRectMin().x;
                    ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
                    // click behavior
                    if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            Plater* plater = wxGetApp().plater();
                            wxCommandEvent evt(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG);
                            evt.SetEventObject(plater);
                            evt.SetInt(1); // 1 means from gcode viewer
                            wxPostEvent(plater, evt);
                        }
                    }
                    };
                auto link_text_set_to_optional = [&](const std::string& label) {
                    ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());
                    ImColor HyperColor = ImColor(8, 119, 232, 255).Value;
                    ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
                    imgui.text(label.c_str());
                    ImGui::PopStyleColor();
                    // underline
                    ImVec2 lineEnd = ImGui::GetItemRectMax();
                    lineEnd.y -= 2.0f;
                    ImVec2 lineStart = lineEnd;
                    lineStart.x = ImGui::GetItemRectMin().x;
                    ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
                    // click behavior
                    if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            MessageDialog msg_dlg(nullptr, _L("Automatically re-slice according to the optimal filament grouping, and the grouping results will be displayed after slicing."), wxEmptyString, wxOK | wxCANCEL);
                            if (msg_dlg.ShowModal() == wxID_OK) {
                                PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
                                PartPlate* plate = partplate_list.get_curr_plate();
                                plate->set_filament_map_mode(FilamentMapMode::fmmAutoForFlush);
                                Plater* plater = wxGetApp().plater();
                                wxPostEvent(plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
                            }
                        }
                    }
                    };
                auto link_filament_group_wiki = [&](const std::string& label) {
                    ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());
                    ImColor HyperColor = ImColor(8, 119, 232, 255).Value;
                    ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
                    imgui.text(label.c_str());
                    ImGui::PopStyleColor();
                    // click behavior
                    if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            open_filament_group_wiki();
                        }
                    }
                    };
                auto draw_dash_line = [&](ImDrawList* draw_list, int dash_length = 5, int gap_length = 3) {
                    ImVec2 p1 = ImGui::GetCursorScreenPos();
                    ImVec2 p2 = ImVec2(p1.x + ImGui::GetContentRegionAvail().x, p1.y);
                    for (float i = p1.x; i < p2.x; i += (dash_length + gap_length)) {
                        draw_list->AddLine(ImVec2(i, p1.y), ImVec2(i + dash_length, p1.y), IM_COL32(206, 206, 206, 255));
                    }
                    };
                ////BBS Color Arrangement Recommendation
                auto config = wxGetApp().plater()->get_partplate_list().get_current_fff_print().config();
                auto stats_by_extruder = wxGetApp().plater()->get_partplate_list().get_current_fff_print().statistics_by_extruder();
                float delta_weight_to_single_ext = stats_by_extruder.stats_by_single_extruder.filament_flush_weight - stats_by_extruder.stats_by_multi_extruder_curr.filament_flush_weight;
                float delta_weight_to_best = stats_by_extruder.stats_by_multi_extruder_curr.filament_flush_weight - stats_by_extruder.stats_by_multi_extruder_best.filament_flush_weight;
                int   delta_change_to_single_ext = stats_by_extruder.stats_by_single_extruder.flush_filament_change_count - stats_by_extruder.stats_by_multi_extruder_curr.flush_filament_change_count;
                int   delta_change_to_best = stats_by_extruder.stats_by_multi_extruder_curr.flush_filament_change_count - stats_by_extruder.stats_by_multi_extruder_best.flush_filament_change_count;
                bool any_less_to_single_ext = delta_weight_to_single_ext > EPSILON || delta_change_to_single_ext > 0;
                bool any_more_to_best = delta_weight_to_best > EPSILON || delta_change_to_best > 0;
                bool all_less_to_single_ext = delta_weight_to_single_ext > EPSILON && delta_change_to_single_ext > 0;
                bool all_more_to_best = delta_weight_to_best > EPSILON && delta_change_to_best > 0;
                auto get_filament_display_type = [](const ExtruderFilament& filament) {
                    if (filament.is_support_filament && (filament.type == "PLA" || filament.type == "PA" || filament.type == "ABS"))
                        return "Sup." + filament.type;
                    return filament.type;
                    };
                // BBS AMS containers
                float line_height = ImGui::GetFrameHeight();
                float ams_item_height = 0;
                float filament_group_item_align_width = 0;
                {
                    float three_words_width = imgui.calc_text_size("ABC").x;
                    const int line_capacity = 4;
                    for (const auto& extruder_filaments : { m_left_extruder_filament,m_right_extruder_filament })
                    {
                        float container_height = 0.f;
                        for (size_t idx = 0; idx < extruder_filaments.size(); idx += line_capacity) {
                            float text_line_height = 0;
                            for (int j = idx; j < extruder_filaments.size() && j < idx + line_capacity; ++j) {
                                auto text_info = imgui.calculate_filament_group_text_size(get_filament_display_type(extruder_filaments[j]));
                                auto text_size = std::get<0>(text_info);
                                filament_group_item_align_width = max(filament_group_item_align_width, text_size.x);
                                text_line_height = max(text_line_height, text_size.y);
                            }
                            container_height += (three_words_width * 1.3f + text_line_height);
                        }
                        container_height += 2 * line_height;
                        ams_item_height = std::max(ams_item_height, container_height);
                    }
                }
                int tips_count = 8;
                if (any_more_to_best) {
                    tips_count = 8;
                    if (wxGetApp().app_config->get("language") != "zh_CN")
                        tips_count += 1;
                }
                else if (any_less_to_single_ext) {
                    tips_count = 6;
                    if (wxGetApp().app_config->get("language") != "zh_CN")
                        tips_count += 1;
                }
                else
                    tips_count = 5;
                float AMS_container_height = is_show_left_right_result ? line_height * (tips_count - 3) + line_height / 2 :
                                                                    ams_item_height + line_height * tips_count + line_height / 2;
                is_show_left_right_result ? ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.3f, 0.3f, 0.3f, 0.1f)) :
                                       ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.f, 1.f, 1.f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.15f, .18f, .19f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(window_padding * 3, 0));
                // ImGui::Dummy({window_padding, window_padding});
                ImGui::BeginChild("#AMS", ImVec2(0, AMS_container_height), true, ImGuiWindowFlags_AlwaysUseWindowPadding);
                {
                    if (!is_show_left_right_result) {
                        float available_width = ImGui::GetContentRegionAvail().x;
                        float half_width      = available_width * 0.49f;
                        float spacing         = 18.0f * m_scale;
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(.8f, .8f, .8f, 1.0f));
                        imgui.bold_text(_u8L("Filament Grouping"));
                        ImGui::SameLine();
                        std::string tip_str = _u8L("Why this grouping");
                        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionWidth() - window_padding - ImGui::CalcTextSize(tip_str.c_str()).x);
                        link_filament_group_wiki(tip_str);
                        ImGui::Separator();
                        ImGui::PopStyleColor();
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.00f, 0.00f, 0.00f, 0.1f));
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(window_padding * 2, window_padding));
                        ImDrawList *child_begin_draw_list = ImGui::GetWindowDrawList();
                        ImVec2      cursor_pos            = ImGui::GetCursorScreenPos();
                        child_begin_draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + half_width, cursor_pos.y + line_height), IM_COL32(0, 0, 0, 20));
                        std::string br_pt = wxGetApp().preset_bundle->printers.get_edited_preset().get_printer_type(wxGetApp().preset_bundle);
                        ImGui::BeginChild("#LeftAMS", ImVec2(half_width, ams_item_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
                        {
                            std::string br_dep_nz = DevPrinterConfigUtil::get_toolhead_display_name(br_pt, DEPUTY_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::SentenceCase);
                            imgui.text(_u8L(br_dep_nz.c_str()));
                            ImGui::Dummy({window_padding, window_padding});
                            int index = 1;
                            for (const auto &extruder_filament : m_left_extruder_filament) {
                                imgui.filament_group(get_filament_display_type(extruder_filament), extruder_filament.hex_color.c_str(), extruder_filament.filament_id,
                                                     filament_group_item_align_width);
                                if (index % 4 != 0) { ImGui::SameLine(0, spacing); }
                                index++;
                            }
                            ImGui::EndChild();
                        }
                        ImGui::SameLine();
                        cursor_pos = ImGui::GetCursorScreenPos();
                        child_begin_draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + half_width, cursor_pos.y + line_height), IM_COL32(0, 0, 0, 20));
                        ImGui::BeginChild("#RightAMS", ImVec2(half_width, ams_item_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
                        {
                            std::string br_main_nz = DevPrinterConfigUtil::get_toolhead_display_name(br_pt, MAIN_EXTRUDER_ID, ToolHeadComponent::Nozzle, ToolHeadNameCase::SentenceCase);
                            imgui.text(_u8L(br_main_nz.c_str()));
                            ImGui::Dummy({window_padding, window_padding});
                            int index = 1;
                            for (const auto &extruder_filament : m_right_extruder_filament) {
                                imgui.filament_group(get_filament_display_type(extruder_filament), extruder_filament.hex_color.c_str(), extruder_filament.filament_id,
                                                     filament_group_item_align_width);
                                if (index % 4 != 0) { ImGui::SameLine(0, spacing); }
                                index++;
                            }
                            ImGui::EndChild();
                        }
                        ImGui::PopStyleColor(1);
                        ImGui::PopStyleVar(1);
                        ImGui::Dummy({ window_padding, window_padding });
                        imgui.text_wrapped(from_u8(_u8L("Please place filaments on the printer based on grouping result.")), ImGui::GetContentRegionAvail().x);
                    }
                    ImGui::Dummy({ window_padding, window_padding });
                    {
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        draw_dash_line(draw_list);
                    }
                    ImGui::Dummy({ window_padding, window_padding });
                    bool is_optimal_group = true;
                    float parent_width = ImGui::GetContentRegionAvail().x;
                    auto number_format = [](float num) {
                        if (num > 1000) {
                            std::string number_str = std::to_string(num);
                            std::string first_three_digits = number_str.substr(0, 3);
                            return std::stoi(first_three_digits);
                        }
                        return static_cast<int>(num);
                        };
                    if (any_more_to_best) {
                        is_optimal_group = false;
                        ImVec4 orangeColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, orangeColor);
                        imgui.text(_u8L("Tips:"));
                        imgui.text(_u8L("Current grouping of slice result is not optimal."));
                        wxString tip;
                        if (delta_weight_to_best >= 0 && delta_change_to_best >= 0)
                            tip = from_u8((boost::format(_u8L("Increase %1%g filament and %2% changes compared to optimal grouping."))
                                % number_format(delta_weight_to_best)
                                % delta_change_to_best).str());
                        else if (delta_weight_to_best >= 0 && delta_change_to_best < 0)
                            tip = from_u8((boost::format(_u8L("Increase %1%g filament and save %2% changes compared to optimal grouping."))
                                % number_format(delta_weight_to_best)
                                % std::abs(delta_change_to_best)).str());
                        else if (delta_weight_to_best < 0 && delta_change_to_best >= 0)
                            tip = from_u8((boost::format(_u8L("Save %1%g filament and increase %2% changes compared to optimal grouping."))
                                % number_format(std::abs(delta_weight_to_best))
                                % delta_change_to_best).str());
                        imgui.text_wrapped(tip, parent_width);
                        ImGui::PopStyleColor(1);
                    }
                    else if (any_less_to_single_ext) {
                        ImVec4 color = is_show_left_right_result ? ImVec4(0.95f, 0.95f, 0.95f, 1.0f) : ImVec4(0.42f, 0.42f, 0.42f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, color);
                        wxString tip;
                        if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext >= 0)
                            tip = from_u8((boost::format(_u8L("Save %1%g filament and %2% changes compared to a printer with one nozzle."))
                                % number_format(delta_weight_to_single_ext)
                                % delta_change_to_single_ext).str());
                        else if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext < 0)
                            tip = from_u8((boost::format(_u8L("Save %1%g filament and increase %2% changes compared to a printer with one nozzle."))
                                % number_format(delta_weight_to_single_ext)
                                % std::abs(delta_change_to_single_ext)).str());
                        else if (delta_weight_to_single_ext < 0 && delta_change_to_single_ext >= 0)
                            tip = from_u8((boost::format(_u8L("Increase %1%g filament and save %2% changes compared to a printer with one nozzle."))
                                % number_format(std::abs(delta_weight_to_single_ext))
                                % delta_change_to_single_ext).str());
                        imgui.text_wrapped(tip, parent_width);
                        ImGui::PopStyleColor(1);
                    }
                    ImGui::Dummy({ window_padding, window_padding });
                    if (!is_optimal_group) {
                        link_text_set_to_optional(_u8L("Set to Optimal"));
                        ImGui::SameLine();
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                    }
                    link_text(_u8L("Regroup filament"));
                    ImGui::EndChild();
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(1);
            }

            void BaseRenderer::update_moves_slider(bool set_to_max)
            {
                const auto& p_sequential_view = get_sequential_view();
                if (!p_sequential_view) {
                    return;
                }
                // this should not be needed, but it is here to try to prevent rambling crashes on Mac Asan
                if (p_sequential_view->endpoints.last < p_sequential_view->endpoints.first) return;
                std::vector<double> values(p_sequential_view->endpoints.last - p_sequential_view->endpoints.first + 1);
                unsigned int        count = 0;
                for (unsigned int i = p_sequential_view->endpoints.first; i <= p_sequential_view->endpoints.last; ++i) {
                    values[count] = static_cast<double>(i + 1);
                    ++count;
                }
                m_moves_slider->SetSliderValues(values);
                m_moves_slider->SetMaxValue(p_sequential_view->endpoints.last - p_sequential_view->endpoints.first);
                m_moves_slider->SetSelectionSpan(p_sequential_view->current.first - p_sequential_view->endpoints.first, p_sequential_view->current.last - p_sequential_view->endpoints.first);
                if (set_to_max)
                    m_moves_slider->SetHigherValue(m_moves_slider->GetMaxValue());
            }

            bool BaseRenderer::show_sequential_view() const
            {
                return false;
            }

            void BaseRenderer::on_visibility_changed()
            {
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
            }

            void BaseRenderer::apply_view_type_selection(int view_type_sel, EViewType type)
            {
                m_view_type_sel = view_type_sel;
                m_last_option_item = view_type_sel;
                record_gcodeviewer_option_item();
                set_view_type(type);
                reset_visible(type);
                refresh_render_paths();
                update_moves_slider();
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
            }

            void BaseRenderer::do_set_view_type(EViewType type)
            {
                m_view_type = type;
            }

            void BaseRenderer::update_default_view_type()
            {
                if (view_type_items.empty()) { return; }
                if (m_last_option_item >= (int)view_type_items.size()) { return; }
                //Orio: "Line Type" (was "Filament") until the user chooses another color scheme.
                EViewType cur_type = m_nozzle_nums > 1 ? EViewType::Summary : EViewType::FeatureType;
                if (m_last_option_item < 0) {//not set
                    m_view_type_sel = std::distance(view_type_items.begin(), std::find(view_type_items.begin(), view_type_items.end(), cur_type));
                    m_last_option_item = m_view_type_sel;
                    record_gcodeviewer_option_item();
                }
                else {
                    m_view_type_sel = m_last_option_item;
                    cur_type = view_type_items[m_view_type_sel];
                }
                set_view_type(cur_type);
            }

            void BaseRenderer::record_gcodeviewer_option_item() {
                if (GUI::wxGetApp().app_config->get_bool("enable_record_gcodeviewer_option_item")) {
                    wxGetApp().app_config->set("gcodeviewer_option_item", std::to_string(m_last_option_item));
                    wxGetApp().app_config->save();
                }
            }

            void BaseRenderer::push_combo_style()
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0, 8.0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.0f, 0.0f, 0.0f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.0f));
            }
            void BaseRenderer::pop_combo_style()
            {
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(8);
            }

            void BaseRenderer::update_by_mode(ConfigOptionMode mode)
            {
                view_type_items.clear();
                view_type_image_names.clear();
                options_items.clear();
                // BBS initialzed view_type items
                view_type_items.push_back(EViewType::Summary);
                view_type_items.push_back(EViewType::FeatureType);
                view_type_items.push_back(EViewType::ColorPrint);
                view_type_items.push_back(EViewType::Feedrate);
                view_type_items.push_back(EViewType::Height);
                view_type_items.push_back(EViewType::Width);
                view_type_items.push_back(EViewType::VolumetricRate);
                view_type_items.push_back(EViewType::LayerTime);
                view_type_items.push_back(EViewType::FanSpeed);
                view_type_items.push_back(EViewType::AdditionalFanSpeed);
                view_type_items.push_back(EViewType::Temperature);
                // Orio : avant FilamentId (reste le dernier), apres les autres : les indices memorises ne bougent pas
                if (supports_layer_fusion())
                    view_type_items.push_back(EViewType::LayerFusion);
                for (int i = 0; i < view_type_items.size(); i++) {
                    if (view_type_items[i] == EViewType::FilamentId) {
                        continue;
                    }
                    ImageName temp = { get_view_type_string(view_type_items[i]), nullptr, nullptr };
                    view_type_image_names.push_back(temp);
                }
                view_type_items.push_back(EViewType::FilamentId); // BBS for first layer inspection
                options_items.push_back(EMoveType::Travel);
                options_items.push_back(EMoveType::Retract);
                options_items.push_back(EMoveType::Unretract);
                options_items.push_back(EMoveType::Wipe);
                //if (mode == ConfigOptionMode::comDevelop) {
                //    options_items.push_back(EMoveType::Tool_change);
                //}
                //BBS: seam is not real move and extrusion, put at last line
                options_items.push_back(EMoveType::Seam);
            }

            GCodeWindow::GCodeWindow()
            {

            }

            GCodeWindow::~GCodeWindow()
            {
                stop_mapping_file();
            }

            void GCodeWindow::reset() {
                stop_mapping_file();
                m_lines_ends.clear();
                m_lines_ends.shrink_to_fit();
                m_lines.clear();
                m_lines.shrink_to_fit();
                m_filename.clear();
                m_filename.shrink_to_fit();
            }

            void GCodeWindow::toggle_visibility()
            {
                m_visible = !m_visible;
            }

            void GCodeWindow::load_gcode(const std::string& filename, const std::vector<size_t>& lines_ends)
            {
                assert(!m_file.is_open());
                if (m_file.is_open())
                    return;
                m_filename = filename;
                m_lines_ends = lines_ends;
                m_selected_line_id = 0;
                m_last_lines_size = 0;
                try
                {
                    m_file.open(boost::filesystem::path(m_filename));
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": mapping file " << PathSanitizer::sanitize(m_filename);
                }
                catch (...)
                {
                    BOOST_LOG_TRIVIAL(error) << "Unable to map file " << PathSanitizer::sanitize(m_filename) << ". Cannot show G-code window.";
                    reset();
                }
            }

            //BBS: GUI refactor: move to right
            void GCodeWindow::render(float top, float bottom, float right, uint64_t curr_line_id) const
                //void LegacyRenderer::SequentialView::GCodeWindow::render(float top, float bottom, uint64_t curr_line_id) const
            {
                auto update_lines = [this](uint64_t start_id, uint64_t end_id) {
                    std::vector<Line> ret;
                    ret.reserve(end_id - start_id + 1);
                    const size_t length_of_line = 40;
                    for (uint64_t id = start_id; id <= end_id; ++id) {
                        // read line from file
                        const size_t start = id == 1 ? 0 : m_lines_ends[id - 2];
                        const size_t original_len = m_lines_ends[id - 1] - start;
                        const size_t len = std::min(original_len, (size_t)length_of_line);
                        std::string gline(m_file.data() + start, len);
                        std::string command;
                        std::string parameters;
                        std::string comment;

                        // If original line is longer than 55 characters, truncate and append "..."
                        if (original_len > length_of_line)
                            gline = gline.substr(0, length_of_line - 3) + "...";

                        // extract comment
                        std::vector<std::string> tokens;
                        boost::split(tokens, gline, boost::is_any_of(";"), boost::token_compress_on);
                        command = tokens.front();
                        if (tokens.size() > 1) {
                            comment = ";" + tokens.back();
                        }

                        // extract gcode command and parameters
                        if (!command.empty()) {
                            boost::split(tokens, command, boost::is_any_of(" "), boost::token_compress_on);
                            command = tokens.front();
                            if (tokens.size() > 1) {
                                for (size_t i = 1; i < tokens.size(); ++i) {
                                    parameters += " " + tokens[i];
                                }
                            }
                        }

                        boost::trim(command);
                        boost::trim(parameters);
                        boost::trim(comment);
                        ret.push_back({ command, parameters, comment });
                    }
                    return ret;
                };
                static const ImVec4 LINE_NUMBER_COLOR = { 8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.0f };
                static const ImVec4 SELECTION_RECT_COLOR = { 8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.0f };
                static const ImVec4 COMMAND_COLOR = m_is_dark ? ImVec4(240.0f / 255.0f, 240.0f / 255.0f, 240.0f / 255.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                static const ImVec4 PARAMETERS_COLOR = m_is_dark ? ImVec4(179.0f / 255.0f, 179.0f / 255.0f, 179.0f / 255.0f, 1.0f) : ImVec4(206.0f / 255.0f, 206.0f / 255.0f, 206.0f / 255.0f, 1.0f);
                static const ImVec4 COMMENT_COLOR = m_is_dark ? ImVec4(129.0f / 255.0f, 129.0f / 255.0f, 129.0f / 255.0f, 1.0f) : ImVec4(172.0f / 255.0f, 172.0f / 255.0f, 172.0f / 255.0f, 1.0f);
                // Orio : masquee par la case "Afficher le G-code" de la legende
                if (!m_visible || !wxGetApp().app_config->get_bool("show_gcode_window") || m_filename.empty() || m_lines_ends.empty() || curr_line_id == 0)
                    return;
                const float text_height = ImGui::CalcTextSize("0").y;
                const ImGuiStyle& style = ImGui::GetStyle();
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                // Orio : fenetre deplacable (glissee n'importe ou) et agrandissable (par son coin inferieur droit, en
                // vert), a la place et a la taille laissees par l'utilisateur (app config "gcode_window_rect" : x y largeur
                // hauteur, pixels) ; sinon a droite, sous la legende, jusqu'au curseur des mouvements. Le nombre de lignes suit
                // sa hauteur.
                static float  s_rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // largeur 0 : pas de place choisie
                static bool   s_loaded  = false;
                static ImVec2 s_shown_pos, s_shown_size;              // la ou elle est apparue, ou laissee
                if (!s_loaded) {
                    s_loaded = true;
                    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
                    if (std::sscanf(wxGetApp().app_config->get("gcode_window_rect").c_str(), "%f %f %f %f", &x, &y, &w, &h) == 4 && w > 0.0f && h > 0.0f) {
                        s_rect[0] = x;
                        s_rect[1] = y;
                        s_rect[2] = w;
                        s_rect[3] = h;
                    }
                }
                const ImVec2 display = ImGui::GetIO().DisplaySize;
                const ImVec2 min_size(ImGui::CalcTextSize("000000 G1 X000").x + 2.0f * style.WindowPadding.x,
                    6.0f * (text_height + style.ItemSpacing.y) + 2.0f * style.WindowPadding.y);
                const ImVec2 max_size(std::max(min_size.x, display.x), std::max(min_size.y, display.y));
                ImVec2 size = s_rect[2] > 0.0f ? ImVec2(s_rect[2], s_rect[3]) :
                    ImVec2(ImGui::CalcTextSize(std::string(48, '0').c_str()).x + 2.0f * style.WindowPadding.x, bottom - top);
                size.x = std::clamp(size.x, min_size.x, max_size.x);
                size.y = std::clamp(size.y, min_size.y, max_size.y);
                ImVec2 pos = s_rect[2] > 0.0f ? ImVec2(s_rect[0], s_rect[1]) : ImVec2(right - size.x, top);
                pos.x = std::clamp(pos.x, 0.0f, std::max(0.0f, display.x - size.x));
                pos.y = std::clamp(pos.y, 0.0f, std::max(0.0f, display.y - size.y));
                ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
                ImGui::SetNextWindowSizeConstraints(min_size, max_size);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::SetNextWindowBgAlpha(0.8f);
                // son coin inferieur droit, qui l'agrandit, en vert (ImGui le dessine dans Begin)
                const ImVec4 grip = ImGuiWrapper::COL_BAMBU;
                ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(grip.x, grip.y, grip.z, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(grip.x, grip.y, grip.z, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, grip);
                imgui.begin(std::string("G-code"), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
                ImGui::PopStyleColor(3);
                {
                    ImVec2 wpos  = ImGui::GetWindowPos();
                    const ImVec2 wsize = ImGui::GetWindowSize();
                    // gardee dans la vue
                    const ImVec2 kept(std::clamp(wpos.x, 0.0f, std::max(0.0f, display.x - wsize.x)), std::clamp(wpos.y, 0.0f, std::max(0.0f, display.y - wsize.y)));
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && (kept.x != wpos.x || kept.y != wpos.y)) {
                        ImGui::SetWindowPos(kept);
                        wpos = kept;
                    }
                    if (ImGui::IsWindowAppearing()) {
                        s_shown_pos  = wpos;
                        s_shown_size = wsize;
                    }
                    // deplacee ou agrandie par l'utilisateur, relachee : gardee
                    else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                        (std::abs(wpos.x - s_shown_pos.x) >= 1.0f || std::abs(wpos.y - s_shown_pos.y) >= 1.0f ||
                         std::abs(wsize.x - s_shown_size.x) >= 1.0f || std::abs(wsize.y - s_shown_size.y) >= 1.0f)) {
                        s_shown_pos  = wpos;
                        s_shown_size = wsize;
                        s_rect[0] = wpos.x;
                        s_rect[1] = wpos.y;
                        s_rect[2] = wsize.x;
                        s_rect[3] = wsize.y;
                        wxGetApp().app_config->set("gcode_window_rect", std::to_string(int(wpos.x)) + " " + std::to_string(int(wpos.y)) + " " +
                            std::to_string(int(wsize.x)) + " " + std::to_string(int(wsize.y)));
                    }
                }
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                // en-tete : bandeau sombre et titre
                const ImVec2 header_min = ImGui::GetWindowPos();
                const ImVec2 header_max(header_min.x + ImGui::GetWindowWidth(), ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() + 4.0f);
                draw_list->AddRectFilled(header_min, header_max, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.3)));
                imgui.text("G-code");
                if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(header_min, header_max) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const std::string tip = _u8L("Drag to move the window, drag its green corner to resize it");
                    imgui.tooltip(tip, ImGui::CalcTextSize(tip.c_str()).x + 2.0f * style.WindowPadding.x);
                }
                // lignes visibles : la hauteur qui reste ; numeros 1 a l'avant-derniere ligne (la derniere laissee, comme avant)
                const uint64_t lines_count = static_cast<uint64_t>(std::max(0.0f, (ImGui::GetContentRegionAvail().y + style.ItemSpacing.y) / (text_height + style.ItemSpacing.y)));
                const uint64_t last_id     = static_cast<uint64_t>(m_lines_ends.size()) - 1;
                const uint64_t count       = std::min(lines_count, last_id);
                bool           lines_ok    = count > 0;
                uint64_t       start_id    = 1;
                uint64_t       end_id      = 0;
                if (lines_ok) {
                    start_id = curr_line_id > count / 2 ? curr_line_id - count / 2 : 1;
                    start_id = std::max<uint64_t>(1, std::min(start_id, last_id - count + 1));
                    end_id   = start_id + count - 1;
                    // updates list of lines to show, if needed
                    if (m_selected_line_id != curr_line_id || m_last_lines_size != end_id - start_id + 1 || m_lines.empty() || m_lines.size() != count) {
                        try
                        {
                            *const_cast<std::vector<Line>*>(&m_lines) = update_lines(start_id, end_id);
                            *const_cast<uint64_t*>(&m_selected_line_id) = curr_line_id;
                            *const_cast<size_t*>(&m_last_lines_size) = m_lines.size();
                        }
                        catch (...)
                        {
                            BOOST_LOG_TRIVIAL(error) << "Error while loading from file " << PathSanitizer::sanitize(m_filename) << ". Cannot show G-code window.";
                            lines_ok = false;
                        }
                    }
                }
                if (lines_ok && m_lines.size() == count) {
                    // line number's column width
                    const float id_width = ImGui::CalcTextSize(std::to_string(end_id).c_str()).x;
                    const float right_x  = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                    for (uint64_t id = start_id; id <= end_id; ++id) {
                        const Line& line = m_lines[id - start_id];
                        // rect around the current selected line
                        if (id == curr_line_id) {
                            const float pos_y = ImGui::GetCursorScreenPos().y;
                            const float pos_x = ImGui::GetCursorScreenPos().x;
                            const float half_ItemSpacing_y = 0.5f * style.ItemSpacing.y;
                            const float half_ItemSpacing_x = 0.5f * style.ItemSpacing.x;
                            ImGui::GetWindowDrawList()->AddRect({ pos_x - half_ItemSpacing_x, pos_y - half_ItemSpacing_y },
                                { right_x + half_ItemSpacing_x, pos_y + text_height + half_ItemSpacing_y },
                                ImGui::GetColorU32(SELECTION_RECT_COLOR));
                        }
                        // render line number
                        const std::string id_str = std::to_string(id);
                        // spacer to right align text
                        ImGui::Dummy({ id_width - ImGui::CalcTextSize(id_str.c_str()).x, text_height });
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, LINE_NUMBER_COLOR);
                        imgui.text(id_str);
                        ImGui::PopStyleColor();
                        if (!line.command.empty() || !line.comment.empty())
                            ImGui::SameLine();
                        // render command
                        if (!line.command.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, COMMAND_COLOR);
                            imgui.text(line.command);
                            ImGui::PopStyleColor();
                        }
                        // render parameters
                        if (!line.parameters.empty()) {
                            ImGui::SameLine(0.0f, 0.0f);
                            ImGui::PushStyleColor(ImGuiCol_Text, PARAMETERS_COLOR);
                            imgui.text(line.parameters);
                            ImGui::PopStyleColor();
                        }
                        // render comment
                        if (!line.comment.empty()) {
                            if (!line.command.empty())
                                ImGui::SameLine(0.0f, 0.0f);
                            ImGui::PushStyleColor(ImGuiCol_Text, COMMENT_COLOR);
                            imgui.text(line.comment);
                            ImGui::PopStyleColor();
                        }
                    }
                }

                imgui.end();
                ImGui::PopStyleVar();
            }

            void GCodeWindow::stop_mapping_file()
            {
                //BBS: add log to trace the gcode file issue
                if (m_file.is_open()) {
                    m_file.close();
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished mapping file " << PathSanitizer::sanitize(m_filename);
                }
            }

            void SequentialView::Marker::init(std::string filename)
            {
                if (filename.empty()) {
                    m_model.init_from(stilized_arrow(16, 1.5f, 3.0f, 0.8f, 3.0f));
                }
                else {
                    m_model.init_from_file(filename);
                }
                m_model.set_color(-1, { 1.0f, 1.0f, 1.0f, 0.5f });
            }
            void SequentialView::Marker::set_world_position(const Vec3f& position)
            {
                m_world_position = position;
                m_world_transform = (Geometry::assemble_transform((position + m_z_offset * Vec3f::UnitZ()).cast<double>()) * Geometry::assemble_transform(m_model.get_bounding_box().size().z() * Vec3d::UnitZ(), { M_PI, 0.0, 0.0 })).cast<float>();
            }
            void SequentialView::Marker::update_curr_move(const GCodeProcessorResult::MoveVertex move) {
                m_curr_move = move;
            }
            //BBS: GUI refactor: add canvas size from parameters
            void SequentialView::Marker::render(int canvas_width, int canvas_height, const EViewType& view_type) const
            {
                if (!m_visible)
                    return;
                const auto& shader = wxGetApp().get_shader("gouraud_light");
                if (shader == nullptr)
                    return;
                glsafe(::glEnable(GL_BLEND));
                glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
                wxGetApp().bind_shader(shader);
                shader->set_uniform("emission_factor", 0.0f);
                // Orio : le marqueur de position n'est jamais coupe en X/Y (gouraud_light partage)
                shader->set_uniform("u_clip_xy_enabled", 0.0f);
                const Camera& camera = wxGetApp().plater()->get_camera();
                const Transform3d matrix = camera.get_view_matrix() * m_world_transform.cast<double>();
                shader->set_uniform("view_model_matrix", matrix);
                shader->set_uniform("projection_matrix", camera.get_projection_matrix());
                shader->set_uniform("normal_matrix", (Matrix3d)matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
                m_model.render_geometry();
                wxGetApp().unbind_shader();
                glsafe(::glDisable(GL_BLEND));
                static float last_window_width = 0.0f;
                size_t text_line = 0;
                static size_t last_text_line = 0;
                const ImU32 text_name_clr = m_is_dark ? IM_COL32(255, 255, 255, 0.88 * 255) : IM_COL32(38, 46, 48, 255);
                const ImU32 text_value_clr = m_is_dark ? IM_COL32(255, 255, 255, 0.4 * 255) : IM_COL32(144, 144, 144, 255);
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                //BBS: GUI refactor: add canvas size from parameters
                imgui.set_next_window_pos(0.5f * static_cast<float>(canvas_width), static_cast<float>(canvas_height), ImGuiCond_Always, 0.5f, 1.0f);
                imgui.push_toolbar_style(m_scale);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 4.0 * m_scale));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0 * m_scale, 6.0 * m_scale));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, text_name_clr);
                ImGui::PushStyleColor(ImGuiCol_Text, text_value_clr);
                imgui.begin(std::string("ExtruderPosition"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
                ImGui::AlignTextToFramePadding();
                //BBS: minus the plate offset when show tool position
                PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
                PartPlate* plate = partplate_list.get_curr_plate();
                const Vec3f position = m_world_position + m_world_offset;
                std::string x = ImGui::ColorMarkerStart + std::string("X: ") + ImGui::ColorMarkerEnd;
                std::string y = ImGui::ColorMarkerStart + std::string("Y: ") + ImGui::ColorMarkerEnd;
                std::string z = ImGui::ColorMarkerStart + std::string("Z: ") + ImGui::ColorMarkerEnd;
                std::string height = ImGui::ColorMarkerStart + _u8L("Height: ") + ImGui::ColorMarkerEnd;
                std::string width = ImGui::ColorMarkerStart + _u8L("Width: ") + ImGui::ColorMarkerEnd;
                std::string speed = ImGui::ColorMarkerStart + _u8L("Speed: ") + ImGui::ColorMarkerEnd;
                std::string flow = ImGui::ColorMarkerStart + _u8L("Flow: ") + ImGui::ColorMarkerEnd;
                std::string layer_time = ImGui::ColorMarkerStart + _u8L("Layer Time: ") + ImGui::ColorMarkerEnd;
                std::string fanspeed = ImGui::ColorMarkerStart + _u8L("Fan Speed: ") + ImGui::ColorMarkerEnd;
                std::string additional_fanspeed = ImGui::ColorMarkerStart + _u8L("AUX Fan Speed: ") + ImGui::ColorMarkerEnd;
                std::string temperature = ImGui::ColorMarkerStart + _u8L("Temperature: ") + ImGui::ColorMarkerEnd;
                const float item_size = imgui.calc_text_size("X: 000.000  ").x;
                const float item_spacing = imgui.get_item_spacing().x;
                const float window_padding = ImGui::GetStyle().WindowPadding.x;
                char buf[1024];
                // extra text depends on whether current move is extrude type
                bool show_extra_text = m_curr_move.type == EMoveType::Extrude;
                // FeatureType and ColorPrint shall only show x,y,z
                if (view_type == EViewType::FeatureType || view_type == EViewType::ColorPrint)
                    show_extra_text = false;
                // Feedrate and LayerTime shall always show extra text
                else if (view_type == EViewType::Feedrate || view_type == EViewType::LayerTime)
                    show_extra_text = true;
                if (show_extra_text)
                {
                    sprintf(buf, "%s%.3f", x.c_str(), position.x() - plate->get_origin().x());
                    ImGui::PushItemWidth(item_size);
                    imgui.text(buf);
                    ImGui::SameLine(window_padding + item_size + item_spacing);
                    sprintf(buf, "%s%.3f", y.c_str(), position.y() - plate->get_origin().y());
                    ImGui::PushItemWidth(item_size);
                    imgui.text(buf);
                    sprintf(buf, "%s%.3f", z.c_str(), position.z());
                    ImGui::PushItemWidth(item_size);
                    imgui.text(buf);
                    switch (view_type) {
                    case EViewType::Height: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.2f", height.c_str(), m_curr_move.height);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::Width: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.2f", width.c_str(), m_curr_move.width);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::Feedrate: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.0f", speed.c_str(), m_curr_move.feedrate);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::VolumetricRate: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.2f", flow.c_str(), m_curr_move.volumetric_rate());
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::FanSpeed: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.0f", fanspeed.c_str(), m_curr_move.fan_speed);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::AdditionalFanSpeed: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.0f", additional_fanspeed.c_str(), m_curr_move.additional_fan_speed);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::Temperature: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.0f", temperature.c_str(), m_curr_move.temperature);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::LayerTime: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.1f", layer_time.c_str(), m_curr_move.layer_duration);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    default:
                        break;
                    }
                    text_line = 2;
                }
                else {
                    sprintf(buf, "%s%.3f", x.c_str(), position.x() - plate->get_origin().x());
                    imgui.text(buf);
                    ImGui::SameLine();
                    sprintf(buf, "%s%.3f", y.c_str(), position.y() - plate->get_origin().y());
                    imgui.text(buf);
                    ImGui::SameLine();
                    sprintf(buf, "%s%.3f", z.c_str(), position.z());
                    imgui.text(buf);
                    text_line = 1;
                }
                // force extra frame to automatically update window size
                float window_width = ImGui::GetWindowWidth();
                if (window_width != last_window_width || text_line != last_text_line) {
                    last_window_width = window_width;
                    last_text_line = text_line;
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                    imgui.set_requires_extra_frame();
#else
                    wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                    wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                }
                imgui.end();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                imgui.pop_toolbar_style();
            }

            //BBS: GUI refactor: move to the right
            void SequentialView::render(float legend_height, int canvas_width, int canvas_height, int right_margin, const EViewType& view_type) const
            {
                marker.render(canvas_width, canvas_height, view_type);
                //float bottom = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size().get_height();
                // BBS
#if 0
                if (wxGetApp().is_editor())
                    bottom -= wxGetApp().plater()->get_view_toolbar().get_height();
#endif
                //gcode_window.render(legend_height, bottom, static_cast<uint64_t>(gcode_ids[current.last]));
                gcode_window.render(legend_height, (float)canvas_height, (float)canvas_width - (float)right_margin, static_cast<uint64_t>(gcode_ids[current.last]));
            }

            // Orio : encart de la vue "Fusion de couche" : surface collee a la couche du dessous de l'objet
            // selectionne, a la couche du curseur haut, en moyenne entre les deux curseurs et sur toute la piece
            // (moyennes ponderees par la hauteur de couche ; la 1re couche, posee sur le plateau, n'y entre pas).
            void BaseRenderer::render_layer_fusion_stats(float window_padding)
            {
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                auto line = [&imgui, window_padding](const std::string& text) {
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(text);
                };
                ImGui::Spacing();
                const LayerFusion::Result* res = m_layer_fusion.result();
                if (res == nullptr || m_gcode_result == nullptr || res->gcode_id != m_gcode_result->id) {
                    line(m_layer_fusion.busy() ? _u8L("Computing the contact areas...") : _u8L("Slice the plate again to compute the contact areas."));
                    return;
                }
                // Objet clique dans l'Apercu, sinon celui selectionne dans la Preparation, sinon le seul du plateau
                const Model& model = wxGetApp().plater()->model();
                const ModelObject* object = nullptr;
                auto on_plate = [res](const ModelObject* mo) { return mo != nullptr && res->objects.count(mo->id().id) > 0; };
                if (m_picked_label >= 0)
                    for (const ModelObject* mo : model.objects)
                        for (const ModelInstance* mi : mo->instances)
                            if (int(mi->get_labeled_id()) == m_picked_label && on_plate(mo))
                                object = mo;
                const int idx = wxGetApp().plater()->get_selection().get_object_idx();
                if (object == nullptr && idx >= 0 && idx < int(model.objects.size()) && on_plate(model.objects[idx]))
                    object = model.objects[idx];
                if (object == nullptr && res->objects.size() == 1)
                    for (const ModelObject* mo : model.objects)
                        if (mo->id().id == res->objects.begin()->first)
                            object = mo;
                if (object == nullptr) {
                    line(_u8L("Click an object to see its contact areas."));
                    return;
                }
                const auto it = res->objects.find(object->id().id);
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::SameLine();
                imgui.bold_text(object->name);
                if (it->second.tilted) {
                    line(_u8L("Tilted layers: contact areas not computed."));
                    return;
                }
                line(_u8L("Contact with the layer below / layer section"));
                if (ImGui::IsItemHovered())
                    imgui.tooltip(_u8L("Real contact: a line is rounded on its sides, only its flat (width minus height) touches the line "
                                       "below. With brick layers, each line is counted on the level it really rests on.").c_str(),
                                  ImGui::GetFontSize() * 20.f);

                const double lo = m_layers_slider ? m_layers_slider->GetLowerValueD() : 0.;
                const double hi = m_layers_slider ? m_layers_slider->GetHigherValueD() : DBL_MAX;
                // couche courante : la plus haute de l'objet sous le curseur haut
                const LayerFusion::LayerStat* current = nullptr;
                double range_c = 0., range_s = 0., range_h = 0., part_c = 0., part_s = 0., part_h = 0.;
                for (const LayerFusion::LayerStat& l : it->second.layers) {
                    const bool in_range = l.print_z >= lo - 1e-4 && l.print_z <= hi + 1e-4;
                    if (l.print_z <= hi + 1e-4)
                        current = &l;
                    if (l.contact < 0.)
                        continue;
                    part_c += l.contact * l.height;
                    part_s += l.section * l.height;
                    part_h += l.height;
                    if (in_range) {
                        range_c += l.contact * l.height;
                        range_s += l.section * l.height;
                        range_h += l.height;
                    }
                }
                auto mm = [](double z) {
                    char buf[32];
                    ::sprintf(buf, "%.2f", z);
                    return std::string(buf);
                };
                // Orio : valeurs en colonnes alignees (contact / section (pourcentage)), puis "(?)" et son infobulle.
                // section vide : texte seul (premiere couche) dans la colonne du contact.
                struct Row { std::string label, contact, section, percent, tip; };
                auto value_row = [](const std::string& label, double contact, double section, const std::string& tip) {
                    char c[64], s[64], p[32];
                    ::sprintf(c, "%.1f mm\xC2\xB2", contact);
                    ::sprintf(s, "%.1f mm\xC2\xB2", section);
                    ::sprintf(p, "(%.0f %%)", section > 0. ? 100. * contact / section : 0.);
                    return Row{ label, c, s, p, tip };
                };
                std::vector<Row> rows;
                if (current != nullptr) {
                    const std::string label = (boost::format(_u8L("Layer at %1% mm")) % mm(current->print_z)).str();
                    if (current->contact < 0.)
                        rows.push_back({ label, _u8L("on the bed"), "", "",
                                         _u8L("First layer: it rests on the bed, there is no layer below to stick to.") });
                    else
                        rows.push_back(value_row(label, current->contact, current->section,
                                                 _u8L("Layer under the top handle of the slider.\n"
                                                      "First value: area really stuck to the layer below (flat of the lines resting on the flat of the lines below).\n"
                                                      "Second value: area of the part section at this height.\n"
                                                      "Percentage: first value divided by the second, the higher it is, the better the layers hold together.")));
                }
                if (range_h > 0.)
                    rows.push_back(value_row((boost::format(_u8L("Average from %1% to %2% mm")) % mm(lo) % mm(hi)).str(),
                                             range_c / range_h, range_s / range_h,
                                             _u8L("Same values, averaged over the layers between the two handles of the slider "
                                                  "(weighted by layer height, first layer excluded).\n"
                                                  "Move the handles to measure another zone.")));
                if (part_h > 0.)
                    rows.push_back(value_row(_u8L("Part average"), part_c / part_h, part_s / part_h,
                                             _u8L("Same values, averaged over all the layers of the part "
                                                  "(weighted by layer height, first layer excluded).")));
                auto width = [](const std::string& t) { return ImGui::CalcTextSize(t.c_str()).x; };
                float w_label = 0.f, w_contact = 0.f, w_section = 0.f, w_percent = 0.f;
                for (const Row& r : rows) {
                    w_label = std::max(w_label, width(r.label));
                    if (r.section.empty())
                        continue;
                    w_contact = std::max(w_contact, width(r.contact));
                    w_section = std::max(w_section, width(r.section));
                    w_percent = std::max(w_percent, width(r.percent));
                }
                const float sp      = ImGui::GetStyle().ItemSpacing.x;
                const float x_value = window_padding + w_label + 3.f * sp;
                const float x_slash = x_value + w_contact + sp;
                const float x_sect  = x_slash + width("/") + sp;
                const float x_perc  = x_sect + w_section + sp;
                float       x_help  = x_perc + w_percent + 2.f * sp;
                for (const Row& r : rows)
                    if (r.section.empty())
                        x_help = std::max(x_help, x_value + width(r.contact) + 2.f * sp);
                // texte cale a droite, finissant en x_end
                auto right = [&imgui, &width](const std::string& t, float x_end) {
                    ImGui::SameLine(x_end - width(t));
                    imgui.text(t);
                };
                for (const Row& r : rows) {
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(r.label);
                    if (r.section.empty()) {
                        ImGui::SameLine(x_value);
                        imgui.text(r.contact);
                    }
                    else {
                        right(r.contact, x_value + w_contact);
                        ImGui::SameLine(x_slash);
                        imgui.text("/");
                        right(r.section, x_sect + w_section);
                        right(r.percent, x_perc + w_percent);
                    }
                    ImGui::SameLine(x_help);
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered())
                        imgui.tooltip(r.tip.c_str(), ImGui::GetFontSize() * 20.f);
                }
            }

            ColorRGBA Range::get_color_at(float value, EType type) const
            {
                float       global_t = 0.0f;
                const float step = step_size(type);
                float       _min = min;
                if (log_scale) {
                    value = std::log(value);
                    _min = std::log(min);
                }
                if (value > max) {
                    return range_colors[range_colors.size() - 1];
                }
                if (value < _min) {
                    if (value < _min - 0.01f) {
                        return ColorRGBA::GRAY(); // well below the range
                    }
                    else {
                        return range_colors[0];
                    }
                }
                if (step > 0.0f) {
                    switch (type) {
                    default:
                    case EType::Linear: {
                        global_t = (value > min) ? (value - min) / step : 0.0f;
                        break;
                    }
                    case EType::Logarithmic: {
                        global_t = (value > _min  && step != 0.0f) ? std::max(0.0f, value - _min) / step : 0.0f;
                        break;
                    }
                    }
                }
                const size_t color_max_idx = range_colors.size() - 1;

                // Compute the two colors just below (low) and above (high) the input value
                const size_t color_low_idx = std::clamp<size_t>(static_cast<size_t>(global_t), 0, color_max_idx);
                const size_t color_high_idx = std::clamp<size_t>(color_low_idx + 1, 0, color_max_idx);
                // Compute how far the value is between the low and high colors so that they can be interpolated
                const float local_t = std::clamp(global_t - static_cast<float>(color_low_idx), 0.0f, 1.0f);
                // Interpolate between the low and high colors to find exactly which color the input value should get
                return lerp(range_colors[color_low_idx], range_colors[color_high_idx], local_t);
            }

            float Range::step_size(EType type) const {
                switch (type) {
                default:
                case EType::Linear: {
                    return (max > min) ? (max - min) / get_color_size() : 0.0f;
                }
                case EType::Logarithmic: {
                    float min_range = min;
                    if (min_range == 0) min_range = 0.001f;
                    return std::log(max / min_range) / get_color_size();
                }
                }
            }

            float Range::get_value_at_step(int step) const {
                if (!log_scale)
                    return min + static_cast<float>(step) * step_size();
                else
                    // calculate log-average
                {
                    float min_range = min;
                    if (min_range == 0)
                        min_range = 0.0001f;
                    float step_size = std::log(max / min_range) / get_color_size();
                    return std::exp(std::log(min) + static_cast<float>(step) * step_size);
                }
            }

            float Range::get_color_size() const {
                return (static_cast<float>(range_colors.size()) - 1.0f);
            }
} // namespace gcode
    } // namespace GUI
} // namespace Slic3r
