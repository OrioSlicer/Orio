// Orio: "Variable infill and walls" tool, the interactive editor of InfillWallsProfile (libslic3r).
// GLCanvas3D::InfillWallsEditing is declared in GLCanvas3D.hpp. It is a mirror image of GLCanvas3D::SpeedEditing
// (itself a mirror image of GLCanvas3D::LayersEditing): same tool window, same vertical bar on the right hand
// side of the 3D scene, same mouse gestures. Only the painted quantities differ: the sparse infill density in
// percent or the number of walls, chosen with a drop-down in the tool window.
// A toggle of the window paints along the X / Y axes of the object instead of Z: the bar is then laid along the
// bottom of the scene (PaintAxisBar), along the axis of the object seen the most from the side.

#include "libslic3r/libslic3r.h"
#include "GLCanvas3D.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/InfillWallsProfile.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
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

float GLCanvas3D::InfillWallsEditing::s_overlay_window_width;

// Section of the application config remembering the fields of the tool window across sessions.
static const char *INFILL_WALLS_APP_CONFIG_SECTION = "infill_walls_editing";

// Value of a numeric option of the edited object: its own settings first, then the print profile. The profile is the
// config of the plater, which only holds the keys listed in Plater::priv::priv(): a key read here must be listed there,
// else only the fallback is read.
static double infill_walls_config_value(const ModelObject *model_object, const DynamicPrintConfig *config, const char *key, double fallback)
{
    const ConfigOption *opt = (model_object != nullptr) ? model_object->config.option(key) : nullptr;
    if (opt == nullptr && config != nullptr)
        opt = config->option(key);
    if (opt == nullptr)
        return fallback;
    switch (opt->type()) {
    case coInt:
    case coEnum:
        return (double)opt->getInt();
    case coFloat:
    case coPercent:
        return opt->getFloat();
    default:
        return fallback;
    }
}

// Curve of a quantity painted along an axis (paint_axis_key()) in a profile, empty when it was not painted.
static const std::vector<double>& infill_walls_profile_curve(const InfillWallsProfile& profile, uint32_t key)
{
    static const std::vector<double> none;
    const auto it = profile.curves.find(key);
    return it == profile.curves.end() ? none : it->second;
}

GLCanvas3D::InfillWallsEditing::~InfillWallsEditing()
{
    if (m_z_texture_id != 0) {
        glsafe(::glDeleteTextures(1, &m_z_texture_id));
        m_z_texture_id = 0;
    }

    m_baseline.reset();
    m_profile_curve.reset();
}

void GLCanvas3D::InfillWallsEditing::init()
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

    // Restore the fields of the tool window saved in a previous session.
    load_persisted_fields();
}

void GLCanvas3D::InfillWallsEditing::load_persisted_fields()
{
    const AppConfig *app_config = wxGetApp().app_config;
    if (app_config == nullptr || ! app_config->has_section(INFILL_WALLS_APP_CONFIG_SECTION))
        return;
    // The fields hold whole numbers, stored as integers so that no decimal separator is involved.
    auto get_int = [app_config](const char *key, float &out) {
        std::string v;
        if (app_config->get(INFILL_WALLS_APP_CONFIG_SECTION, key, v) && ! v.empty()) {
            try { out = (float)std::stoi(v); } catch (...) {}
        }
    };
    get_int("density_min", m_density_min);
    get_int("density_max", m_density_max);
    get_int("walls_min",   m_walls_min);
    get_int("walls_max",   m_walls_max);

    std::string v;
    if (app_config->get(INFILL_WALLS_APP_CONFIG_SECTION, "mode", v))
        m_mode = (v == "walls") ? (uint32_t)iwtWallLoops : (uint32_t)iwtInfillDensity;
    if (app_config->get(INFILL_WALLS_APP_CONFIG_SECTION, "axis", v)) {
        // "z", "x" or "y" ("xy": the first version of the X / Y painting, which chose the axis itself).
        m_axis_bar.set_xy(v == "x" || v == "y" || v == "xy");
        m_axis_bar.set_axis(v == "y" ? paY : paX);
    }
    if (app_config->get(INFILL_WALLS_APP_CONFIG_SECTION, "smooth_radius", v) && ! v.empty()) {
        try { m_smooth_params.radius = (unsigned int)std::clamp(std::stoi(v), 1, 10); } catch (...) {}
    }
    if (app_config->get(INFILL_WALLS_APP_CONFIG_SECTION, "smooth_keep_min", v) && ! v.empty())
        m_smooth_params.keep_min = (v == "true" || v == "1");
    if (app_config->get(INFILL_WALLS_APP_CONFIG_SECTION, "hide_other_objects", v) && ! v.empty())
        m_hide_other_objects = (v == "true" || v == "1");

    // Keep the scales consistent whatever was stored: whole numbers, at least one unit wide.
    auto sanitize = [](float &vmin, float &vmax, float lo, float hi) {
        vmin = std::clamp(std::round(vmin), lo, hi - 1.0f);
        vmax = std::clamp(std::round(vmax), vmin + 1.0f, hi);
    };
    sanitize(m_density_min, m_density_max, (float)MIN_VARIABLE_INFILL_DENSITY, (float)MAX_VARIABLE_INFILL_DENSITY);
    sanitize(m_walls_min,   m_walls_max,   (float)MIN_VARIABLE_WALL_LOOPS,     (float)MAX_VARIABLE_WALL_LOOPS);
}

void GLCanvas3D::InfillWallsEditing::save_persisted_fields() const
{
    AppConfig *app_config = wxGetApp().app_config;
    if (app_config == nullptr)
        return;
    app_config->set(INFILL_WALLS_APP_CONFIG_SECTION, "mode",            std::string(is_walls_mode() ? "walls" : "infill"));
    app_config->set(INFILL_WALLS_APP_CONFIG_SECTION, "axis",            std::string(! m_axis_bar.is_xy() ? "z" : m_axis_bar.axis() == paY ? "y" : "x"));
    app_config->set(INFILL_WALLS_APP_CONFIG_SECTION, "density_min",     std::to_string((int)std::lround(m_density_min)));
    app_config->set(INFILL_WALLS_APP_CONFIG_SECTION, "density_max",     std::to_string((int)std::lround(m_density_max)));
    app_config->set(INFILL_WALLS_APP_CONFIG_SECTION, "walls_min",       std::to_string((int)std::lround(m_walls_min)));
    app_config->set(INFILL_WALLS_APP_CONFIG_SECTION, "walls_max",       std::to_string((int)std::lround(m_walls_max)));
    app_config->set(INFILL_WALLS_APP_CONFIG_SECTION, "smooth_radius",   std::to_string(m_smooth_params.radius));
    app_config->set(INFILL_WALLS_APP_CONFIG_SECTION, "smooth_keep_min", m_smooth_params.keep_min);
    app_config->set(INFILL_WALLS_APP_CONFIG_SECTION, "hide_other_objects", m_hide_other_objects);
}

void GLCanvas3D::InfillWallsEditing::set_config(const DynamicPrintConfig* config)
{
    m_config = config;
    // The base values come from the print profile. Do NOT clear the painted curves here: they are per object
    // data and set_config() runs on every configuration change, including at slicing time.
    m_texture.valid = false;
    m_profile_dirty = true;
}

void GLCanvas3D::InfillWallsEditing::select_object(const Model& model, int object_id)
{
    const ModelObject* model_object_new = (object_id >= 0 && object_id < (int)model.objects.size()) ? model.objects[object_id] : nullptr;

    // Detaching (switching to the Preview tab, the tool being turned off, or an Undo / Redo jump about to delete
    // the model objects). The curves were committed to the model, they are reloaded from it when attaching again.
    if (model_object_new == nullptr) {
        if (m_model_object != nullptr || this->last_object_id != -1) {
            this->last_object_id = -1;
            m_model_object = nullptr;
            m_object_max_z = 0.0f;
            m_texture.valid = false;
            m_profile_dirty = true;
        }
        return;
    }

    const float new_max_z = static_cast<float>(model_object_new->bounding_box().max.z());
    const bool  attached  = m_model_object != model_object_new || this->last_object_id != object_id;
    this->last_object_id = object_id;
    m_model_object = model_object_new;
    // Extents of the object along its own X and Y axes, spanned by the curves painted along them.
    const std::pair<double, double> old_extent_x = m_axis_bar.extent(paX);
    const std::pair<double, double> old_extent_y = m_axis_bar.extent(paY);
    m_axis_bar.set_object(model_object_new);
    const bool extents_changed = m_axis_bar.extent(paX) != old_extent_x || m_axis_bar.extent(paY) != old_extent_y;

    // Every edit is committed to the model, so the model holds the reference curves: reload them when attaching
    // to an object, when its size changed, or when its curves changed behind our back (undo / redo, project
    // reload). A slice or a configuration change never reloads, and the model does not change while a stroke
    // is being painted, so the stroke in progress is kept.
    const InfillWallsProfile& model_profile = model_object_new->infill_walls_profile;
    if (! attached && ! extents_changed && m_curves_oid == model_object_new->id() && m_object_max_z == new_max_z && m_seen_profile == model_profile)
        return;

    // Undo / redo of an edit of a curve that is not displayed (each edit changes a single curve): display it, so
    // that Ctrl+Z / Ctrl+Y always show on the bar.
    if (m_curves_oid == model_object_new->id() && m_seen_profile != model_profile &&
        infill_walls_profile_curve(m_seen_profile, edited_key()) == infill_walls_profile_curve(model_profile, edited_key())) {
        std::set<uint32_t> keys;
        for (const auto& kv : m_seen_profile.curves)
            keys.insert(kv.first);
        for (const auto& kv : model_profile.curves)
            keys.insert(kv.first);
        for (uint32_t key : keys) {
            const uint32_t  target = paint_axis_key_target(key);
            const PaintAxis axis   = paint_axis_key_axis(key);
            if (key == edited_key() || (target != (uint32_t)iwtInfillDensity && target != (uint32_t)iwtWallLoops) || ! paint_axis_is_valid(axis) ||
                infill_walls_profile_curve(m_seen_profile, key) == infill_walls_profile_curve(model_profile, key))
                continue;
            m_mode = target;
            m_axis_bar.set_xy(axis != paZ);
            m_axis_bar.set_axis(axis);
            break;
        }
    }

    m_object_max_z = new_max_z;
    m_curves_oid   = model_object_new->id();
    m_seen_profile = model_profile;
    m_curves.clear();
    m_bounds = m_seen_profile.bounds;
    for (const auto& kv : m_seen_profile.curves) {
        std::vector<double> curve = kv.second;
        if (curve.size() < 4 || (curve.size() & 1) != 0)
            continue;
        const PaintAxis axis   = paint_axis_key_axis(kv.first);
        // The bounds of the curve are stretched with it.
        const auto      bounds = m_bounds.find(kv.first);
        if (axis == paZ) {
            // Stretch a curve painted for another object height (the object was scaled since it was painted)
            // instead of throwing the user's work away. The slicer stretches it the same way.
            const double old_height = curve[curve.size() - 2];
            if (old_height > EPSILON && std::abs(old_height - (double)m_object_max_z) > 1e-3) {
                const double k = (double)m_object_max_z / old_height;
                for (size_t i = 0; i < curve.size(); i += 2)
                    curve[i] *= k;
                curve[curve.size() - 2] = (double)m_object_max_z;
                if (bounds != m_bounds.end())
                    for (double& bound : bounds->second)
                        bound *= k;
            }
        } else if (axis == paX || axis == paY) {
            // A curve painted along X / Y spans the extent of the object along that axis when it was painted: put it
            // exactly over the current extent (stretched if a part was added or a volume scaled since).
            const std::pair<double, double> extent = m_axis_bar.extent(axis);
            const double                    from   = curve.front();
            const double                    to     = curve[curve.size() - 2];
            if (extent.second - extent.first > EPSILON && to - from > EPSILON && (from != extent.first || to != extent.second)) {
                const double k = (extent.second - extent.first) / (to - from);
                for (size_t i = 0; i < curve.size(); i += 2)
                    curve[i] = extent.first + (curve[i] - from) * k;
                curve.front()           = extent.first;
                curve[curve.size() - 2] = extent.second;
                if (bounds != m_bounds.end())
                    for (double& bound : bounds->second)
                        bound = extent.first + (bound - from) * k;
            }
        }
        // The curves of a later version (another axis) are kept as they are: committing must not drop them.
        m_curves[kv.first] = std::move(curve);
    }
    load_mode_curve();
}

bool GLCanvas3D::InfillWallsEditing::is_allowed() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr && m_z_texture_id > 0;
}

bool GLCanvas3D::InfillWallsEditing::is_enabled() const
{
    return m_enabled;
}

void GLCanvas3D::InfillWallsEditing::set_enabled(bool enabled)
{
    m_enabled = is_allowed() && enabled;
    if (! m_enabled)
        m_axis_bar.release_parts();
}

bool GLCanvas3D::InfillWallsEditing::is_initialized() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr;
}

float GLCanvas3D::InfillWallsEditing::base_value() const
{
    if (is_walls_mode())
        return (float)infill_walls_config_value(m_model_object, m_config, "wall_loops", 2.0);
    return (float)infill_walls_config_value(m_model_object, m_config, "sparse_infill_density", 15.0);
}

float GLCanvas3D::InfillWallsEditing::walls_width() const
{
    const double loops = std::round(infill_walls_config_value(m_model_object, m_config, "wall_loops", 2.0));
    if (loops < 1.)
        return 0.0f;
    // A line width of 0 is the default one, itself automatic from the nozzle when 0.
    double nozzle = 0.4;
    if (m_config != nullptr)
        if (const ConfigOptionFloats *opt = m_config->option<ConfigOptionFloats>("nozzle_diameter"); opt != nullptr && ! opt->values.empty())
            nozzle = opt->values.front();
    const double default_width = infill_walls_config_value(m_model_object, m_config, "line_width", 0.);
    auto width = [&](const char *key) {
        const double w = infill_walls_config_value(m_model_object, m_config, key, 0.);
        return w > 0. ? w : default_width > 0. ? default_width : 1.125 * nozzle;
    };
    return (float)(width("outer_wall_line_width") + (loops - 1.) * width("inner_wall_line_width"));
}

std::pair<float, float> GLCanvas3D::InfillWallsEditing::shells_thickness() const
{
    const double layer_height = infill_walls_config_value(m_model_object, m_config, "layer_height", 0.2);
    // The first layer belongs to the bottom shell. A shell is as thick as its layers, and at least as its thickness
    // setting (more layers are then added), unless it has no layer.
    auto shell = [&](const char *layers_key, const char *thickness_key, double first_layer) {
        const double layers = std::round(infill_walls_config_value(m_model_object, m_config, layers_key, 0.));
        if (layers < 1.)
            return 0.0f;
        return (float)std::max(first_layer + (layers - 1.) * layer_height, infill_walls_config_value(m_model_object, m_config, thickness_key, 0.));
    };
    return { shell("bottom_shell_layers", "bottom_shell_thickness", infill_walls_config_value(m_model_object, m_config, "initial_layer_print_height", layer_height)),
             shell("top_shell_layers", "top_shell_thickness", layer_height) };
}

float GLCanvas3D::InfillWallsEditing::neutral_value() const
{
    const uint32_t edited  = edited_key();
    bool           painted = false;
    double         lowest  = 0.;
    for (PaintAxis axis : { paZ, paX, paY }) {
        const uint32_t key = paint_axis_key(m_mode, axis);
        const auto     it  = m_curves.find(key);
        if (key == edited || it == m_curves.end() || it->second.size() < 4)
            continue;
        double curve_min = std::numeric_limits<double>::max();
        for (size_t i = 1; i < it->second.size(); i += 2)
            curve_min = std::min(curve_min, it->second[i]);
        lowest  = painted ? std::max(lowest, curve_min) : curve_min;
        painted = true;
    }
    const float value = painted ? (float)lowest : base_value();
    return std::clamp(value, (float)infill_walls_min_value(mode(), edited_axis()), (float)infill_walls_max_value(mode(), edited_axis()));
}

float GLCanvas3D::InfillWallsEditing::min_value() const
{
    const float lo = (float)infill_walls_min_value(mode(), edited_axis());
    const float hi = (float)infill_walls_max_value(mode(), edited_axis());
    return std::clamp(is_walls_mode() ? m_walls_min : m_density_min, lo, hi - 1.0f);
}

float GLCanvas3D::InfillWallsEditing::max_value() const
{
    const float hi = (float)infill_walls_max_value(mode(), edited_axis());
    return std::clamp(is_walls_mode() ? m_walls_max : m_density_max, min_value() + 1.0f, hi);
}

float GLCanvas3D::InfillWallsEditing::edit_length() const
{
    return m_axis_bar.is_xy() ? (float)m_axis_bar.length() : m_object_max_z;
}

double GLCanvas3D::InfillWallsEditing::edit_origin() const
{
    return m_axis_bar.is_xy() ? m_axis_bar.origin() : 0.;
}

float GLCanvas3D::InfillWallsEditing::strength() const
{
    const float range = max_value() - min_value();
    // The wall count is rounded when slicing: a couple of clicks (or ~0.2 s with the button held) add or
    // remove one wall, a drag along the bar stays controllable.
    if (is_walls_mode())
        return std::clamp(0.03f * range, 0.1f, 0.5f);
    // The infill density moves by ~2 % of the scale per step, like the variable speed tool.
    return std::max(0.1f, 0.02f * range);
}

double GLCanvas3D::InfillWallsEditing::display_value(double value) const
{
    return is_walls_mode() ? std::round(value) : value;
}

std::vector<double> GLCanvas3D::InfillWallsEditing::display_curve() const
{
    if (m_profile.size() >= 4)
        return m_profile;
    // Not painted yet: flat at the value in effect where it is not painted.
    const double neutral = std::clamp((double)neutral_value(), (double)min_value(), (double)max_value());
    return Slic3r::infill_walls_curve_flat((double)edit_length(), neutral);
}

void GLCanvas3D::InfillWallsEditing::ensure_profile()
{
    const float length = edit_length();
    if (length <= 0.0f)
        return;
    if (m_profile.size() < 4 || (m_profile.size() & 1) != 0 ||
        std::abs(m_profile.front()) > EPSILON ||
        std::abs(m_profile[m_profile.size() - 2] - (double)length) > 1e-3) {
        m_profile = Slic3r::infill_walls_curve_flat((double)length,
            std::clamp((double)neutral_value(), (double)min_value(), (double)max_value()));
        m_profile_changed = true;
    }
}

void GLCanvas3D::InfillWallsEditing::sync_mode_into_curves()
{
    // Only a painted curve is stored, so merely choosing a mode never makes it apply. Only a changed one is
    // written back: along X / Y the coordinates are converted back into the frame of the object.
    if (! m_profile_changed || m_profile.size() < 4)
        return;
    std::vector<double> curve  = m_profile;
    const double        origin = edit_origin();
    if (origin != 0.)
        for (size_t i = 0; i < curve.size(); i += 2)
            curve[i] += origin;
    m_curves[edited_key()] = std::move(curve);
    m_profile_changed = false;
}

void GLCanvas3D::InfillWallsEditing::load_mode_curve()
{
    auto it = m_curves.find(edited_key());
    m_profile = (it != m_curves.end()) ? it->second : std::vector<double>();
    // Along X / Y the bar starts at the origin of the axis.
    const double origin = edit_origin();
    if (origin != 0.)
        for (size_t i = 0; i < m_profile.size(); i += 2)
            m_profile[i] -= origin;
    m_profile_changed = false;
    m_texture.valid = false;
    m_profile_dirty = true;
}

void GLCanvas3D::InfillWallsEditing::set_mode(uint32_t mode)
{
    if (mode == m_mode)
        return;
    // Keep whatever was painted on the previous quantity before switching.
    sync_mode_into_curves();
    m_mode = mode;
    load_mode_curve();
    save_persisted_fields();
}

void GLCanvas3D::InfillWallsEditing::set_xy(bool xy)
{
    if (xy == m_axis_bar.is_xy())
        return;
    // Keep whatever was painted along the previous axis before switching.
    sync_mode_into_curves();
    m_axis_bar.set_xy(xy);
    load_mode_curve();
    save_persisted_fields();
}

void GLCanvas3D::InfillWallsEditing::set_xy_axis(PaintAxis axis)
{
    if ((axis != paX && axis != paY) || axis == m_axis_bar.axis())
        return;
    // Keep whatever was painted along the previous axis before switching.
    sync_mode_into_curves();
    m_axis_bar.set_axis(axis);
    load_mode_curve();
    save_persisted_fields();
}

void GLCanvas3D::InfillWallsEditing::update_axis_bar(const GLCanvas3D& canvas)
{
    // The edited axis is the user's choice; only how it appears on the screen follows the view and the object.
    if (m_axis_bar.is_xy() && m_model_object != nullptr)
        m_axis_bar.update_orientation(canvas, *m_model_object, canvas.get_selection().get_instance_idx());
    m_axis_bar.set_height(m_object_max_z);
    // The parts of the object without infill, which follow its settings: along X / Y, the walls at both ends, not
    // painted (for the infill density only: the walls are painted up to the ends); along Z, the bottom and the top
    // shells, shown outside the bar.
    if (m_model_object == nullptr)
        m_axis_bar.set_margins(0., 0.);
    else if (m_axis_bar.is_xy()) {
        const float walls = is_walls_mode() ? 0.0f : walls_width();
        m_axis_bar.set_margins(walls, walls);
    } else {
        const std::pair<float, float> shells = shells_thickness();
        m_axis_bar.set_margins(shells.first, shells.second);
    }
}

void GLCanvas3D::InfillWallsEditing::show_tooltip_information(const GLCanvas3D& canvas, std::map<wxString, wxString> captions_texts, float x, float y)
{
    //Orio: shown by the keyboard icon right of the name of the tool, in the title bar of its window.
    wxGetApp().imgui()->tool_window_shortcuts(std::vector<std::pair<wxString, wxString>>(captions_texts.begin(), captions_texts.end()));
}

void GLCanvas3D::InfillWallsEditing::render_dialog(const GLCanvas3D& canvas)
{
    if (!m_enabled)
        return;

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const Size& cnv_size = canvas.get_canvas_size();
    float         zoom = (float)canvas.get_active_camera().get_zoom();
    const auto& p_main_toolbar = canvas.get_main_toolbar();
    float left_pos = 0.0f;
    float main_toolbar_height = 0.0f;
    if (p_main_toolbar && p_main_toolbar->get_item("infillwallsediting")) {
        left_pos = p_main_toolbar->get_item("infillwallsediting")->render_rect[0];
        main_toolbar_height = canvas.main_toolbar_window_top(p_main_toolbar->get_item("infillwallsediting")->render_rect[3]); //Orio
    }
    float x = 0.5 * cnv_size.get_width() + left_pos * zoom;

    const auto canvas_width = cnv_size.get_width();
    if (std::abs(GLCanvas3D::InfillWallsEditing::s_overlay_window_width) > 0.01f) {
        if (x + GLCanvas3D::InfillWallsEditing::s_overlay_window_width > canvas_width) {
            if (GLCanvas3D::InfillWallsEditing::s_overlay_window_width > canvas_width)
                x = 0;
            else
                x = canvas_width - GLCanvas3D::InfillWallsEditing::s_overlay_window_width;
        }
    }
    x = std::max(x, 0.0f);

    imgui.set_next_window_pos(x, main_toolbar_height, ImGuiCond_Always, 0.0f, 0.0f);

    imgui.push_toolbar_style(canvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * canvas.get_scale(), 4.0f * canvas.get_scale()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * canvas.get_scale(), 10.0f * canvas.get_scale()));
    // Orio: floating, collapsible window (ImGuiWrapper::begin_tool_window), its place remembered under the name of
    // the icon of the tool; default place under the icon, as above.
    imgui.begin_tool_window("toolbar_variable_infill_walls", _L("Variable infill and walls"), wxGetApp().dark_mode() ? "toolbar_variable_infill_walls_dark.svg" : "toolbar_variable_infill_walls.svg",
                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    const float sliders_width = imgui.scaled(7.0f);
    const float input_box_width = 3.0f * imgui.get_slider_icon_size().x;

    static float text_align = ImGui::GetCursorPosX();
    static float input_align = ImGui::GetCursorPosX();

    auto align_label = [&](const wxString& label) {
        ImGui::AlignTextToFramePadding();
        text_align = std::max(text_align, ImGui::GetCursorPosX());
        ImGui::SetCursorPosX(text_align);
        imgui.text(label);
        ImGui::SameLine();
        input_align = std::max(input_align, ImGui::GetCursorPosX());
        ImGui::SetCursorPosX(input_align);
    };
    // Is a curve of this quantity painted along this axis?
    auto painted = [this](uint32_t mode, PaintAxis axis) {
        const auto it = m_curves.find(paint_axis_key(mode, axis));
        return it != m_curves.end() && it->second.size() >= 4;
    };

    // 1) The painted quantity. Each one keeps its own curves; a painted one that is not being edited is marked
    // "(edited)" so that it is clear it still applies. The revert arrow next to the drop-down resets the edited curve.
    const uint32_t modes[] = { (uint32_t)iwtInfillDensity, (uint32_t)iwtWallLoops };
    auto mode_label = [this, &painted](uint32_t mode) -> std::string {
        std::string label = (mode == (uint32_t)iwtWallLoops) ? _u8L("Walls") : _u8L("Infill");
        if (mode != m_mode && (painted(mode, paZ) || painted(mode, paX) || painted(mode, paY)))
            label += "  " + _u8L("(edited)");
        return label;
    };
    align_label(_L("Mode"));
    uint32_t new_mode = m_mode;
    // Wide enough for the longest entry, "(edited)" mark included.
    float combo_width = 2.0f * input_box_width;
    for (uint32_t mode : modes) {
        const std::string widest = ((mode == (uint32_t)iwtWallLoops) ? _u8L("Walls") : _u8L("Infill")) + "  " + _u8L("(edited)");
        combo_width = std::max(combo_width, ImGui::CalcTextSize(widest.c_str()).x + ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().FramePadding.x);
    }
    ImGuiWrapper::push_combo_style(canvas.get_scale());
    ImGui::PushItemWidth(combo_width);
    const std::string preview = mode_label(m_mode);
    if (ImGui::BBLBeginCombo("##infill_walls_mode", preview.c_str(), 0)) {
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++ i) {
            ImGui::PushID(int(i));
            if (ImGui::Selectable("", modes[i] == m_mode))
                new_mode = modes[i];
            ImGui::SameLine();
            ImGui::Text("%s", mode_label(modes[i]).c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    ImGuiWrapper::pop_combo_style();
    if (new_mode != m_mode)
        set_mode(new_mode);

    // The revert arrow shown next to a modified setting (same icon as in the object manipulation panel): it appears
    // once the edited curve has been painted and removes it, the other curves stay. Its slot stays reserved while
    // it is hidden, so that the window keeps its width.
    ImGui::SameLine();
    const float  reset_icon_size = ImGui::GetFontSize();
    const ImVec2 reset_button_size(reset_icon_size + 2.0f * ImGui::GetStyle().FramePadding.x, reset_icon_size + 2.0f * ImGui::GetStyle().FramePadding.y);
    if (m_profile.size() >= 4) {
        ImTextureID normal_id = canvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET);
        ImTextureID hover_id  = canvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET_HOVER);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        if (ImGui::ImageButton3(normal_id, hover_id, ImVec2(reset_icon_size, reset_icon_size)))
            wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), SimpleEvent(EVT_GLCANVAS_RESET_INFILL_WALLS_PROFILE));
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) {
            const wxString reset_tooltip = _L("Reset mode");
            imgui.tooltip(reset_tooltip, imgui.calc_text_size(reset_tooltip).x + 2.0f * ImGui::GetStyle().WindowPadding.x);
        }
    } else
        ImGui::Dummy(reset_button_size);

    // 2) Along Z (the bar on the right) or along the X / Y axes of the object (the bar at the bottom of the scene).
    // The other choice is marked "(edited)" when the quantity is painted there: it applies too.
    align_label(_L("Axis"));
    bool xy = m_axis_bar.is_xy();
    if (PaintAxisBar::render_axis_toggle(xy, painted(m_mode, paZ), painted(m_mode, paX) || painted(m_mode, paY)))
        set_xy(xy);
    if (m_axis_bar.is_xy())
        imgui.text_wrapped(_L("Paints along the X or Y axis of the object, chosen with the X / Y buttons over the bar. "
                              "Where several axes are painted, the highest value applies."), imgui.scaled(16.0f));

    // 3) Scale of the bar: the lowest and highest value one can paint.
    auto labelled_float_input = [&](const wxString& label, const char* id, float& value, float step, float v_min, float v_max) -> bool {
        align_label(label);
        ImGui::PushItemWidth(input_box_width);
        const bool changed = ImGui::BBLDragFloat(id, &value, step, v_min, v_max, "%.0f");
        ImGui::PopItemWidth();
        if (changed)
            value = std::clamp(std::round(value), v_min, v_max);
        return changed;
    };

    const float value_lo = (float)infill_walls_min_value(mode());
    const float value_hi = (float)infill_walls_max_value(mode());
    float& scale_min = is_walls_mode() ? m_walls_min : m_density_min;
    float& scale_max = is_walls_mode() ? m_walls_max : m_density_max;
    bool range_changed = false;
    if (is_walls_mode()) {
        range_changed |= labelled_float_input(_L("Min walls"), "##infill_walls_walls_min", scale_min, 0.05f, value_lo, value_hi - 1.0f);
        range_changed |= labelled_float_input(_L("Max walls"), "##infill_walls_walls_max", scale_max, 0.05f, value_lo + 1.0f, value_hi);
    } else {
        range_changed |= labelled_float_input(_L("Min density (%)"), "##infill_walls_density_min", scale_min, 0.2f, value_lo, value_hi - 1.0f);
        range_changed |= labelled_float_input(_L("Max density (%)"), "##infill_walls_density_max", scale_max, 0.2f, value_lo + 1.0f, value_hi);
    }
    if (range_changed) {
        // Keep the scale at least one unit wide, the edited bound wins.
        if (scale_max < scale_min + 1.0f) {
            if (scale_min + 1.0f <= value_hi)
                scale_max = scale_min + 1.0f;
            else
                scale_min = scale_max - 1.0f;
        }
        m_texture.valid = false;
        m_profile_dirty = true;
        save_persisted_fields();
    }

    // The value the object is sliced with where nothing was painted (object settings, else print profile).
    {
        const double base = (double)base_value();
        const std::string base_text = is_walls_mode() ? std::to_string((int)std::lround(base)) : GUI::format("%1% %%", (int)std::lround(base));
        ImGui::AlignTextToFramePadding();
        imgui.text(format_wxstr(_L("Base value: %1%"), base_text));
    }

    // These infill patterns are generated once per object from the density of the print profile: a painted
    // density has no effect on them.
    if (! is_walls_mode()) {
        const int pattern = (int)std::lround(infill_walls_config_value(m_model_object, m_config, "sparse_infill_pattern", -1.0));
        if (pattern == (int)ipAdaptiveCubic || pattern == (int)ipSupportCubic || pattern == (int)ipLightning)
            imgui.warning_text_wrapped(_L("This infill pattern ignores the painted density."), imgui.scaled(14.0f));
    }

    ImGui::Separator();

    // 4) Smoothing of the whole edited curve.
    bool fields_changed = false;
    if (imgui.button(_CTX(L_CONTEXT("Smooth", "Verb"), "Verb")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), HeightProfileSmoothEvent(EVT_GLCANVAS_SMOOTH_INFILL_WALLS_PROFILE, m_smooth_params));
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
    if (ImGui::BBLSliderScalar("##infill_walls_radius_slider", ImGuiDataType_S32, &radius, &v_min, &v_max)) {
        radius = std::clamp(radius, 1, 10);
        m_smooth_params.radius = (unsigned int)radius;
        fields_changed = true;
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::PopItemWidth();

    fields_changed |= imgui.bbl_checkbox("##infill_walls_keep_min", m_smooth_params.keep_min);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Keep min"));

    // The other objects hidden while this one is edited (hides()).
    fields_changed |= imgui.bbl_checkbox("##infill_walls_hide_other_objects", m_hide_other_objects);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Hide the other objects"));

    if (fields_changed)
        save_persisted_fields();

    ImGui::Separator();

    // Orio: under the window where it is (it can be moved).
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + ImGui::GetWindowPos().y;
    std::map<wxString, wxString> captions_texts = {
        {_L("Left mouse button:") ,_L("Decrease")},
        {_L("Right mouse button:"), _L("Increase")},
        {_L("Shift + Left mouse button:"),_L("Reset to base value")},
        {_L("Shift + Right mouse button:"), _L("Smoothing")},
        {_L("Mouse wheel:"), _L("Increase/decrease edit area")},
        // Anywhere in the scene, over the part too.
        {_L("Ctrl + Mouse wheel:"), _L("Increase/decrease edit area")},
        {_L("Parts and modifiers:"), _L("Keep their own value")},
        // Also on the part, at the height (or the position along X / Y) of the point clicked.
        {_L("On the part:"), _L("Same buttons as on the bar")},
        // The bounds the painting does not cross.
        {_L("Right click on the graduations:"), _L("Place or delete a bound")}
    };
    show_tooltip_information(canvas, captions_texts, ImGui::GetWindowPos().x, get_cur_y);

    GLCanvas3D::InfillWallsEditing::s_overlay_window_width = ImGui::GetWindowSize().x;
    imgui.end_tool_window();
    ImGui::PopStyleVar(2);
    imgui.pop_toolbar_style();
}

void GLCanvas3D::InfillWallsEditing::render_overlay(const GLCanvas3D& canvas)
{
    // Along X / Y, how the edited axis appears on the screen follows the view and the object; what the bar cannot paint
    // follows the object.
    update_axis_bar(canvas);
    // An unpainted curve is shown flat at the value in effect, which follows the object settings and the other
    // painted curves.
    if (m_displayed_base != neutral_value()) {
        m_texture.valid = false;
        m_profile_dirty = true;
    }
    render_dialog(canvas);
    if (edit_length() <= 0.0f)
        return;
    // The bar sits in a framed panel, at the bottom of the scene along X / Y, at its right along Z, whose background goes
    // under it.
    if (m_enabled)
        m_axis_bar.render_panel_background(canvas);
    render_background_texture(canvas);
    const Rect& bar_rect = get_bar_rect_viewport(canvas);
    render_curve(bar_rect);
    if (m_enabled) {
        // The rest of the panel, with the X / Y switch along X / Y. It tells whether the other axis is painted too: its
        // painting applies, though the object is colored by the edited axis only.
        auto painted = [this](PaintAxis axis) {
            const auto it = m_curves.find(paint_axis_key(m_mode, axis));
            return it != m_curves.end() && it->second.size() >= 4;
        };
        const std::string title = _u8L("Infill and walls") + " \xC2\xB7 " + (m_axis_bar.is_xy() ? _u8L("axis") : std::string("Z"));
        const PaintAxis   clicked = m_axis_bar.render_panel(canvas, title, painted(paX), painted(paY));
        // The value under the mouse goes beside the panel, where it hides nothing (see get_tooltip()); over the part, at
        // the place of the point along the bar.
        m_axis_bar.render_value_tip(canvas, cursor_value_text(canvas), cursor_coordinate(canvas) / edit_length());
        if (clicked != paZ)
            set_xy_axis(clicked);
        // The bounds of the edited curve, over the bar and its graduations: one undo step for each change.
        if (m_model_object != nullptr) {
            std::vector<double>            bounds = active_bounds();
            const PaintAxisBar::BoundsEdit edit   = m_axis_bar.render_bounds(canvas, *m_model_object, m_config, bounds);
            if (edit != PaintAxisBar::BoundsEdit::None)
                set_active_bounds(bounds);
            switch (edit) {
            case PaintAxisBar::BoundsEdit::Moved:      commit_to_model(m_canvas, "Variable infill and walls - Move bound"); break;
            case PaintAxisBar::BoundsEdit::Added:      commit_to_model(m_canvas, "Variable infill and walls - Add bound"); break;
            case PaintAxisBar::BoundsEdit::Deleted:    commit_to_model(m_canvas, "Variable infill and walls - Delete bound"); break;
            case PaintAxisBar::BoundsEdit::DeletedAll: commit_to_model(m_canvas, "Variable infill and walls - Delete bounds"); break;
            default: break;
            }
        }
    }
}

std::vector<double> GLCanvas3D::InfillWallsEditing::active_bounds() const
{
    std::vector<double> ret;
    const auto it = m_bounds.find(edited_key());
    if (it == m_bounds.end())
        return ret;
    const double origin = edit_origin();
    const double length = edit_length();
    for (const double bound : it->second)
        if (bound - origin > PaintAxisBar::BOUND_MIN_GAP && bound - origin < length - PaintAxisBar::BOUND_MIN_GAP)
            ret.push_back(bound - origin);
    std::sort(ret.begin(), ret.end());
    return ret;
}

void GLCanvas3D::InfillWallsEditing::set_active_bounds(std::vector<double> bounds)
{
    std::sort(bounds.begin(), bounds.end());
    const double origin = edit_origin();
    for (double& bound : bounds)
        bound += origin;
    if (bounds.empty())
        m_bounds.erase(edited_key());
    else
        m_bounds[edited_key()] = std::move(bounds);
}

float GLCanvas3D::InfillWallsEditing::cursor_coordinate(const GLCanvas3D& canvas) const
{
    const float relative = m_axis_bar.cursor_relative(canvas);
    if (relative != -1000.0f)
        return edit_length() * relative;
    // Over the part, where a click would paint.
    const Vec2d mouse   = canvas.get_local_mouse_position();
    const float on_part = part_coordinate_at(canvas, (float)mouse.x(), (float)mouse.y());
    return std::isnan(on_part) ? -1000.0f : on_part;
}

bool GLCanvas3D::InfillWallsEditing::bar_rect_contains(const GLCanvas3D& canvas, float x, float y) const
{
    // The bar, or beside its ends in its panel: a stroke may start just past them.
    return m_axis_bar.contains(canvas, x, y);
}

Rect GLCanvas3D::InfillWallsEditing::get_bar_rect_screen(const GLCanvas3D& canvas) const
{
    return m_axis_bar.rect_screen(canvas);
}

Rect GLCanvas3D::InfillWallsEditing::get_bar_rect_viewport(const GLCanvas3D& canvas) const
{
    return m_axis_bar.rect_viewport(canvas);
}

float GLCanvas3D::InfillWallsEditing::edit_coordinate_at(const GLCanvas3D& canvas, float x, float y) const
{
    // A stroke started on the part follows the part, and pauses while the mouse is off it (NaN).
    if (this->part_stroke)
        return part_coordinate_at(canvas, x, y);
    // Brought into the part of the axis that can be painted: a stroke from the walls or the shells at the ends of the
    // bar, or beyond them, paints next to them.
    return edit_length() * m_axis_bar.paintable(m_axis_bar.relative_along(canvas, x, y));
}

float GLCanvas3D::InfillWallsEditing::part_coordinate_at(const GLCanvas3D& canvas, float x, float y) const
{
    return (m_model_object != nullptr) ? m_axis_bar.part_coordinate_at(canvas, *m_model_object, x, y) : std::numeric_limits<float>::quiet_NaN();
}

float GLCanvas3D::InfillWallsEditing::bottom_bar_height(const GLCanvas3D& canvas) const
{
    return (m_enabled && m_axis_bar.is_xy() && this->last_object_id >= 0) ? m_axis_bar.reserved_height(canvas) : 0.0f;
}

float GLCanvas3D::InfillWallsEditing::right_bar_width(const GLCanvas3D& canvas) const
{
    return (m_enabled && ! m_axis_bar.is_xy() && this->last_object_id >= 0) ? m_axis_bar.reserved_width(canvas) : 0.0f;
}

bool GLCanvas3D::InfillWallsEditing::hides(const GLVolume& volume) const
{
    return m_enabled && m_hide_other_objects && this->last_object_id >= 0 && volume.composite_id.object_id != this->last_object_id;
}

std::string GLCanvas3D::InfillWallsEditing::get_tooltip(const GLCanvas3D&) const
{
    // A tooltip next to the mouse would hide the bar: the panel shows the value beside itself instead
    // (render_overlay()).
    return std::string();
}

std::string GLCanvas3D::InfillWallsEditing::cursor_value_text(const GLCanvas3D& canvas) const
{
    std::string ret;
    const float length = edit_length();
    if (m_enabled && length > 0.0f) {
        const float z = cursor_coordinate(canvas);
        if (z != -1000.0f) {
            // Where the edited curve was not painted, the value in effect there.
            const double v = display_value(Slic3r::infill_walls_curve_value_at(m_profile, (double)z, (double)neutral_value()));
            if (is_walls_mode())
                ret = GUI::format(_u8L("Walls: %1%"), (int)std::lround(v));
            else
                ret = GUI::format(_u8L("Infill density: %1% %%"), (int)std::lround(v));
        }
    }
    return ret;
}

void GLCanvas3D::InfillWallsEditing::render_background_texture(const GLCanvas3D& canvas)
{
    if (!m_enabled)
        return;

    const Size cnv_size = canvas.get_canvas_size();
    const int cnv_width = cnv_size.get_width();
    const int cnv_height = cnv_size.get_height();
    if (cnv_width == 0 || cnv_height == 0)
        return;
    const float length = edit_length();
    if (length <= 0.0f)
        return;

    const auto& shader = wxGetApp().get_shader("variable_layer_height");
    if (shader == nullptr)
        return;

    generate_texture();

    wxGetApp().bind_shader(shader);

    shader->set_uniform("z_to_texture_row", float(m_texture.cells - 1) / (float(m_texture.width) * length));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / (float)m_texture.height);
    shader->set_uniform("z_cursor", this->cursor_coordinate(canvas));
    shader->set_uniform("z_cursor_band_width", band_width);
    shader->set_uniform("object_max_z", length);

    glsafe(::glActiveTexture(GL_TEXTURE0 + 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));

    const GLsizei w = (GLsizei)m_texture.width;
    const GLsizei h = (GLsizei)m_texture.height;
    const GLsizei half_w = w / 2;
    const GLsizei half_h = h / 2;
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, half_w, half_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_texture.data.data()));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, half_w, half_h, GL_RGBA, GL_UNSIGNED_BYTE, m_texture.data.data() + m_texture.width * m_texture.height * 4));

    // The bar in its panel, at the bottom of the scene along X / Y, at its right along Z.
    m_axis_bar.render_background(canvas, *shader);

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    wxGetApp().unbind_shader();
}

void GLCanvas3D::InfillWallsEditing::render_curve(const Rect& bar_rect)
{
    const float length = edit_length();
    if (!m_enabled || length <= 0.0f)
        return;

    const auto& shader = wxGetApp().get_shader("flat");
    if (!shader)
        return;

    wxGetApp().bind_shader(shader);

    // Across the bar, the value; along the bar, the position along the edited axis. The vertical bar of Z grows
    // upwards, the horizontal bar of X / Y from left to right (from right to left when the axis points to the left).
    const bool  xy        = m_axis_bar.is_xy();
    const float value_min = min_value();
    const float span      = std::max(max_value() - value_min, 1.0f);
    // Reference line at the value in effect where the edited curve is not painted.
    const float neutral   = std::clamp(neutral_value(), value_min, max_value());

    const Camera& camera = m_canvas.get_active_camera();
    Transform3d view_matrix = camera.get_view_matrix_for_billboard();

    Transform3d model_matrix{ Transform3d::Identity() };
    if (! xy) {
        const float x = bar_rect.get_left() + (neutral - value_min) * bar_rect.get_width() / span;
        model_matrix.data()[3 * 4 + 0] = x;
        model_matrix.data()[3 * 4 + 1] = (bar_rect.get_top() + bar_rect.get_bottom()) * 0.5f;
        model_matrix.data()[0 * 4 + 0] = 0.0f;
        model_matrix.data()[1 * 4 + 1] = (bar_rect.get_top() - bar_rect.get_bottom());
    } else {
        // The unit vertical segment of the line, turned to lie along the bar.
        const float y = bar_rect.get_bottom() + (neutral - value_min) * bar_rect.get_height() / span;
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

    // The points of the curve are <value, position along the axis>.
    Transform3d model_matrix_curve{ Transform3d::Identity() };
    if (! xy) {
        const float scale_x = bar_rect.get_width() / span;
        const float scale_y = bar_rect.get_height() / length;
        model_matrix_curve.data()[3 * 4 + 0] = bar_rect.get_left() - value_min * scale_x;
        model_matrix_curve.data()[3 * 4 + 1] = bar_rect.get_bottom();
        model_matrix_curve.data()[0 * 4 + 0] = scale_x;
        model_matrix_curve.data()[1 * 4 + 1] = scale_y;
    } else {
        const bool  flipped = m_axis_bar.flipped();
        const float along   = (flipped ? -1.0f : 1.0f) * bar_rect.get_width() / length;
        const float across  = bar_rect.get_height() / span;
        model_matrix_curve.matrix()(0, 0) = 0.0;
        model_matrix_curve.matrix()(0, 1) = along;
        model_matrix_curve.matrix()(0, 3) = flipped ? bar_rect.get_right() : bar_rect.get_left();
        model_matrix_curve.matrix()(1, 0) = across;
        model_matrix_curve.matrix()(1, 1) = 0.0;
        model_matrix_curve.matrix()(1, 3) = bar_rect.get_bottom() - value_min * across;
    }
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix_curve);

    if (!m_profile_curve.is_initialized() || m_profile_dirty) {
        // The painted curve, or a straight line at the value in effect while nothing has been painted yet, so the
        // tool always shows what a first click would start from.
        const std::vector<double> curve = display_curve();
        // Points <value, position along the axis> of the displayed line strip.
        std::vector<Vec2f> points;
        if (is_walls_mode()) {
            // The wall count is an integer: draw the staircase the slicer will actually apply.
            const int    samples = 512;
            const double dz = (double)length / (double)samples;
            double prev = std::round(Slic3r::infill_walls_curve_value_at(curve, 0., (double)neutral));
            points.emplace_back((float)prev, 0.0f);
            for (int i = 1; i <= samples; ++ i) {
                const double z = dz * (double)i;
                const double v = std::round(Slic3r::infill_walls_curve_value_at(curve, z, (double)neutral));
                if (v != prev) {
                    points.emplace_back((float)prev, (float)z);
                    points.emplace_back((float)v, (float)z);
                    prev = v;
                }
            }
            points.emplace_back((float)prev, length);
        } else {
            for (size_t i = 0; i + 1 < curve.size(); i += 2)
                points.emplace_back((float)curve[i + 1], (float)curve[i]);
        }

        m_profile_curve.reset();
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(points.size());
        init_data.reserve_indices(points.size());
        for (unsigned int i = 0; i < (unsigned int)points.size(); ++ i) {
            init_data.add_vertex(Vec3f(points[i].x(), points[i].y(), 0.0f));
            init_data.add_index(i);
        }

        m_profile_curve.init_from(std::move(init_data));
        m_profile_dirty = false;
        m_displayed_base = neutral_value();
    }
    m_profile_curve.set_color({ 0.0f, 0.0f, 1.0f, 1.0f });
    m_profile_curve.render_geometry();

    wxGetApp().unbind_shader();
}

void GLCanvas3D::InfillWallsEditing::render_volumes(const GLCanvas3D& canvas, const GLVolumeCollection& volumes)
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

    generate_texture();

    // Along X / Y the shader is given the coordinate along the edited axis as the "z" of the vertices.
    const bool  xy     = m_axis_bar.is_xy();
    const float length = std::max(edit_length(), 1e-3f);
    shader->set_uniform("z_to_texture_row", float(m_texture.cells - 1) / (float(m_texture.width) * length));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / float(m_texture.height));
    shader->set_uniform("z_cursor", this->cursor_coordinate(canvas));
    shader->set_uniform("z_cursor_band_width", float(this->band_width));

    const Camera& camera = m_canvas.get_active_camera();
    const auto& view_matrix = camera.get_view_matrix();
    const auto& projection_matrix = camera.get_projection_matrix();
    shader->set_uniform("projection_matrix", projection_matrix);

    const GLsizei w = (GLsizei)m_texture.width;
    const GLsizei h = (GLsizei)m_texture.height;
    const GLsizei half_w = w / 2;
    const GLsizei half_h = h / 2;
    glsafe(::glActiveTexture(GL_TEXTURE0 + 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, half_w, half_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_texture.data.data()));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, half_w, half_h, GL_RGBA, GL_UNSIGNED_BYTE, m_texture.data.data() + m_texture.width * m_texture.height * 4));
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

void GLCanvas3D::InfillWallsEditing::generate_texture()
{
    if (m_texture.data.empty()) {
        m_texture.width = 1024;
        m_texture.height = 1024;
        m_texture.levels = 2;
        m_texture.data.assign(m_texture.width * m_texture.height * 5, 0);
        m_texture.valid = false;
    }

    if (m_texture.valid && m_texture.cells > 0)
        return;

    m_texture.cells = Slic3r::generate_infill_walls_texture(
        (double)edit_length(), (double)min_value(), (double)max_value(), display_curve(), is_walls_mode(),
        m_texture.data.data(), (int)m_texture.height, (int)m_texture.width, true);
    m_texture.valid = true;
    m_displayed_base = neutral_value();
}

void GLCanvas3D::InfillWallsEditing::adjust_profile()
{
    const float length = edit_length();
    if (m_model_object == nullptr || length <= 0.0f)
        return;

    ensure_profile();
    const std::vector<double> before = m_profile;
    // Along X / Y, the walls at both ends of the object keep their value.
    const std::pair<float, float> paintable = m_axis_bar.is_xy() ? m_axis_bar.paintable_range() : std::make_pair(0.0f, 1.0f);
    Slic3r::adjust_infill_walls_curve(m_profile, (double)length, (double)neutral_value(),
        (double)min_value(), (double)max_value(), (double)this->last_z, (double)this->strength(),
        (double)this->band_width, this->last_action, (double)length * paintable.first, (double)length * paintable.second);
    // What the stroke changed stops at the bounds around it; the bound itself, the top of the last layer on its lower side,
    // goes with that side.
    if (const std::vector<double> bounds = active_bounds(); ! bounds.empty() && m_profile != before) {
        double lo = 0., hi = (double)length;
        for (const double bound : bounds) {
            if (bound < (double)this->last_z)
                lo = bound;
            else {
                hi = bound;
                break;
            }
        }
        m_profile = Slic3r::variable_speed_clip_stroke(before, m_profile, lo, hi, (double)length);
    }
    m_profile_changed = true;
    sync_mode_into_curves();
    m_texture.valid = false;
    m_profile_dirty = true;
}

void GLCanvas3D::InfillWallsEditing::commit_to_model(GLCanvas3D& canvas, const std::string& snapshot_name)
{
    if (m_model_object == nullptr)
        return;
    // Fold the working curve back in, then publish every painted curve (each one applies).
    sync_mode_into_curves();
    ModelObject* model_object = const_cast<ModelObject*>(m_model_object);
    InfillWallsProfile new_profile;
    for (const auto& kv : m_curves)
        if (kv.second.size() >= 4)
            new_profile.curves[kv.first] = kv.second;
    for (const auto& kv : m_bounds)
        if (! kv.second.empty())
            new_profile.bounds[kv.first] = kv.second;
    if (model_object->infill_walls_profile == new_profile)
        return;
    if (! snapshot_name.empty())
        wxGetApp().plater()->take_snapshot(snapshot_name);
    model_object->infill_walls_profile = std::move(new_profile);
    m_seen_profile = model_object->infill_walls_profile;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

void GLCanvas3D::InfillWallsEditing::accept_changes(GLCanvas3D& canvas)
{
    if (last_object_id >= 0)
        commit_to_model(canvas, "Variable infill and walls - Manual edit");
}

void GLCanvas3D::InfillWallsEditing::reset_profile(GLCanvas3D& canvas)
{
    // Removes the edited curve: where no other curve of the quantity is painted, it is sliced with its base value
    // again. The other curves stay.
    m_curves.erase(edited_key());
    load_mode_curve();
    commit_to_model(canvas, "Variable infill and walls - Reset");
}

void GLCanvas3D::InfillWallsEditing::smooth_profile(GLCanvas3D& canvas, const HeightProfileSmoothingParams& smoothing_params)
{
    if (m_model_object == nullptr || m_profile.empty())
        return;
    ensure_profile();
    // The Gaussian smoothing of the variable speed, on each part between the bounds on its own: the steps at the bounds stay.
    m_profile = Slic3r::smooth_variable_speed_profile_bounded(m_profile, active_bounds(), (double)min_value(), (double)max_value(), smoothing_params);
    m_profile_changed = true;
    sync_mode_into_curves();
    m_texture.valid = false;
    m_profile_dirty = true;
    commit_to_model(canvas, "Variable infill and walls - Smooth");
}

} // namespace GUI
} // namespace Slic3r
