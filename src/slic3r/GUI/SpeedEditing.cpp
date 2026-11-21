// Orio: "Variable speed" tool, the interactive editor of VariableSpeedProfile (libslic3r/Slicing.hpp).
// GLCanvas3D::SpeedEditing is declared in GLCanvas3D.hpp. Moved out of GLCanvas3D.cpp as is, like
// InfillWallsEditing.cpp, so that the tool can grow without touching GLCanvas3D.cpp, shared by many sessions.
// A toggle of the window paints along the X / Y axes of the object instead of Z, exactly like "Variable infill and
// walls": the bar is then laid along the bottom of the scene (PaintAxisBar). Where several axes of a type are
// painted, the slowest speed applies.

#include "libslic3r/libslic3r.h"
#include "GLCanvas3D.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <string>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PaintAxis.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "GLToolbar.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "ImGuiWrapper.hpp"
#include "format.hpp"

#include <GL/glew.h>
#include <wx/glcanvas.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

namespace Slic3r {
namespace GUI {

// ------------------------------------------------------------------------------------------------
//BBS: variable print speed along Z. Mirror image of GLCanvas3D::LayersEditing.
// ------------------------------------------------------------------------------------------------

float GLCanvas3D::SpeedEditing::s_overlay_window_width;

// Reads a per nozzle speed option out of the print config, skipping the "nil" entries.
static float variable_speed_config_value(const DynamicPrintConfig* config, const char* key, float fallback)
{
    if (config == nullptr)
        return fallback;
    const ConfigOption* opt = config->option(key);
    if (opt == nullptr)
        return fallback;
    if (const ConfigOptionFloatsNullable* fn = dynamic_cast<const ConfigOptionFloatsNullable*>(opt)) {
        for (double v : fn->values)
            if (std::isfinite(v) && v > 0.)
                return (float)v;
        return fallback;
    }
    if (const ConfigOptionFloats* fs = dynamic_cast<const ConfigOptionFloats*>(opt)) {
        for (double v : fs->values)
            if (std::isfinite(v) && v > 0.)
                return (float)v;
        return fallback;
    }
    if (const ConfigOptionFloat* f = dynamic_cast<const ConfigOptionFloat*>(opt))
        return (f->value > 0.) ? (float)f->value : fallback;
    return fallback;
}

GLCanvas3D::SpeedEditing::~SpeedEditing()
{
    if (m_z_texture_id != 0) {
        glsafe(::glDeleteTextures(1, &m_z_texture_id));
        m_z_texture_id = 0;
    }

    m_baseline.reset();
    m_profile_curve.reset();
}

void GLCanvas3D::SpeedEditing::init()
{
    const auto& p_ogl_manager = wxGetApp().get_opengl_manager();
    const auto& gl_info = p_ogl_manager->get_gl_info();
    const auto formated_gl_version = gl_info.get_formated_gl_version();
    glsafe(::glGenTextures(1, (GLuint*)&m_z_texture_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, formated_gl_version >= 30 ? GL_CLAMP_TO_EDGE : GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, formated_gl_version >= 30 ? GL_CLAMP_TO_EDGE : GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    // Orio: restore the tool's numeric fields saved in a previous session.
    load_persisted_fields();
}

// Orio: the print setting holding the speed of the extrusions of a type (VariableSpeedTarget bit), the one its painted
// speed replaces in the G-code; the inner walls when no type is edited.
static const char* variable_speed_preset_key(uint32_t target)
{
    switch (target) {
    case vstOuterWall:    return "outer_wall_speed";
    case vstSparseInfill: return "sparse_infill_speed";
    case vstSolidInfill:  return "internal_solid_infill_speed";
    case vstTopSurface:   return "top_surface_speed";
    case vstBridge:       return "bridge_speed";
    case vstSupport:      return "support_speed";
    case vstGapFill:      return "gap_infill_speed";
    case vstIroning:      return "ironing_speed";
    default:              return "inner_wall_speed";
    }
}

// Orio: remembers the edited axis (Z, X or Y) alone, as soon as it is chosen. The step is saved only once the user
// changed it (save_persisted_fields()): until then it follows the scale. The base / min of a type are not saved (10/11).
static void variable_speed_save_axis(const PaintAxisBar& axis_bar)
{
    if (AppConfig* app_config = wxGetApp().app_config)
        app_config->set("speed_editing", "axis", std::string(! axis_bar.is_xy() ? "z" : axis_bar.axis() == paY ? "y" : "x"));
}

// Orio: the numeric fields of the Variable-speed tool are remembered across sessions in the
// application config (Orio.conf), section "speed_editing". They are NOT per-object: they are the
// editor's working scale/step, restored on launch so the user does not re-enter them every time.
void GLCanvas3D::SpeedEditing::load_persisted_fields()
{
    const auto* app_config = wxGetApp().app_config;
    if (app_config == nullptr || ! app_config->has_section("speed_editing"))
        return;

    std::string v;
    // Orio: the edited axis, "z", "x" or "y".
    if (app_config->get("speed_editing", "axis", v)) {
        m_axis_bar.set_xy(v == "x" || v == "y");
        m_axis_bar.set_axis(v == "y" ? paY : paX);
    }
    if (app_config->get("speed_editing", "smooth_radius", v) && ! v.empty()) {
        try { m_smooth_params.radius = (unsigned int)std::clamp(std::stoi(v), 1, 10); } catch (...) {}
    }
    if (app_config->get("speed_editing", "smooth_keep_min", v) && ! v.empty())
        m_smooth_params.keep_min = (v == "true" || v == "1");
    // Orio: not a speed, it does not make them user-set (see below).
    if (app_config->get("speed_editing", "hide_other_objects", v) && ! v.empty())
        m_hide_other_objects = (v == "true" || v == "1");

    auto get_f = [app_config](const std::string& key, float v_min, float v_max, float& out) {
        std::string value;
        if (! app_config->get("speed_editing", key, value) || value.empty())
            return false;
        try { out = std::clamp(std::stof(value), v_min, v_max); return true; } catch (...) { return false; }
    };
    // Orio: the base and min typed for a type are kept only until Orio is closed (asked by the user, 10/11): each
    // type starts again from the print settings. The keys "base_speed", "min_speed", "base_speed_<bit>" and
    // "min_speed_<bit>" saved by older builds are ignored. max_speed is always derived from the object's material.
    m_step_user_set = get_f("step", 0.1f, 100.0f, strength);
}

void GLCanvas3D::SpeedEditing::save_persisted_fields() const
{
    auto* app_config = wxGetApp().app_config;
    if (app_config == nullptr)
        return;
    // Orio: the base / min typed for a type live until Orio is closed, max_speed is derived from the object's
    // material: neither is saved.
    if (m_step_user_set)
        app_config->set("speed_editing", "step",        std::to_string(strength));
    app_config->set("speed_editing", "smooth_radius",   std::to_string(m_smooth_params.radius));
    app_config->set("speed_editing", "smooth_keep_min", m_smooth_params.keep_min);
    variable_speed_save_axis(m_axis_bar);
}

void GLCanvas3D::SpeedEditing::set_config(const DynamicPrintConfig* config)
{
    m_config = config;
    m_speed_texture.valid = false;
    // Base / min / max speeds are derived from the print config, so recompute them.
    m_speed_range_valid = false;
    m_profile_dirty = true;
    // IMPORTANT: do NOT clear the painted curves here. They are per-object data, unrelated to the
    // print config, and set_config() runs on every config change (including at slice time). Clearing
    // them here was what made the tool forget the edits after a slice, because the same-object path in
    // select_object() does not reload. The curves are dropped only when a different object is selected
    // (select_object) or the object list is reset.
}

// Orio: the tool needs the print speeds, the line width and the filament's max volumetric speed.
// m_config is the plater's REDUCED config (only the keys the 3D scene needs, see the key list in the Plater
// constructor): it holds none of them, so option() returned nullptr and the scale silently kept its built-in
// defaults (max = 300 whatever the preset). They are read from the presets being edited instead, which also
// carries unsaved changes.
static const DynamicPrintConfig* variable_speed_print_config()
{
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    return bundle != nullptr ? &bundle->prints.get_edited_preset().config : nullptr;
}

// Value #idx of a float vector option, plain or nullable (0 when absent or nil).
static double variable_speed_float_at(const ConfigOption* opt, int idx)
{
    if (const auto* fn = dynamic_cast<const ConfigOptionFloatsNullable*>(opt)) {
        if (idx >= 0 && idx < (int)fn->values.size() && std::isfinite(fn->values[idx]))
            return fn->values[idx];
    } else if (const auto* fs = dynamic_cast<const ConfigOptionFloats*>(opt)) {
        if (idx >= 0 && idx < (int)fs->values.size())
            return fs->values[idx];
    }
    return 0.0;
}

// Max volumetric speed (mm3/s) of the 1-based project filament `filament`, for the nozzle variant of the
// extruder it prints with: a filament preset holds one value per extruder variant (Bambu PLA Basic: 21 for a
// standard nozzle, 29 for a high flow one). Lowered by its fitted value at this line height and width when its "Adaptive
// volumetric speed" is on, as GCode::_extrude. The filament being edited with its unsaved changes. 0 when unknown or not set.
// ponytail: same code as tab_filament_max_volumetric (Tab.cpp, static), share them when a third user comes.
static double variable_speed_filament_max_volumetric(int filament, double height, double width)
{
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || filament < 1 || filament > (int)bundle->filament_presets.size())
        return 0.0;
    const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[filament - 1], true);
    if (preset != nullptr && preset->name == bundle->filaments.get_edited_preset().name)
        preset = &bundle->filaments.get_edited_preset();
    if (preset == nullptr)
        return 0.0;
    const ConfigOption* opt = preset->config.option("filament_max_volumetric_speed");
    if (opt == nullptr)
        return 0.0;

    // Extruder the filament prints with (filament_map is 1-based; single extruder printers: extruder 0), then
    // the entry of that extruder's variant (extruder type + nozzle flow type, as set in the sidebar).
    int extruder = 0;
    if (const auto* fmap = bundle->project_config.option<ConfigOptionInts>("filament_map"))
        if (filament - 1 < (int)fmap->values.size() && fmap->values[filament - 1] >= 1)
            extruder = fmap->values[filament - 1] - 1;
    int index = 0;
    const auto* ext_types = bundle->printers.get_edited_preset().config.option<ConfigOptionEnumsGeneric>("extruder_type");
    const auto* vol_types = bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (ext_types != nullptr && vol_types != nullptr && extruder < (int)ext_types->values.size() &&
        extruder < (int)vol_types->values.size()) {
        const int i = preset->config.get_index_for_extruder(extruder + 1, std::string(), ExtruderType(ext_types->values[extruder]),
                                                            NozzleVolumeType(vol_types->values[extruder]), "filament_extruder_variant");
        if (i >= 0)
            index = i;
    }
    double v = variable_speed_float_at(opt, index);
    if (v <= 0.0)
        v = variable_speed_float_at(opt, 0);
    const auto* adaptive = preset->config.option<ConfigOptionBoolsNullable>("filament_adaptive_volumetric_speed");
    const auto* coefs    = preset->config.option<ConfigOptionStrings>("volumetric_speed_coefficients");
    if (v > 0.0 && adaptive != nullptr && ! adaptive->empty() && coefs != nullptr && ! coefs->empty() &&
        adaptive->get_at(size_t(index) < adaptive->size() ? index : 0) == 1) {
        std::vector<double> cs;
        std::stringstream   ss(coefs->get_at(size_t(index) < coefs->size() ? index : 0));
        for (double c; ss >> c;)
            cs.push_back(c);
        if (cs.size() == 6 && std::any_of(cs.begin(), cs.end(), [](double c) { return c != 0.; })) {
            const double h = height, w = width;
            v = std::min(v, std::max(1., cs[0] * h * h + cs[1] * w * w + cs[2] * h * w + cs[3] * h + cs[4] * w + cs[5]));
        }
    }
    return v;
}

// Orio: highest speed the object's material can actually reach for the extrusions of a type, from its maximum
// volumetric flow: speed = max_volumetric / mm3_per_mm, with the line width of the type and the cross-section of
// Flow::mm3_per_mm(). This is the same ceiling the slicer enforces in GCode::_extrude (10/11: it was computed with a
// rectangle of the default line width for every type, 179 mm/s where an outer wall prints at 199; then without the flow
// ratios, the adaptive volumetric speed and the bridging flow the speed settings count since). Returns 0 when no
// usable limit is configured, so the caller can fall back to a print-speed-based scale. For a
// multi-material object the most limiting filament (smallest volumetric flow) is used, so the whole
// scale stays reachable by every part.
float GLCanvas3D::SpeedEditing::object_volumetric_speed_max(uint32_t target) const
{
    if (m_model_object == nullptr)
        return 0.0f;

    // Flow of a line of the type, from its line width and the layer height of THIS object (its own settings, e.g. a
    // per-object layer height, override the print preset), with its flow ratios: the same mm3 per mm the slicer divides
    // the filament flow by (GCode::_extrude) and the speed settings show at the right of a capped speed (Tab.cpp,
    // speed_cap_note).
    const DynamicPrintConfig* print_cfg = variable_speed_print_config();
    auto option = [this, print_cfg](const char* key) -> const ConfigOption* {
        if (const ConfigOption* o = m_model_object->config.option(key))
            return o;
        return print_cfg != nullptr ? print_cfg->option(key) : nullptr;
    };
    auto setting = [&option](const char* key) -> double {
        const ConfigOption* o = option(key);
        if (const auto* f = dynamic_cast<const ConfigOptionFloat*>(o))
            return f->value;
        return variable_speed_float_at(o, 0); // a per nozzle variant option: the first variant
    };
    auto ratio = [&setting](const char* key) { const double r = setting(key); return r > 0.0 ? r : 1.0; };
    double lh = setting("layer_height");
    if (lh <= 0.0)
        lh = 0.2;
    double nozzle = 0.0;
    if (const PresetBundle* bundle = wxGetApp().preset_bundle)
        nozzle = variable_speed_float_at(bundle->printers.get_edited_preset().config.option("nozzle_diameter"), 0);
    if (nozzle <= 0.0)
        nozzle = 0.4;
    // ponytail: gap fill and ironing take the default line width, not their own flow; the slicer still caps them.
    const char* width_key = target == vstOuterWall    ? "outer_wall_line_width" :
                            target == vstInnerWall    ? "inner_wall_line_width" :
                            target == vstSparseInfill ? "sparse_infill_line_width" :
                            target == vstSolidInfill  ? "internal_solid_infill_line_width" :
                            target == vstTopSurface   ? "top_surface_line_width" :
                            target == vstSupport      ? "support_line_width" :
                            target == vstBridge       ? "inner_wall_line_width" : "line_width";
    double lw = setting(width_key);
    if (lw <= 0.0)
        lw = setting("line_width");
    if (lw <= 0.0)
        lw = nozzle;
    double mm3 = lh * (lw - lh * (1. - 0.25 * PI)); // Flow::mm3_per_mm
    if (target == vstBridge) {
        // The bridging flow (LayerRegion::bridging_flow): a round thread of bridge_flow x the nozzle section for thick
        // bridges, else the inner wall line with bridge_flow. No line type ratio (a bridge infill has none in _extrude).
        const auto* thick = dynamic_cast<const ConfigOptionBool*>(option("thick_bridges"));
        const double bridge_flow = ratio("bridge_flow");
        mm3 = thick != nullptr && thick->value ? 0.25 * PI * bridge_flow * nozzle * nozzle : mm3 * bridge_flow;
    } else {
        const char* ratio_key = target == vstOuterWall    ? "outer_wall_flow_ratio" :
                                target == vstInnerWall    ? "inner_wall_flow_ratio" :
                                target == vstSparseInfill ? "sparse_infill_flow_ratio" :
                                target == vstSolidInfill  ? "internal_solid_infill_flow_ratio" :
                                target == vstTopSurface   ? "top_solid_infill_flow_ratio" :
                                target == vstSupport      ? "support_flow_ratio" :
                                target == vstGapFill      ? "gap_fill_flow_ratio" : nullptr;
        if (ratio_key != nullptr)
            mm3 *= ratio(ratio_key);
    }
    mm3 *= ratio("print_flow_ratio");
    if (mm3 <= 0.0)
        return 0.0f;

    // Smallest positive volumetric flow among the filaments this object's parts print with (1-based ids;
    // filament 1 when the object has no printable part).
    double limit = 0.0;
    bool   has_part = false;
    for (const ModelVolume* v : m_model_object->volumes) {
        if (v == nullptr || ! v->is_model_part())
            continue;
        has_part = true;
        const double vv = variable_speed_filament_max_volumetric(std::max(1, v->extruder_id()), lh, lw);
        if (vv > 0.0)
            limit = (limit == 0.0) ? vv : std::min(limit, vv);
    }
    if (! has_part)
        limit = variable_speed_filament_max_volumetric(1, lh, lw);
    if (limit <= 0.0)
        return 0.0; // material has no volumetric limit set -> caller keeps its print-speed scale

    return (float)(limit / mm3);
}

void GLCanvas3D::SpeedEditing::update_speed_range()
{
    if (m_speed_range_valid)
        return;

    // Orio: speeds come from the edited print preset (m_config, the plater's reduced config, has none), the object's
    // own settings first.
    const DynamicPrintConfig* print_cfg = variable_speed_print_config();
    auto preset_speed = [this, print_cfg](const char* key, float fallback) {
        const float own = (m_model_object != nullptr) ? variable_speed_config_value(&m_model_object->config.get(), key, 0.0f) : 0.0f;
        return own > 0.0f ? own : variable_speed_config_value(print_cfg, key, fallback);
    };
    const float inner_wall = preset_speed("inner_wall_speed", 100.0f);
    m_preset_speed = std::clamp(preset_speed(variable_speed_preset_key(m_active_target), inner_wall), (float)MIN_VARIABLE_SPEED, (float)MAX_VARIABLE_SPEED);

    // The scale MAX always tracks the volumetric flow limit of the object's material for the extrusions of the edited type,
    // so the top of the bar is the real achievable speed. When the material sets no limit, fall back to 1.5x the fastest
    // print speed.
    float target_max = object_volumetric_speed_max(m_active_target);
    if (target_max <= 0.0f) {
        float fastest = std::max(inner_wall, m_preset_speed);
        for (const char* key : { "outer_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed",
                                 "top_surface_speed", "gap_infill_speed", "support_speed" })
            fastest = std::max(fastest, preset_speed(key, 0.0f));
        target_max = std::ceil(1.5f * fastest);
    }

    m_max_speed = std::clamp(target_max, (float)MIN_VARIABLE_SPEED + 1.0f, (float)MAX_VARIABLE_SPEED);

    // Orio: the base of the edited type is the speed it is really printed at where it is not painted: its speed in
    // the print settings, capped like the slicer does by the filament's flow (10/11: the column's value was shown, above
    // what is printed). The min is 10% of it. Unless the user typed them for this type (01/11).
    const auto base_it = m_user_base_speeds.find(m_active_target);
    const auto min_it  = m_user_min_speeds.find(m_active_target);
    m_base_speed = base_it != m_user_base_speeds.end() ? base_it->second : std::min(m_preset_speed, m_max_speed);
    m_min_speed  = min_it != m_user_min_speeds.end() ? min_it->second : std::max((float)MIN_VARIABLE_SPEED, std::floor(0.1f * m_base_speed));
    if (m_min_speed > m_max_speed - 1.0f)
        m_min_speed = std::max((float)MIN_VARIABLE_SPEED, m_max_speed - 1.0f);
    m_base_speed = std::clamp(m_base_speed, m_min_speed, m_max_speed);
    if (! m_step_user_set)
        // One click moves the speed by ~2% of the scale, the same order of magnitude as the layer
        // height tool moves the layer height.
        strength = std::max(0.5f, 0.02f * (m_max_speed - m_min_speed));
    m_speed_range_valid = true;
    // Orio: the bar and the flat curve of a type not painted follow the new scale (the texture may have been drawn
    // with the scale of the previous type).
    m_speed_texture.valid = false;
    m_profile_dirty = true;
}

// Orio: curve to show (paint_axis_key() of a type and an axis) after an undo / redo changed the painted curves
// of the edited object. The ticked type and the edited axis are part of the UI, not of the model: they stay when their
// own curve changed (even removed: flat preview, ready to be painted again) or when nothing else did. Otherwise the
// type and the axis of the curve the jump changed are shown (each edit changes a single curve), so that Ctrl+Z / Ctrl+Y
// always show on the bar.
static uint32_t variable_speed_key_to_show(const std::map<uint32_t, std::vector<double>>& before,
                                           const std::map<uint32_t, std::vector<double>>& after, uint32_t edited)
{
    static const std::vector<double> none;
    auto curve = [](const std::map<uint32_t, std::vector<double>>& curves, uint32_t key) -> const std::vector<double>& {
        const auto it = curves.find(key);
        return it == curves.end() ? none : it->second;
    };
    if (paint_axis_key_target(edited) != 0 && curve(before, edited) != curve(after, edited))
        return edited;
    for (const std::map<uint32_t, std::vector<double>>* curves : { &before, &after })
        for (const auto& kv : *curves)
            if (kv.first != edited && paint_axis_key_target(kv.first) != 0 && paint_axis_is_valid(paint_axis_key_axis(kv.first)) &&
                curve(before, kv.first) != curve(after, kv.first))
                return kv.first;
    return edited;
}

// Orio: fits a curve stored in m_curves to the current size of the object: a Z curve to its height, an X / Y
// curve over its extent along that axis (the object was scaled, or a part added, since it was painted) instead of
// throwing the user's work away.
static void variable_speed_fit_curve(std::vector<double>& curve, PaintAxis axis, double height, const PaintAxisBar& axis_bar)
{
    if (curve.size() < 4 || (curve.size() & 1) != 0)
        return;
    if (axis == paZ) {
        const double old_h = curve[curve.size() - 2];
        if (old_h > EPSILON && std::abs(old_h - height) > 1e-3) {
            const double k = height / old_h;
            for (size_t i = 0; i < curve.size(); i += 2)
                curve[i] *= k;
            curve[curve.size() - 2] = height;
        }
    } else if (axis == paX || axis == paY) {
        const std::pair<double, double> extent = axis_bar.extent(axis);
        const double                    from   = curve.front();
        const double                    to     = curve[curve.size() - 2];
        if (extent.second - extent.first > EPSILON && to - from > EPSILON && (from != extent.first || to != extent.second)) {
            const double k = (extent.second - extent.first) / (to - from);
            for (size_t i = 0; i < curve.size(); i += 2)
                curve[i] = extent.first + (curve[i] - from) * k;
            curve.front()           = extent.first;
            curve[curve.size() - 2] = extent.second;
        }
    }
}

// Orio: the bounds of a curve follow it when it is fitted to a resized object (variable_speed_fit_curve()).
static void variable_speed_fit_bounds(std::vector<double>& bounds, const std::vector<double>& curve_before, const std::vector<double>& curve_after)
{
    if (bounds.empty() || curve_before.size() < 4 || curve_after.size() < 4)
        return;
    const double from = curve_before.front();
    const double to   = curve_before[curve_before.size() - 2];
    if (to - from <= EPSILON)
        return;
    const double k = (curve_after[curve_after.size() - 2] - curve_after.front()) / (to - from);
    for (double& bound : bounds)
        bound = curve_after.front() + (bound - from) * k;
}

void GLCanvas3D::SpeedEditing::select_object(const Model& model, int object_id)
{
    // Orio: within the model, as in InfillWallsEditing::select_object(). While a project is being closed or
    // opened, the selection may still name an object of the previous model (crash at File > Open, build of 05/10 19h40).
    const ModelObject* model_object_new = (object_id >= 0 && object_id < (int)model.objects.size()) ? model.objects[object_id] : nullptr;

    // Detaching (switching to the Preview tab, or the tool being turned off): keep the painted curves
    // cached in memory so a slice or a Preview round-trip never loses them. We only stop pointing at
    // an object; m_curves and m_curves_oid are preserved and restored when we reattach to it.
    if (model_object_new == nullptr) {
        this->last_object_id = -1;
        m_model_object = nullptr;
        m_object_max_z = 0.0f;
        m_speed_texture.valid = false;
        m_profile_dirty = true;
        return;
    }

    const float new_max_z = static_cast<float>(model_object_new->bounding_box().max.z());
    const bool same_object = m_curves_oid.valid() && m_curves_oid == model_object_new->id();
    this->last_object_id = object_id;
    m_model_object = model_object_new;
    m_speed_texture.valid = false;
    m_profile_dirty = true;
    update_speed_range();
    // Orio: extents of the object along its own X and Y axes, spanned by the curves painted along them.
    const std::pair<double, double> old_extent_x = m_axis_bar.extent(paX);
    const std::pair<double, double> old_extent_y = m_axis_bar.extent(paY);
    m_axis_bar.set_object(model_object_new);
    const bool extents_changed = m_axis_bar.extent(paX) != old_extent_x || m_axis_bar.extent(paY) != old_extent_y;

    if (same_object) {
        // If the model's speed_profile no longer matches what we last committed, an undo/redo or a
        // project reload changed it under us: reload the cached curves from the model so Ctrl+Z/Y take
        // effect. A plain reslice leaves speed_profile untouched, so the painted curves still survive.
        if (model_object_new->speed_profile.curves != m_committed_curves || model_object_new->speed_profile.bounds != m_committed_bounds) {
            // Orio: keep the ticked type and the edited axis, or show the curve the jump changed (see above).
            const uint32_t key = variable_speed_key_to_show(m_committed_curves, model_object_new->speed_profile.curves, edited_key());
            load_curves_from_model(model_object_new);
            const uint32_t  target = paint_axis_key_target(key);
            const PaintAxis axis   = paint_axis_key_axis(key);
            if (target != m_active_target || axis != edited_axis()) {
                m_active_target = target;
                m_axis_bar.set_xy(axis != paZ);
                m_axis_bar.set_axis(axis);
                load_active_curve();
            }
            return;
        }
        // Same content: keep the user's cached edits. Only fit them to the object if it was resized.
        if (m_object_max_z != new_max_z || extents_changed) {
            m_object_max_z = new_max_z;
            for (auto& kv : m_curves) {
                const std::vector<double> before = kv.second;
                variable_speed_fit_curve(kv.second, paint_axis_key_axis(kv.first), (double)new_max_z, m_axis_bar);
                if (auto bounds = m_bounds.find(kv.first); bounds != m_bounds.end())
                    variable_speed_fit_bounds(bounds->second, before, kv.second);
            }
            load_active_curve();
        }
        return;
    }

    // A different object became selected: load its painted curves from the model.
    load_curves_from_model(model_object_new);
}

void GLCanvas3D::SpeedEditing::load_curves_from_model(const ModelObject* model_object)
{
    m_object_max_z = static_cast<float>(model_object->bounding_box().max.z());
    m_curves_oid   = model_object->id();
    // Orio: a newly selected object may use a different material, so re-derive the scale
    // (its max tracks the material's volumetric flow).
    m_speed_range_valid = false;
    m_curves.clear();
    m_bounds           = model_object->speed_profile.bounds;
    m_committed_bounds = model_object->speed_profile.bounds;
    m_bound_drag       = -1;
    // Orio: the X / Y curves are fitted to the extents of the object, given to m_axis_bar by select_object().
    for (const auto& kv : model_object->speed_profile.curves) {
        std::vector<double> curve = kv.second;
        if (curve.size() < 4 || (curve.size() & 1) != 0)
            continue;
        // The curves of a later version (another axis) are kept as they are: committing must not drop them.
        variable_speed_fit_curve(curve, paint_axis_key_axis(kv.first), (double)m_object_max_z, m_axis_bar);
        if (auto bounds = m_bounds.find(kv.first); bounds != m_bounds.end())
            variable_speed_fit_bounds(bounds->second, kv.second, curve);
        m_curves[kv.first] = std::move(curve);
    }
    // Remember the model's own copy so a later external change (undo/redo, project reload) can be told
    // apart from a plain reslice, which never touches speed_profile.
    m_committed_curves = model_object->speed_profile.curves;
    // Keep editing the same type if it is still painted (along any axis), otherwise the lowest painted type, else none.
    auto type_painted = [this](uint32_t target) {
        for (PaintAxis axis : { paZ, paX, paY })
            if (painted(target, axis))
                return true;
        return false;
    };
    if (m_active_target == 0 || ! type_painted(m_active_target)) {
        uint32_t lowest = 0;
        for (const auto& kv : m_curves) {
            const uint32_t target = paint_axis_key_target(kv.first);
            if (target != 0 && paint_axis_is_valid(paint_axis_key_axis(kv.first)) && kv.second.size() >= 4 && (lowest == 0 || target < lowest))
                lowest = target;
        }
        m_active_target = lowest;
    }
    load_active_curve();
}

bool GLCanvas3D::SpeedEditing::is_allowed() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr && m_z_texture_id > 0;
}

bool GLCanvas3D::SpeedEditing::is_enabled() const
{
    return m_enabled;
}

void GLCanvas3D::SpeedEditing::set_enabled(bool enabled)
{
    m_enabled = is_allowed() && enabled;
    if (! m_enabled)
        m_part_raycasters.clear();
}

bool GLCanvas3D::SpeedEditing::is_initialized() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr;
}

bool GLCanvas3D::SpeedEditing::painted(uint32_t target, PaintAxis axis) const
{
    const auto it = m_curves.find(paint_axis_key(target, axis));
    return it != m_curves.end() && it->second.size() >= 4;
}

float GLCanvas3D::SpeedEditing::edit_length() const
{
    return m_axis_bar.is_xy() ? (float)m_axis_bar.length() : m_object_max_z;
}

double GLCanvas3D::SpeedEditing::edit_origin() const
{
    return m_axis_bar.is_xy() ? m_axis_bar.origin() : 0.;
}

float GLCanvas3D::SpeedEditing::neutral_speed() const
{
    // The slowest painted speed applies where several curves of the type are painted (GCode::VariableSpeedAxes): the
    // edited curve changes nothing as long as it is not slower than the others anywhere. The highest speed of each of
    // the others is such a value, the lowest of them the one that changes nothing for sure.
    const uint32_t edited = edited_key();
    bool           others = false;
    double         lowest = 0.;
    for (PaintAxis axis : { paZ, paX, paY }) {
        const uint32_t key = paint_axis_key(m_active_target, axis);
        const auto     it  = m_curves.find(key);
        if (m_active_target == 0 || key == edited || it == m_curves.end() || it->second.size() < 4)
            continue;
        double curve_max = std::numeric_limits<double>::lowest();
        for (size_t i = 1; i < it->second.size(); i += 2)
            curve_max = std::max(curve_max, it->second[i]);
        lowest = others ? std::min(lowest, curve_max) : curve_max;
        others = true;
    }
    const float value = others ? (float)lowest : m_base_speed;
    return std::clamp(value, m_min_speed, std::max(m_min_speed, m_max_speed));
}

std::vector<double> GLCanvas3D::SpeedEditing::display_curve() const
{
    if (m_speed_profile.size() >= 4)
        return m_speed_profile;
    // Not painted yet: flat at the speed in effect where it is not painted.
    return variable_speed_profile_flat((double)edit_length(), (double)neutral_speed());
}

void GLCanvas3D::SpeedEditing::ensure_profile()
{
    update_speed_range();
    const float length = edit_length();
    if (length <= 0.0f)
        return;
    if (m_speed_profile.size() < 4 || (m_speed_profile.size() & 1) != 0 ||
        std::abs(m_speed_profile.front()) > EPSILON ||
        std::abs(m_speed_profile[m_speed_profile.size() - 2] - (double)length) > 1e-3) {
        m_speed_profile = variable_speed_profile_flat((double)length, (double)neutral_speed());
        m_profile_changed = true;
    }
}

void GLCanvas3D::SpeedEditing::sync_active_into_curves()
{
    // Only a painted curve (>= 4 values) is stored, so merely selecting a type never makes it apply. Only a changed one
    // is written back: along X / Y the coordinates are converted back into the frame of the object.
    if (m_active_target == 0 || ! m_profile_changed || m_speed_profile.size() < 4)
        return;
    std::vector<double> curve  = m_speed_profile;
    const double        origin = edit_origin();
    if (origin != 0.)
        for (size_t i = 0; i < curve.size(); i += 2)
            curve[i] += origin;
    m_curves[edited_key()] = std::move(curve);
    m_profile_changed = false;
}

void GLCanvas3D::SpeedEditing::load_active_curve()
{
    auto it = (m_active_target != 0) ? m_curves.find(edited_key()) : m_curves.end();
    m_speed_profile = (it != m_curves.end()) ? it->second : std::vector<double>();
    // Along X / Y the bar starts at the origin of the axis.
    const double origin = edit_origin();
    if (origin != 0.)
        for (size_t i = 0; i < m_speed_profile.size(); i += 2)
            m_speed_profile[i] -= origin;
    m_profile_changed = false;
    m_speed_texture.valid = false;
    m_profile_dirty = true;
    // Orio: every change of the edited type goes through here, and each type has its own base / min (01/11).
    m_speed_range_valid = false;
}

void GLCanvas3D::SpeedEditing::set_active_target(uint32_t bit)
{
    if (bit == m_active_target)
        return;
    // Keep whatever the user just painted on the previous type before switching. Selecting a type
    // does not create a curve: the bar shows a flat preview until the user actually paints it, so a
    // type only starts applying its speed once painted.
    sync_active_into_curves();
    m_active_target = bit;
    load_active_curve();
}

void GLCanvas3D::SpeedEditing::set_xy(bool xy)
{
    if (xy == m_axis_bar.is_xy())
        return;
    // Keep whatever was painted along the previous axis before switching.
    sync_active_into_curves();
    m_axis_bar.set_xy(xy);
    load_active_curve();
    variable_speed_save_axis(m_axis_bar);
}

void GLCanvas3D::SpeedEditing::set_xy_axis(PaintAxis axis)
{
    if ((axis != paX && axis != paY) || axis == m_axis_bar.axis())
        return;
    // Keep whatever was painted along the previous axis before switching.
    sync_active_into_curves();
    m_axis_bar.set_axis(axis);
    load_active_curve();
    variable_speed_save_axis(m_axis_bar);
}

void GLCanvas3D::SpeedEditing::update_axis_bar(const GLCanvas3D& canvas)
{
    // The edited axis is the user's choice; only how it appears on the screen follows the view and the object. Unlike
    // the infill, the speed of every extrusion can be painted: no margin at the ends of the axis (the panel still leaves
    // room beside the ends of the bar, where the mouse paints them).
    if (m_axis_bar.is_xy() && m_model_object != nullptr)
        m_axis_bar.update_orientation(canvas, *m_model_object, canvas.get_selection().get_instance_idx());
    m_axis_bar.set_height(m_object_max_z);
}

void GLCanvas3D::SpeedEditing::show_tooltip_information(const GLCanvas3D& canvas, std::map<wxString, wxString> captions_texts, float x, float y)
{
    //Orio: shown by the keyboard icon right of the name of the tool, in the title bar of its window.
    wxGetApp().imgui()->tool_window_shortcuts(std::vector<std::pair<wxString, wxString>>(captions_texts.begin(), captions_texts.end()));
}

void GLCanvas3D::SpeedEditing::render_variable_speed_dialog(const GLCanvas3D& canvas)
{
    if (!m_enabled)
        return;

    update_speed_range();

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const Size& cnv_size = canvas.get_canvas_size();
    float         zoom = (float)canvas.get_active_camera().get_zoom();
    const auto& p_main_toolbar = canvas.get_main_toolbar();
    float left_pos = 0.0f;
    float main_toolbar_height = 0.0f;
    if (p_main_toolbar && p_main_toolbar->get_item("speedediting")) {
        left_pos = p_main_toolbar->get_item("speedediting")->render_rect[0];
        main_toolbar_height = canvas.main_toolbar_window_top(p_main_toolbar->get_item("speedediting")->render_rect[3]); //Orio
    }
    float x = 0.5 * cnv_size.get_width() + left_pos * zoom;

    const auto canvas_width = cnv_size.get_width();
    if (abs(GLCanvas3D::SpeedEditing::s_overlay_window_width) > 0.01f) {
        if (x + GLCanvas3D::SpeedEditing::s_overlay_window_width > canvas_width) {
            if (GLCanvas3D::SpeedEditing::s_overlay_window_width > canvas_width)
                x = 0;
            else
                x = canvas_width - GLCanvas3D::SpeedEditing::s_overlay_window_width;
        }
    }
    x = std::max(x, 0.0f);

    imgui.set_next_window_pos(x, main_toolbar_height, ImGuiCond_Always, 0.0f, 0.0f);

    imgui.push_toolbar_style(canvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * canvas.get_scale(), 4.0f * canvas.get_scale()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * canvas.get_scale(), 10.0f * canvas.get_scale()));
    // Orio: floating, collapsible window (ImGuiWrapper::begin_tool_window), its place remembered under the name of
    // the icon of the tool; default place under the icon, as above.
    imgui.begin_tool_window("toolbar_variable_speed", _L("Variable speed"), wxGetApp().dark_mode() ? "toolbar_variable_speed_dark.svg" : "toolbar_variable_speed.svg",
                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    const float sliders_width = imgui.scaled(7.0f);
    const float input_box_width = 3.0f * imgui.get_slider_icon_size().x;

    static float speed_text_align = ImGui::GetCursorPosX();
    static float speed_input_align = ImGui::GetCursorPosX();

    // Orio: two columns, half as high as one (asked by the user, laid out by the "UX/UI" session, 05/10): the
    // settings of the tool on the left, the speed by type on the right. The window stays against the right edge of the
    // scene (see above): it grows to the left. In ImGui 1.83 a Separator() spans the whole window: within the left
    // column a line is drawn instead, as wide as the column was at the previous frame.
    const ImGuiStyle& style = ImGui::GetStyle();
    static float left_column_width = 0.0f;
    if (left_column_width <= 0.0f)
        left_column_width = imgui.scaled(16.0f);
    ImGui::BeginGroup();

    auto align_label = [&](const wxString& label) {
        ImGui::AlignTextToFramePadding();
        speed_text_align = std::max(speed_text_align, ImGui::GetCursorPosX());
        ImGui::SetCursorPosX(speed_text_align);
        imgui.text(label);
        ImGui::SameLine();
        speed_input_align = std::max(speed_input_align, ImGui::GetCursorPosX());
        ImGui::SetCursorPosX(speed_input_align);
    };

    // Orio: along Z (the bar on the right) or along the X / Y axes of the object (the bar at the bottom of the
    // scene), as in "Variable infill and walls". The other choice is marked "(edited)" when a type is painted there: it
    // applies too, the slowest painted speed winning.
    auto painted_in = [this](bool xy) {
        for (const auto& kv : m_curves) {
            const PaintAxis axis = paint_axis_key_axis(kv.first);
            if (paint_axis_key_target(kv.first) != 0 && kv.second.size() >= 4 && (xy ? (axis == paX || axis == paY) : axis == paZ))
                return true;
        }
        return false;
    };
    align_label(_L("Axis"));
    bool xy = m_axis_bar.is_xy();
    if (PaintAxisBar::render_axis_toggle(xy, painted_in(false), painted_in(true)))
        set_xy(xy);
    if (m_axis_bar.is_xy())
        imgui.text_wrapped(_L("Paints along the X or Y axis of the object, chosen with the X / Y buttons over the bar. "
                              "Where several axes are painted, the slowest speed applies."), imgui.scaled(16.0f));

    auto labelled_float_input = [&](const wxString& label, const char* id, float& value, float step, float v_min, float v_max) -> bool {
        align_label(label);
        ImGui::PushItemWidth(input_box_width);
        const bool changed = ImGui::BBLDragFloat(id, &value, step, v_min, v_max, "%.0f");
        ImGui::PopItemWidth();
        if (changed)
            value = std::clamp(value, v_min, v_max);
        return changed;
    };

    // Orio: the revert arrow shown next to a modified setting (same icon as the object manipulation panel and the
    // variable infill / walls tool), or its room left empty so that the width does not change when it appears.
    const float icon_w = ImGui::GetFontSize() + 2.0f * style.FramePadding.x;
    auto revert_icon = [&](const std::string& id, bool shown, const wxString& tooltip) -> bool {
        if (! shown) {
            ImGui::Dummy(ImVec2(icon_w, ImGui::GetFrameHeight()));
            return false;
        }
        ImGui::PushID(id.c_str()); // disambiguate: every icon shares the same texture
        const float icon_size = ImGui::GetFontSize();
        ImTextureID normal_id = canvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET);
        ImTextureID hover_id  = canvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET_HOVER);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        const bool clicked = ImGui::ImageButton3(normal_id, hover_id, ImVec2(icon_size, icon_size));
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered())
            imgui.tooltip(tooltip, imgui.calc_text_size(tooltip).x + 2.0f * style.WindowPadding.x);
        ImGui::PopID();
        return clicked;
    };

    // Base speed: the value a freshly painted object starts from and the one "Reset to base"
    // and "Smooth" converge to. Orio: the speed the edited type is really printed at (see update_speed_range), and
    // the min 10% of it, until the user types them: then they are kept for this type until its revert arrow or until Orio
    // is closed (10/11).
    const bool base_changed = labelled_float_input(_L("Base speed (mm/s)"), "##speed_base", m_base_speed, 1.0f, (float)MIN_VARIABLE_SPEED, (float)MAX_VARIABLE_SPEED);
    if (ImGui::IsItemHovered())
        imgui.tooltip(_L("Speed the ticked type is really printed at: its speed in the print settings, limited by the "
                         "maximum volumetric flow of the filament. A value typed here is kept for this type until Orio is closed."),
                      ImGui::GetFontSize() * 20.0f);
    ImGui::SameLine();
    const bool base_typed = m_user_base_speeds.count(m_active_target) != 0;
    if (revert_icon("##speed_base_revert", base_typed, _L("Back to the speed of the print settings"))) {
        m_user_base_speeds.erase(m_active_target);
        m_speed_range_valid = false;
    }
    // Orio: the value of the print settings cut by the filament's flow, so that the base does not look wrong (10/11).
    if (! base_typed && m_preset_speed > m_max_speed + 0.5f) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        imgui.text_wrapped(wxString::Format(_L("Print settings: %.0f, limited to %.0f by the filament flow"), m_preset_speed, m_max_speed),
                           imgui.scaled(16.0f));
        ImGui::PopStyleColor();
    }
    const bool min_changed = labelled_float_input(_L("Scale min (mm/s)"), "##speed_min", m_min_speed, 1.0f, (float)MIN_VARIABLE_SPEED, (float)MAX_VARIABLE_SPEED);
    ImGui::SameLine();
    if (revert_icon("##speed_min_revert", m_user_min_speeds.count(m_active_target) != 0, _L("Back to a tenth of the base speed"))) {
        m_user_min_speeds.erase(m_active_target);
        m_speed_range_valid = false;
    }
    // Orio: Scale max is NOT editable — it is always derived from the volumetric flow limit of
    // the object's material (see update_speed_range / object_volumetric_speed_max), so it is shown
    // read-only.
    align_label(_L("Scale max (mm/s)"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    imgui.text(wxString::Format("%.0f", m_max_speed));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        imgui.tooltip(_L("Automatically set from the maximum volumetric flow of this object's filament."), ImGui::GetFontSize() * 20.0f);
    // Orio: the step is remembered across sessions (see load/save_persisted_fields).
    bool fields_changed = labelled_float_input(_L("Step (mm/s)"), "##speed_step", strength, 0.5f, 0.1f, 100.0f);
    m_step_user_set |= fields_changed;
    if (base_changed || min_changed) {
        // Scale max is computed from the material, so keep it fixed and clamp the editable fields under it.
        if (m_min_speed > m_max_speed - 1.0f)
            m_min_speed = std::max((float)MIN_VARIABLE_SPEED, m_max_speed - 1.0f);
        m_base_speed = std::clamp(m_base_speed, m_min_speed, m_max_speed);
        // Orio: no type ticked, nothing can be painted: the value is not kept.
        if (m_active_target != 0) {
            if (base_changed)
                m_user_base_speeds[m_active_target] = m_base_speed;
            if (min_changed)
                m_user_min_speeds[m_active_target] = m_min_speed;
        }
        m_speed_texture.valid = false;
        m_profile_dirty = true;
        fields_changed = true;
    }

    // The separator, within the left column.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + left_column_width, p.y), ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    // Orio: the verb ("Lisser" in French, not the adjective "Lisse"), as in "Variable infill and walls".
    if (imgui.button(_CTX(L_CONTEXT("Smooth", "Verb"), "Verb")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), HeightProfileSmoothEvent(EVT_GLCANVAS_SMOOTH_SPEED_PROFILE, m_smooth_params));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Radius"));
    ImGui::SameLine();
    ImGui::PushItemWidth(sliders_width);
    int radius = (int)m_smooth_params.radius;
    int v_min = 1, v_max = 10;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.81f, 0.81f, 0.81f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(8.0f / 255.0f, 119.0f / 255.0f, 232.0f / 255.0f, 1.00f));
    if (ImGui::BBLSliderScalar("##speed_radius_slider", ImGuiDataType_S32, &radius, &v_min, &v_max)) {
        radius = std::clamp(radius, 1, 10);
        m_smooth_params.radius = (unsigned int)radius;
        fields_changed = true;
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::PopItemWidth();

    fields_changed |= imgui.bbl_checkbox("##speed_keep_min", m_smooth_params.keep_min);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Keep min"));

    // Orio: the other objects hidden while this one is edited (hides()), saved on its own.
    if (imgui.bbl_checkbox("##speed_hide_other_objects", m_hide_other_objects)) {
        if (auto* app_config = wxGetApp().app_config; app_config != nullptr)
            app_config->set("speed_editing", "hide_other_objects", m_hide_other_objects);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Hide the other objects"));

    // Orio: persist the tool's numeric fields so they are restored on the next session.
    if (fields_changed)
        save_persisted_fields();

    // The mouse help (keyboard icon), at the bottom of the left column; its tooltip goes under the window.
    // Orio: under the window where it is (it can be moved).
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + ImGui::GetWindowPos().y;
    std::map<wxString, wxString> captions_texts = {
        {_L("Left mouse button:") ,_L("Slow down")},
        {_L("Right mouse button:"), _L("Speed up")},
        {_L("Shift + Left mouse button:"),_L("Reset to base speed")},
        {_L("Shift + Right mouse button:"), _L("Smoothing")},
        {_L("Mouse wheel:"), _L("Increase/decrease edit area")},
        // Orio: anywhere in the scene, as in the variable infill / walls and layer height tools.
        {_L("Ctrl + Mouse wheel:"), _L("Increase/decrease edit area")},
        // Orio: also on the part, at the height (or the position along X / Y) of the point clicked.
        {_L("On the part:"), _L("Same buttons as on the bar")},
        // Orio: the bounds the painting does not cross.
        {_L("Right click on the graduations:"), _L("Place or delete a bound")}
    };
    // Orio: no global "Reset type" button any more; each edited type carries its own revert
    // icon (mirroring how a modified print setting is reset).
    show_tooltip_information(canvas, captions_texts, ImGui::GetWindowPos().x, get_cur_y);

    ImGui::EndGroup();
    const ImVec2 left_min = ImGui::GetItemRectMin();
    const ImVec2 left_max = ImGui::GetItemRectMax();
    left_column_width = left_max.x - left_min.x;

    // The right column, after a gap holding a thin vertical line (drawn below, once the height of both columns is known).
    ImGui::SameLine(0.0f, 2.0f * style.ItemSpacing.x);
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Speed by type"));

    // Orio: one independent speed curve per extrusion type. The checkbox enables a speed
    // override for that type; clicking its name makes it the "active" type edited by the bar on the
    // right, so infill and perimeters can carry different painted speeds at the same layer.
    struct TargetItem { uint32_t bit; wxString label; };
    const TargetItem targets[] = {
        { (uint32_t)vstOuterWall,    _L("Outer wall") },
        { (uint32_t)vstInnerWall,    _L("Inner wall") },
        { (uint32_t)vstSparseInfill, _L("Sparse infill") },
        { (uint32_t)vstSolidInfill,  _L("Solid infill") },
        { (uint32_t)vstTopSurface,   _L("Top surface") },
        // Orio: the bridges only (14/10); the overhang walls are set in the print settings (01/11).
        { (uint32_t)vstBridge,       _L("Bridge") },
        { (uint32_t)vstSupport,      _L("Support") },
        { (uint32_t)vstGapFill,      _L("Gap infill") },
        { (uint32_t)vstIroning,      _L("Ironing") },
    };
    const int targets_count = (int)(sizeof(targets) / sizeof(targets[0]));

    // Orio: the column has a fixed width, from its widest name: in ImGui 1.83 a Selectable given a width ignores
    // its text, and GetContentRegionAvail() would let the auto-resizing window grow at every frame. The room of the revert
    // icon (icon_w) is always kept, so that a row becoming painted does not change the width.
    float name_w = imgui.calc_text_size(_L("Speed by type")).x;
    for (int item_idx = 0; item_idx < targets_count; ++ item_idx)
        name_w = std::max(name_w, imgui.calc_text_size(targets[item_idx].label).x);

    bool profile_changed = false;
    for (int item_idx = 0; item_idx < targets_count; ++ item_idx) {
        const TargetItem& item = targets[item_idx];
        // Single-choice selection: the ticked box is the type being edited by the bar on the right.
        // Ticking a type unticks the previous one. Applying a speed is decided separately by whether
        // the type has a painted curve (m_curves), so already-painted types keep applying while another
        // type is being edited.
        bool checked = (m_active_target == item.bit);
        const std::string id = "##speed_target_" + std::to_string(item.bit);
        if (imgui.bbl_checkbox(id.c_str(), checked)) {
            // Ticking selects this type (auto-unticks the previous); unticking the active one leaves
            // nothing selected. Painted curves are kept either way.
            set_active_target(checked ? item.bit : 0);
            profile_changed = true;
        }
        ImGui::SameLine();
        // A type that has a painted curve is shown in the app's "modified" colour with a revert icon on
        // the right that clears just this type, exactly like a modified print setting. Selecting a type
        // for editing is done with the checkbox or by clicking its name; the row itself is not highlighted.
        // Orio: painted along the shown axes: Z, or X / Y (either of them, the icon clears both).
        const bool has_curve = m_axis_bar.is_xy() ? (painted(item.bit, paX) || painted(item.bit, paY)) : painted(item.bit, paZ);
        const std::string row = std::string(item.label.ToUTF8().data()) + "##sel_" + std::to_string(item.bit);
        if (has_curve)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(241.0f / 255.0f, 117.0f / 255.0f, 78.0f / 255.0f, 1.0f)); // #F1754E
        // The clickable name never sits under the revert icon, which has its own room on the right (see above).
        if (ImGui::Selectable(row.c_str(), false, 0, ImVec2(name_w, ImGui::GetFrameHeight()))) {
            set_active_target(item.bit);
            profile_changed = true;
        }
        if (has_curve)
            ImGui::PopStyleColor();
        ImGui::SameLine();
        if (revert_icon("##speed_reset_" + std::to_string(item.bit), has_curve, _L("Reset type"))) {
            // Clear only this type; go through the event so the undo snapshot is taken outside the
            // render pass (see reset_speed_profile).
            m_reset_bit = item.bit;
            wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), SimpleEvent(EVT_GLCANVAS_RESET_SPEED_PROFILE));
        }
    }
    // Do not take an undo snapshot from inside the render pass, just push the new curves and
    // let the background process pick them up.
    if (profile_changed && m_model_object != nullptr)
        commit_to_model(const_cast<GLCanvas3D&>(canvas), std::string());

    ImGui::EndGroup();
    // Orio: the thin vertical line between the columns, from the top to the bottom of the higher one.
    {
        const ImVec2 right_min = ImGui::GetItemRectMin();
        const ImVec2 right_max = ImGui::GetItemRectMax();
        const float  line_x    = 0.5f * (left_max.x + right_min.x);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(line_x, std::min(left_min.y, right_min.y)), ImVec2(line_x, std::max(left_max.y, right_max.y)),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
    }

    GLCanvas3D::SpeedEditing::s_overlay_window_width = ImGui::GetWindowSize().x;
    imgui.end_tool_window();
    ImGui::PopStyleVar(2);
    imgui.pop_toolbar_style();
}

void GLCanvas3D::SpeedEditing::render_overlay(const GLCanvas3D& canvas)
{
    update_speed_range();
    // Orio: along X / Y, how the edited axis appears on the screen follows the view and the object; along Z, the
    // bar spans the height of the object.
    update_axis_bar(canvas);
    // An unpainted curve is shown flat at the speed in effect, which follows the base speed and the other painted
    // curves of the type.
    if (m_displayed_neutral != neutral_speed()) {
        m_speed_texture.valid = false;
        m_profile_dirty = true;
    }
    render_variable_speed_dialog(canvas);
    if (edit_length() <= 0.0f)
        return;
    // The bar sits in a framed panel (PaintAxisBar), at the bottom of the scene along X / Y, at its right along Z, whose
    // background goes under it.
    if (m_enabled)
        m_axis_bar.render_panel_background(canvas);
    render_background_texture(canvas);
    const Rect& bar_rect = get_bar_rect_viewport(canvas);
    render_curve(bar_rect);
    if (m_enabled) {
        // The rest of the panel, with the X / Y switch along X / Y. It tells whether the other axis is painted too for the
        // ticked type: its painting applies, though the object is colored by the edited axis only.
        const std::string title   = _u8L("Variable speed") + " \xC2\xB7 " + (m_axis_bar.is_xy() ? _u8L("axis") : std::string("Z"));
        const PaintAxis   clicked = m_axis_bar.render_panel(canvas, title, painted(m_active_target, paX), painted(m_active_target, paY));
        // The speed under the mouse goes beside the panel, where it hides nothing (see get_tooltip()).
        // Orio: over the part, the bubble goes along the bar at the coordinate of the point under the mouse.
        m_axis_bar.render_value_tip(canvas, cursor_value_text(canvas), cursor_coordinate(canvas) / edit_length());
        if (clicked != paZ)
            set_xy_axis(clicked);
        // Orio: the bounds of the edited curve, over the bar and its graduations.
        render_bounds(canvas);
    }
}

// Orio: bounds the painting does not cross (11/10). Between two bounds, and from the ends of the bar, mm.
static constexpr double SPEED_BOUND_MIN_GAP = 0.05;

std::vector<double> GLCanvas3D::SpeedEditing::active_bounds() const
{
    std::vector<double> ret;
    const auto it = (m_active_target != 0) ? m_bounds.find(edited_key()) : m_bounds.end();
    if (it == m_bounds.end())
        return ret;
    const double origin = edit_origin();
    const double length = edit_length();
    for (const double bound : it->second)
        if (bound - origin > SPEED_BOUND_MIN_GAP && bound - origin < length - SPEED_BOUND_MIN_GAP)
            ret.push_back(bound - origin);
    std::sort(ret.begin(), ret.end());
    return ret;
}

void GLCanvas3D::SpeedEditing::set_active_bounds(std::vector<double> bounds)
{
    if (m_active_target == 0)
        return;
    std::sort(bounds.begin(), bounds.end());
    const double origin = edit_origin();
    for (double& bound : bounds)
        bound += origin;
    if (bounds.empty())
        m_bounds.erase(edited_key());
    else
        m_bounds[edited_key()] = std::move(bounds);
}

std::vector<double> GLCanvas3D::SpeedEditing::object_layer_tops() const
{
    std::vector<double> tops;
    if (m_model_object == nullptr || m_config == nullptr || m_object_max_z <= 0.0f)
        return tops;
    // As GLCanvas3D::LayersEditing does it.
    const SlicingParameters params = PrintObject::slicing_parameters(*m_config, *m_model_object, m_object_max_z);
    std::vector<coordf_t>   profile;
    bool                    nozzle_range_reset = false;
    PrintObject::update_layer_height_profile(*m_model_object, params, profile, nozzle_range_reset);
    if (profile.empty())
        return tops;
    const std::vector<coordf_t> layers = generate_object_layers(params, profile, false);
    for (size_t i = 1; i < layers.size(); i += 2)
        tops.push_back(layers[i]);
    return tops;
}

void GLCanvas3D::SpeedEditing::render_bounds(const GLCanvas3D& canvas)
{
    const float length = edit_length();
    if (! m_enabled || length <= 0.0f || this->last_object_id < 0)
        return;
    const bool   xy    = m_axis_bar.is_xy();
    const Rect   bar   = m_axis_bar.rect_screen(canvas);
    const float  size  = PaintAxisBar::thickness(canvas); // the width of the bar
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // Over the graduations, which an ImGui window of the panel covers: the scene does not get these clicks. Not while the
    // menu is open: a click on it is not for a bound under it.
    const bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && ! ImGui::IsPopupOpen("speed_bounds_menu") &&
        (xy ? (bar.get_left() <= mouse.x && mouse.x <= bar.get_right() && bar.get_bottom() < mouse.y && mouse.y <= bar.get_bottom() + 0.6f * size) :
              (bar.get_left() - 1.2f * size <= mouse.x && mouse.x < bar.get_left() && bar.get_top() <= mouse.y && mouse.y <= bar.get_bottom()));
    // A coordinate along the bar on the screen, and the coordinate under the mouse; along Z, brought to the top of a layer.
    auto screen_of = [&](double coordinate) {
        const float t = float(coordinate / length);
        if (! xy)
            return bar.get_bottom() - t * (bar.get_bottom() - bar.get_top());
        return m_axis_bar.flipped() ? bar.get_right() - t * (bar.get_right() - bar.get_left()) : bar.get_left() + t * (bar.get_right() - bar.get_left());
    };
    std::vector<double> layer_tops;
    auto coordinate_at_mouse = [&]() {
        double coordinate = (double)length * std::clamp(m_axis_bar.relative_along(canvas, mouse.x, mouse.y), 0.0f, 1.0f);
        if (! xy) {
            if (layer_tops.empty())
                layer_tops = object_layer_tops();
            double nearest = std::numeric_limits<double>::max();
            const double wanted = coordinate;
            for (const double top : layer_tops)
                if (top > SPEED_BOUND_MIN_GAP && top < (double)length - SPEED_BOUND_MIN_GAP && std::abs(top - wanted) < nearest) {
                    nearest    = std::abs(top - wanted);
                    coordinate = top;
                }
        }
        return coordinate;
    };

    std::vector<double> bounds = active_bounds();
    // The bound under the mouse.
    int hovered = -1;
    if (over && m_bound_drag < 0) {
        const float along = xy ? mouse.x : mouse.y;
        float       best  = 0.15f * size;
        for (size_t i = 0; i < bounds.size(); ++i)
            if (const float distance = std::abs(screen_of(bounds[i]) - along); distance <= best) {
                best    = distance;
                hovered = (int)i;
            }
    }
    // Dragging one, between its neighbours; one undo step when it is released.
    if (m_bound_drag >= (int)bounds.size())
        m_bound_drag = -1;
    if (m_bound_drag >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const double low        = (m_bound_drag > 0 ? bounds[m_bound_drag - 1] : 0.) + SPEED_BOUND_MIN_GAP;
            const double high       = (m_bound_drag + 1 < (int)bounds.size() ? bounds[m_bound_drag + 1] : (double)length) - SPEED_BOUND_MIN_GAP;
            const double coordinate = coordinate_at_mouse();
            if (coordinate >= low && coordinate <= high && coordinate != bounds[m_bound_drag]) {
                bounds[m_bound_drag] = coordinate;
                set_active_bounds(bounds);
            }
        } else {
            m_bound_drag = -1;
            commit_to_model(m_canvas, "Variable speed - Move bound");
        }
    } else if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_bound_drag = hovered;
    // The menu, by a right click over the graduations.
    if (over && m_bound_drag < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_bounds_menu_coordinate = coordinate_at_mouse();
        m_bounds_menu_bound      = hovered;
        ImGui::OpenPopup("speed_bounds_menu");
    }
    if (ImGui::BeginPopup("speed_bounds_menu")) {
        const double coordinate = m_bounds_menu_coordinate;
        // Not on another one, nor at an end of the bar; a type to paint is ticked.
        const bool room = m_active_target != 0 && coordinate > SPEED_BOUND_MIN_GAP && coordinate < (double)length - SPEED_BOUND_MIN_GAP &&
            std::none_of(bounds.begin(), bounds.end(), [coordinate](double bound) { return std::abs(bound - coordinate) < SPEED_BOUND_MIN_GAP; });
        if (ImGui::MenuItem(_u8L("Place a bound").c_str(), nullptr, false, room)) {
            bounds.push_back(coordinate);
            set_active_bounds(bounds);
            commit_to_model(m_canvas, "Variable speed - Add bound");
        } else if (m_bounds_menu_bound >= 0 && m_bounds_menu_bound < (int)bounds.size() && ImGui::MenuItem(_u8L("Delete the bound").c_str())) {
            bounds.erase(bounds.begin() + m_bounds_menu_bound);
            set_active_bounds(bounds);
            commit_to_model(m_canvas, "Variable speed - Delete bound");
        } else if (! bounds.empty() && ImGui::MenuItem(_u8L("Delete all the bounds").c_str())) {
            set_active_bounds({});
            commit_to_model(m_canvas, "Variable speed - Delete bounds");
        }
        ImGui::EndPopup();
    }

    // Drawn over the bar drawn in GL, under the ImGui windows: a line across the bar, and a tab over the graduations.
    bounds = active_bounds();
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const ImU32 color     = ImGui::GetColorU32(ImGuiWrapper::COL_ORANGE_LIGHT);
    const float tab_long  = std::round(0.3f * size);
    const float tab_half  = std::max(2.0f, std::round(0.09f * size));
    for (size_t i = 0; i < bounds.size(); ++i) {
        const float p     = std::round(screen_of(bounds[i]));
        const float width = ((int)i == hovered || (int)i == m_bound_drag) ? std::max(2.0f, std::round(0.07f * size)) : std::max(1.0f, std::round(0.035f * size));
        if (xy) {
            draw_list->AddLine(ImVec2(p, bar.get_top()), ImVec2(p, bar.get_bottom()), color, width);
            draw_list->AddRectFilled(ImVec2(p - tab_half, bar.get_bottom()), ImVec2(p + tab_half, bar.get_bottom() + tab_long), color, 0.5f * tab_half);
        } else {
            draw_list->AddLine(ImVec2(bar.get_left(), p), ImVec2(bar.get_right(), p), color, width);
            draw_list->AddRectFilled(ImVec2(bar.get_left() - tab_long, p - tab_half), ImVec2(bar.get_left(), p + tab_half), color, 0.5f * tab_half);
        }
    }
    // The bound under the mouse or dragged: its height and its layer along Z (the layer whose top it is, the last one
    // painted on its lower side), its coordinate along X / Y. Over everything, as the graduations are under it.
    const int labelled = (m_bound_drag >= 0) ? m_bound_drag : hovered;
    if (labelled >= 0 && labelled < (int)bounds.size()) {
        const double coordinate = bounds[labelled];
        char         mm[64];
        snprintf(mm, sizeof(mm), "%.2f mm", coordinate);
        std::string text = mm;
        if (! xy) {
            if (layer_tops.empty())
                layer_tops = object_layer_tops();
            const auto top = std::lower_bound(layer_tops.begin(), layer_tops.end(), coordinate - EPSILON);
            if (top != layer_tops.end())
                text = _u8L("Layer") + " " + std::to_string(int(top - layer_tops.begin()) + 1) + " \xC2\xB7 " + text;
        }
        const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
        const float  padding   = std::max(2.0f, std::round(0.08f * size));
        const float  p         = screen_of(coordinate);
        const ImVec2 min       = xy ? ImVec2(p + tab_half + padding, bar.get_bottom() + padding) :
                                      ImVec2(bar.get_left() - tab_long - 2.0f * padding - (text_size.x + 2.0f * padding), p - 0.5f * text_size.y - padding);
        const ImVec2 max(min.x + text_size.x + 2.0f * padding, min.y + text_size.y + 2.0f * padding);
        ImDrawList*  top_list = ImGui::GetForegroundDrawList();
        top_list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_PopupBg), padding);
        top_list->AddRect(min, max, color, padding);
        top_list->AddText(ImVec2(min.x + padding, min.y + padding), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    }
}

float GLCanvas3D::SpeedEditing::cursor_coordinate(const GLCanvas3D& canvas) const
{
    const float relative = m_axis_bar.cursor_relative(canvas);
    if (relative != -1000.0f)
        return edit_length() * relative;
    // Orio: over the part, where a click would paint.
    const Vec2d mouse   = canvas.get_local_mouse_position();
    const float on_part = part_coordinate_at(canvas, (float)mouse.x(), (float)mouse.y());
    return std::isnan(on_part) ? -1000.0f : on_part;
}

bool GLCanvas3D::SpeedEditing::bar_rect_contains(const GLCanvas3D& canvas, float x, float y) const
{
    // The bar, or beside its ends in its panel: a stroke may start just past them.
    return m_axis_bar.contains(canvas, x, y);
}

Rect GLCanvas3D::SpeedEditing::get_bar_rect_screen(const GLCanvas3D& canvas) const
{
    return m_axis_bar.rect_screen(canvas);
}

Rect GLCanvas3D::SpeedEditing::get_bar_rect_viewport(const GLCanvas3D& canvas) const
{
    return m_axis_bar.rect_viewport(canvas);
}

float GLCanvas3D::SpeedEditing::edit_coordinate_at(const GLCanvas3D& canvas, float x, float y) const
{
    // Orio: a stroke started on the part follows the part, and pauses while the mouse is off it (NaN).
    if (this->part_stroke)
        return part_coordinate_at(canvas, x, y);
    // Brought onto the axis: a stroke beside the ends of the bar, or beyond them, paints its ends.
    return edit_length() * m_axis_bar.paintable(m_axis_bar.relative_along(canvas, x, y));
}

void GLCanvas3D::SpeedEditing::update_part_raycasters() const
{
    size_t i    = 0;
    bool   same = true;
    if (m_model_object != nullptr)
        for (const ModelVolume* volume : m_model_object->volumes)
            if (volume->is_model_part()) {
                same = same && i < m_part_raycasters.size() && m_part_raycasters[i].volume == volume &&
                       m_part_raycasters[i].mesh == volume->get_mesh_shared_ptr();
                ++i;
            }
    if (same && i == m_part_raycasters.size())
        return;
    m_part_raycasters.clear();
    if (m_model_object != nullptr)
        for (const ModelVolume* volume : m_model_object->volumes)
            if (volume->is_model_part())
                m_part_raycasters.push_back({ volume, volume->get_mesh_shared_ptr(), std::make_shared<MeshRaycaster>(volume->mesh()) });
}

float GLCanvas3D::SpeedEditing::part_coordinate_at(const GLCanvas3D& canvas, float x, float y) const
{
    float ret = std::numeric_limits<float>::quiet_NaN();
    update_part_raycasters();
    const int   instance_idx = canvas.get_selection().get_instance_idx();
    const float length       = edit_length();
    if (m_model_object == nullptr || instance_idx < 0 || instance_idx >= (int)m_model_object->instances.size() || length <= 0.0f)
        return ret;
    const Camera&     camera   = canvas.get_active_camera();
    const Transform3d instance = m_model_object->instances[instance_idx]->get_matrix();
    // The nearest of the hits on the solid parts.
    double nearest = std::numeric_limits<double>::max();
    for (const PartRaycaster& part : m_part_raycasters) {
        const Transform3d volume = part.volume->get_matrix();
        Vec3f position, normal;
        if (! part.raycaster->unproject_on_mesh(Vec2d(x, y), instance * volume, camera, position, normal))
            continue;
        const Vec3d world = instance * volume * position.cast<double>();
        const double depth = (world - camera.get_position()).dot(camera.get_dir_forward());
        if (depth >= nearest)
            continue;
        nearest = depth;
        // As the object is colored: along Z its height in the scene, along X / Y the coordinate in the frame of the
        // object, where the curves are, minus the origin of the axis (PaintAxisBar::volume_axis_matrix()).
        const Vec3d  in_object = volume * position.cast<double>();
        const double along     = m_axis_bar.is_xy() ? (m_axis_bar.axis() == paY ? in_object.y() : in_object.x()) - edit_origin() : world.z();
        ret = std::clamp((float)along, 0.0f, length);
    }
    return ret;
}

float GLCanvas3D::SpeedEditing::bottom_bar_height(const GLCanvas3D& canvas) const
{
    return (m_enabled && m_axis_bar.is_xy() && this->last_object_id >= 0) ? m_axis_bar.reserved_height(canvas) : 0.0f;
}

float GLCanvas3D::SpeedEditing::right_bar_width(const GLCanvas3D& canvas) const
{
    return (m_enabled && ! m_axis_bar.is_xy() && this->last_object_id >= 0) ? m_axis_bar.reserved_width(canvas) : 0.0f;
}

bool GLCanvas3D::SpeedEditing::hides(const GLVolume& volume) const
{
    return m_enabled && m_hide_other_objects && this->last_object_id >= 0 && volume.composite_id.object_id != this->last_object_id;
}

std::string GLCanvas3D::SpeedEditing::get_tooltip(const GLCanvas3D&) const
{
    // Orio: a tooltip next to the mouse would hide the bar: the panel shows the speed beside itself instead
    // (render_overlay()).
    return std::string();
}

std::string GLCanvas3D::SpeedEditing::cursor_value_text(const GLCanvas3D& canvas) const
{
    std::string ret;
    const float length = edit_length();
    if (m_enabled && length > 0.0f) {
        const float z = cursor_coordinate(canvas);
        if (z != -1000.0f) {
            // Where the edited curve was not painted, the speed in effect there.
            const double v = Slic3r::variable_speed_at(m_speed_profile, (double)z, (double)neutral_speed());
            ret = GUI::format("%1% mm/s", (int)std::lround(v));
        }
    }
    return ret;
}

void GLCanvas3D::SpeedEditing::render_background_texture(const GLCanvas3D& canvas)
{
    if (!m_enabled)
        return;

    const Size cnv_size = canvas.get_canvas_size();
    const int cnv_width = cnv_size.get_width();
    const int cnv_height = cnv_size.get_height();
    if (cnv_width == 0 || cnv_height == 0)
        return;
    // Orio: the length of the edited axis, the height of the object along Z.
    const float length = edit_length();
    if (length <= 0.0f)
        return;

    const auto& shader = wxGetApp().get_shader("variable_layer_height");
    if (shader == nullptr)
        return;

    generate_speed_texture();

    wxGetApp().bind_shader(shader);

    shader->set_uniform("z_to_texture_row", float(m_speed_texture.cells - 1) / (float(m_speed_texture.width) * length));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / (float)m_speed_texture.height);
    shader->set_uniform("z_cursor", this->cursor_coordinate(canvas));
    shader->set_uniform("z_cursor_band_width", band_width);
    shader->set_uniform("object_max_z", length);

    glsafe(::glActiveTexture(GL_TEXTURE0 + 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));

    const GLsizei w = (GLsizei)m_speed_texture.width;
    const GLsizei h = (GLsizei)m_speed_texture.height;
    const GLsizei half_w = w / 2;
    const GLsizei half_h = h / 2;
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, half_w, half_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_speed_texture.data.data()));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, half_w, half_h, GL_RGBA, GL_UNSIGNED_BYTE, m_speed_texture.data.data() + m_speed_texture.width * m_speed_texture.height * 4));

    // Orio: the bar in its panel, at the bottom of the scene along X / Y, at its right along Z.
    m_axis_bar.render_background(canvas, *shader);

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    wxGetApp().unbind_shader();
}

void GLCanvas3D::SpeedEditing::render_curve(const Rect& bar_rect)
{
    const float length = edit_length();
    if (!m_enabled || length <= 0.0f)
        return;

    const auto& shader = wxGetApp().get_shader("flat");
    if (!shader)
        return;

    wxGetApp().bind_shader(shader);

    // Orio: across the bar, the speed; along the bar, the position along the edited axis. The vertical bar of Z
    // grows upwards, the horizontal bar of X / Y from left to right (from right to left when the axis points to the
    // left).
    const bool  xy         = m_axis_bar.is_xy();
    const float speed_span = std::max(m_max_speed - m_min_speed, 1.0f);
    // Reference line at the speed in effect where the edited curve is not painted (the base speed, or the slowest
    // speed the other curves of the type leave).
    const float neutral    = neutral_speed();

    const Camera& camera = m_canvas.get_active_camera();
    Transform3d view_matrix = camera.get_view_matrix_for_billboard();

    Transform3d model_matrix{ Transform3d::Identity() };
    if (! xy) {
        const float x = bar_rect.get_left() + (neutral - m_min_speed) * bar_rect.get_width() / speed_span;
        model_matrix.data()[3 * 4 + 0] = x;
        model_matrix.data()[3 * 4 + 1] = (bar_rect.get_top() + bar_rect.get_bottom()) * 0.5f;
        model_matrix.data()[0 * 4 + 0] = 0.0f;
        model_matrix.data()[1 * 4 + 1] = (bar_rect.get_top() - bar_rect.get_bottom());
    } else {
        // The unit vertical segment of the line, turned to lie along the bar.
        const float y = bar_rect.get_bottom() + (neutral - m_min_speed) * bar_rect.get_height() / speed_span;
        model_matrix.matrix()(0, 0) = 0.0;
        model_matrix.matrix()(0, 1) = bar_rect.get_width();
        model_matrix.matrix()(0, 3) = (bar_rect.get_left() + bar_rect.get_right()) * 0.5f;
        model_matrix.matrix()(1, 0) = 0.0;
        model_matrix.matrix()(1, 1) = 0.0;
        model_matrix.matrix()(1, 3) = y;
    }

    const auto& projection_matrix = camera.get_projection_matrix();
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    shader->set_uniform("projection_matrix", projection_matrix);

    if (!m_baseline.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);
        init_data.add_vertex(Vec3f(0.0f, -0.5f, 0.0f));
        init_data.add_vertex(Vec3f(0.0f, 0.5f, 0.0f));
        init_data.add_line(0, 1);
        m_baseline.init_from(std::move(init_data));
    }
    m_baseline.set_color({ 0.0f, 0.0f, 0.0f, 1.0f });
    m_baseline.render_geometry();

    // The points of the curve are <speed, position along the axis>.
    Transform3d model_matrix_curve{ Transform3d::Identity() };
    if (! xy) {
        const float scale_x = bar_rect.get_width() / speed_span;
        const float scale_y = bar_rect.get_height() / length;
        model_matrix_curve.data()[3 * 4 + 0] = bar_rect.get_left() - m_min_speed * scale_x;
        model_matrix_curve.data()[3 * 4 + 1] = bar_rect.get_bottom();
        model_matrix_curve.data()[0 * 4 + 0] = scale_x;
        model_matrix_curve.data()[1 * 4 + 1] = scale_y;
    } else {
        const bool  flipped = m_axis_bar.flipped();
        const float along   = (flipped ? -1.0f : 1.0f) * bar_rect.get_width() / length;
        const float across  = bar_rect.get_height() / speed_span;
        model_matrix_curve.matrix()(0, 0) = 0.0;
        model_matrix_curve.matrix()(0, 1) = along;
        model_matrix_curve.matrix()(0, 3) = flipped ? bar_rect.get_right() : bar_rect.get_left();
        model_matrix_curve.matrix()(1, 0) = across;
        model_matrix_curve.matrix()(1, 1) = 0.0;
        model_matrix_curve.matrix()(1, 3) = bar_rect.get_bottom() - m_min_speed * across;
    }
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix_curve);

    if (!m_profile_curve.is_initialized() || m_profile_dirty) {
        // The painted profile. When nothing has been painted yet the curve is a straight line at the
        // speed in effect, so the tool always shows what a first click would start from.
        const std::vector<double> display_profile = display_curve();
        m_profile_curve.reset();
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(display_profile.size() / 2);
        init_data.reserve_indices(display_profile.size() / 2);

        for (unsigned int i = 0; i + 1 < display_profile.size(); i += 2) {
            init_data.add_vertex(Vec3f((float)display_profile[i + 1], (float)display_profile[i], 0.0f));
            init_data.add_index(i / 2);
        }

        m_profile_curve.init_from(std::move(init_data));
        m_profile_dirty = false;
        m_displayed_neutral = neutral;
    }
    m_profile_curve.set_color({ 0.0f, 0.0f, 1.0f, 1.0f });
    m_profile_curve.render_geometry();

    wxGetApp().unbind_shader();
}

void GLCanvas3D::SpeedEditing::render_volumes(const GLCanvas3D& canvas, const GLVolumeCollection& volumes)
{
    assert(this->is_allowed());
    assert(this->last_object_id != -1);

    const auto& current_shader = wxGetApp().get_current_shader();
    ScopeGuard guard([current_shader]() { if (current_shader != nullptr) wxGetApp().bind_shader(current_shader); });
    if (current_shader != nullptr)
        wxGetApp().unbind_shader();

    const auto& shader = wxGetApp().get_shader("variable_layer_height");
    if (shader == nullptr)
        return;

    wxGetApp().bind_shader(shader);

    generate_speed_texture();

    // Orio: along X / Y the shader is given the coordinate along the edited axis as the "z" of the vertices.
    const bool  xy     = m_axis_bar.is_xy();
    const float length = std::max(edit_length(), 1e-3f);
    shader->set_uniform("z_to_texture_row", float(m_speed_texture.cells - 1) / (float(m_speed_texture.width) * length));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / float(m_speed_texture.height));
    shader->set_uniform("z_cursor", this->cursor_coordinate(canvas));
    shader->set_uniform("z_cursor_band_width", float(this->band_width));

    const Camera& camera = m_canvas.get_active_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();
    shader->set_uniform("projection_matrix", projection_matrix);

    const GLsizei w = (GLsizei)m_speed_texture.width;
    const GLsizei h = (GLsizei)m_speed_texture.height;
    const GLsizei half_w = w / 2;
    const GLsizei half_h = h / 2;
    glsafe(::glActiveTexture(GL_TEXTURE0 + 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, half_w, half_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_speed_texture.data.data()));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, half_w, half_h, GL_RGBA, GL_UNSIGNED_BYTE, m_speed_texture.data.data() + m_speed_texture.width * m_speed_texture.height * 4));
    std::vector<std::array<float, 4>> colors = m_canvas.get_active_colors();
    for (GLVolume* glvolume : volumes.volumes) {
        if (!glvolume->is_active || glvolume->composite_id.object_id != this->last_object_id || glvolume->is_modifier)
            continue;

        if (xy)
            shader->set_uniform("volume_world_matrix", m_axis_bar.volume_axis_matrix(*glvolume));
        else
            shader->set_uniform("volume_world_matrix", glvolume->world_matrix());
        shader->set_uniform("object_max_z", 0.0f);

        const Transform3d view_model_matrix = view_matrix * glvolume->world_matrix();
        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("normal_matrix", (Matrix3d)view_model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose());

        glvolume->render(camera, colors, m_canvas.get_ref_model());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLCanvas3D::SpeedEditing::generate_speed_texture()
{
    if (m_speed_texture.data.empty()) {
        m_speed_texture.width = 1024;
        m_speed_texture.height = 1024;
        m_speed_texture.levels = 2;
        m_speed_texture.data.assign(m_speed_texture.width * m_speed_texture.height * 5, 0);
        m_speed_texture.valid = false;
    }

    if (m_speed_texture.valid && m_speed_texture.cells > 0)
        return;

    // When nothing has been painted yet, show the object flat at the speed in effect (Orio: along the edited
    // axis, the height of the object along Z).
    m_speed_texture.cells = Slic3r::generate_variable_speed_texture(
        (double)edit_length(), (double)m_min_speed, (double)m_max_speed, display_curve(),
        m_speed_texture.data.data(), (int)m_speed_texture.height, (int)m_speed_texture.width, true);
    m_speed_texture.valid = true;
    m_displayed_neutral = neutral_speed();
}

void GLCanvas3D::SpeedEditing::adjust_speed_profile()
{
    // Painting only makes sense once a type is active (a curve to edit).
    const float length = edit_length();
    if (m_model_object == nullptr || length <= 0.0f || m_active_target == 0)
        return;

    ensure_profile();
    const std::vector<double> before = m_speed_profile;
    // Orio: along the edited axis (0 at its start along X / Y). Shift + click and the smoothing converge to the
    // speed in effect where the curve is not painted.
    Slic3r::adjust_variable_speed_profile(m_speed_profile, (double)length, (double)neutral_speed(),
        (double)m_min_speed, (double)m_max_speed, (double)this->last_z, (double)this->strength,
        (double)this->band_width, this->last_action);
    // Orio: what the stroke changed stops at the bounds around it; the bound itself, the top of the last layer on
    // its lower side, goes with that side.
    if (const std::vector<double> bounds = active_bounds(); ! bounds.empty() && m_speed_profile != before) {
        double lo = 0., hi = (double)length;
        for (const double bound : bounds) {
            if (bound < (double)this->last_z)
                lo = bound;
            else {
                hi = bound;
                break;
            }
        }
        m_speed_profile = Slic3r::variable_speed_clip_stroke(before, m_speed_profile, lo, hi, (double)length);
    }
    m_profile_changed = true;
    sync_active_into_curves();
    m_speed_texture.valid = false;
    m_profile_dirty = true;
}

void GLCanvas3D::SpeedEditing::commit_to_model(GLCanvas3D& canvas, const std::string& snapshot_name)
{
    if (m_model_object == nullptr)
        return;
    // Fold the working active curve back in, then publish every painted type's curve (each applies).
    sync_active_into_curves();
    ModelObject* model_object = const_cast<ModelObject*>(m_model_object);
    VariableSpeedProfile new_profile;
    for (const auto& kv : m_curves)
        if (kv.second.size() >= 4)
            new_profile.curves[kv.first] = kv.second;
    for (const auto& kv : m_bounds)
        if (! kv.second.empty())
            new_profile.bounds[kv.first] = kv.second;
    if (model_object->speed_profile == new_profile)
        return;
    if (! snapshot_name.empty())
        wxGetApp().plater()->take_snapshot(snapshot_name);
    // Mirror what we are about to store, so select_object() can later tell an external change
    // (undo/redo, project reload) apart from a plain reslice, which never touches speed_profile.
    m_committed_curves = new_profile.curves;
    m_committed_bounds = new_profile.bounds;
    model_object->speed_profile = std::move(new_profile);
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLCanvas3D::SpeedEditing::accept_changes(GLCanvas3D& canvas)
{
    if (last_object_id >= 0 && m_active_target != 0)
        commit_to_model(canvas, "Variable speed - Manual edit");
}

void GLCanvas3D::SpeedEditing::reset_speed_profile(GLCanvas3D& canvas)
{
    // Orio: clear one type's override: the type whose revert icon was clicked (m_reset_bit), or
    // the active type by default. If it was the active type, the bar falls back to a flat preview, ready
    // to be painted again; the type's box state is left untouched.
    // Along the shown axes only: its Z curve, or its X and Y curves. The others still apply.
    const uint32_t target = (m_reset_bit != 0) ? m_reset_bit : m_active_target;
    m_reset_bit = 0;
    if (target != 0) {
        if (m_axis_bar.is_xy()) {
            m_curves.erase(paint_axis_key(target, paX));
            m_curves.erase(paint_axis_key(target, paY));
        } else
            m_curves.erase(paint_axis_key(target, paZ));
        if (target == m_active_target)
            load_active_curve();
    }
    m_speed_texture.valid = false;
    m_profile_dirty = true;
    commit_to_model(canvas, "Variable speed - Reset type");
}

void GLCanvas3D::SpeedEditing::smooth_speed_profile(GLCanvas3D& canvas, const HeightProfileSmoothingParams& smoothing_params)
{
    if (m_model_object == nullptr || m_active_target == 0 || m_speed_profile.empty())
        return;
    ensure_profile();
    // Orio: each part between the bounds on its own: the steps at the bounds stay.
    m_speed_profile = Slic3r::smooth_variable_speed_profile_bounded(m_speed_profile, active_bounds(), (double)m_min_speed, (double)m_max_speed, smoothing_params);
    m_profile_changed = true;
    sync_active_into_curves();
    m_speed_texture.valid = false;
    m_profile_dirty = true;
    commit_to_model(canvas, "Variable speed - Smooth type");
}

} // namespace GUI
} // namespace Slic3r
