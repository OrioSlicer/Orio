#include "PrepareCut.hpp"

#include <GL/glew.h>

#include "3DScene.hpp"
#include "Camera.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "IMSlider.hpp"
#include "OpenGLManager.hpp"
#include "PartPlate.hpp"
#include "libslic3r/Geometry.hpp"

#include <wx/event.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {
// borne qui ne coupe pas : repoussee hors de portee (ni rejet ni face de coupe de ce cote)
constexpr float CUT_FAR = 1.0e5f;
// marge (mm) de la zone du plateau courant autour du plateau et de ses pieces
constexpr double REGION_MARGIN_MM = 1.0;

// Le tampon d'image courant a-t-il un pochoir ? La fenetre en a un (OpenGLManager::create_wxglcanvas), les tampons hors
// ecran aussi (FrameBuffer) ; verifie pour ne jamais dessiner de face de coupe fausse si ce n'etait pas le cas.
bool stencil_available()
{
    if (!OpenGLManager::get_gl_info().is_version_greater_or_equal_to(3, 0))
        return true;
    GLint framebuffer = 0;
    glsafe(::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer));
    if (framebuffer == 0)
        return true;
    GLint type = GL_NONE;
    glsafe(::glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type));
    return type != GL_NONE;
}

// pochoir rendu a son etat par defaut
void reset_stencil()
{
    glsafe(::glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP));
    glsafe(::glStencilFunc(GL_ALWAYS, 0, 0xFF));
    glsafe(::glStencilMask(0xFF));
    glsafe(::glDisable(GL_STENCIL_TEST));
}

// Rectangle de fenetre (x, y, largeur, hauteur) couvert par une boite monde, agrandi d'un pixel et borne au viewport ;
// tout le viewport si un coin est derriere l'oeil.
std::array<GLint, 4> screen_rect(const BoundingBoxf3& box, const Matrix4d& view_projection, const GLint viewport[4])
{
    const std::array<GLint, 4> all = { viewport[0], viewport[1], viewport[2], viewport[3] };
    if (!box.defined)
        return all;
    double x0 = std::numeric_limits<double>::max();
    double y0 = x0;
    double x1 = -x0;
    double y1 = -x0;
    for (int i = 0; i < 8; ++i) {
        const Vec4d corner((i & 1) ? box.max.x() : box.min.x(), (i & 2) ? box.max.y() : box.min.y(),
                           (i & 4) ? box.max.z() : box.min.z(), 1.0);
        const Vec4d clip = view_projection * corner;
        if (clip.w() <= 1.0e-9)
            return all;
        const double x = viewport[0] + (0.5 * clip.x() / clip.w() + 0.5) * viewport[2];
        const double y = viewport[1] + (0.5 * clip.y() / clip.w() + 0.5) * viewport[3];
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    const double left   = std::clamp(std::floor(x0) - 1.0, double(viewport[0]), double(viewport[0] + viewport[2]));
    const double bottom = std::clamp(std::floor(y0) - 1.0, double(viewport[1]), double(viewport[1] + viewport[3]));
    const double right  = std::clamp(std::ceil(x1) + 1.0, double(viewport[0]), double(viewport[0] + viewport[2]));
    const double top    = std::clamp(std::ceil(y1) + 1.0, double(viewport[1]), double(viewport[1] + viewport[3]));
    return { GLint(left), GLint(bottom), GLint(right - left), GLint(top - bottom) };
}
} // namespace

PrepareCut::PrepareCut()
{
    const char* labels[3] = { "X", "Y", "Z" };
    for (int axis = 0; axis < 3; ++axis) {
        m_sliders[axis] = new IMSlider(0, 0, 0, 100, wxSL_VERTICAL);
        m_sliders[axis]->set_as_cut_slider(labels[axis]);
    }
}

PrepareCut::~PrepareCut()
{
    for (IMSlider*& slider : m_sliders) {
        delete slider;
        slider = nullptr;
    }
}

void PrepareCut::update(const GLVolumeCollection& volumes, PartPlate* plate, bool enabled)
{
    m_visible = false;
    m_active  = false;
    m_plate_instances.clear();
    if (!enabled || plate == nullptr)
        return;

    // pieces du plateau courant (ni modificateurs ni tour de purge) : etendue des curseurs ; avec les modificateurs :
    // tout ce qui est coupe
    BoundingBoxf3 extent;
    BoundingBoxf3 cut_extent;
    for (const GLVolume* volume : volumes.volumes) {
        if (volume == nullptr || !volume->is_active || volume->is_wipe_tower || volume->composite_id.volume_id < 0)
            continue;
        if (!plate->contain_instance(volume->object_idx(), volume->instance_idx()))
            continue;
        m_plate_instances.emplace(volume->object_idx(), volume->instance_idx());
        const BoundingBoxf3 box = volume->transformed_bounding_box();
        cut_extent.merge(box);
        if (!volume->is_modifier)
            extent.merge(box);
    }
    if (!extent.defined)
        return;

    // curseurs reconstruits seulement si l'etendue ou le plateau changent : le reglage est preserve
    const Vec3d origin = plate->get_origin();
    const bool changed = !m_extent.defined
        || (m_extent.min - extent.min).cwiseAbs().maxCoeff() > 1.0e-3
        || (m_extent.max - extent.max).cwiseAbs().maxCoeff() > 1.0e-3
        || (m_origin - origin).cwiseAbs().maxCoeff() > 1.0e-6;
    if (changed)
        rebuild_sliders(extent, origin);
    m_visible = true;

    // zone coupee : le plateau courant, elargi a ses pieces et modificateurs qui en debordent
    BoundingBoxf3 region = cut_extent;
    const BoundingBoxf3& plate_box = plate->get_bounding_box();
    if (plate_box.defined)
        region.merge(plate_box);
    m_region = { float(region.min.x() - REGION_MARGIN_MM), float(region.min.y() - REGION_MARGIN_MM),
                 float(region.max.x() + REGION_MARGIN_MM), float(region.max.y() + REGION_MARGIN_MM) };
    m_region_z = { float(cut_extent.min.z() - REGION_MARGIN_MM), float(cut_extent.max.z() + REGION_MARGIN_MM) };

    // Une borne ne coupe que si sa poignee a quitte son bout de course ; les autres sont repoussees au
    // loin (aucune piece rognee de ce cote).
    for (int axis = 0; axis < 3; ++axis) {
        IMSlider* slider = m_sliders[axis];
        const bool low  = !slider->is_lower_at_min();
        const bool high = !slider->is_higher_at_max();
        m_box_min[axis] = low  ? float(slider->GetLowerValueD())  : -CUT_FAR;
        m_box_max[axis] = high ? float(slider->GetHigherValueD()) :  CUT_FAR;
        m_active = m_active || low || high;
    }
}

void PrepareCut::rebuild_sliders(const BoundingBoxf3& extent, const Vec3d& origin)
{
    m_extent = extent;
    m_origin = origin;
    for (int axis = 0; axis < 3; ++axis) {
        IMSlider* slider = m_sliders[axis];
        const double lo = extent.min(axis);
        double       hi = extent.max(axis);
        if (!(hi > lo))
            hi = lo + 1.0;
        // resolution ~0,1 mm, bornee pour rester raisonnable (comme les curseurs X/Y de l'Apercu)
        int steps = (int) std::round((hi - lo) / 0.1);
        steps = std::max(50, std::min(steps, 4000));
        std::vector<double> values(steps + 1);
        for (int i = 0; i <= steps; ++i)
            values[i] = lo + (hi - lo) * double(i) / double(steps);
        slider->SetSliderValues(values);
        slider->SetMaxValue(steps);
        // cotes en coordonnees du plateau courant ; Z depuis le plateau
        slider->set_cut_label_offset(label_offset(axis));

        // Coupe memorisee : chaque borne n'est reprise que si elle tombe strictement dans la nouvelle
        // etendue (sinon ce cote reste en bout de course, sans oublier la memoire).
        auto index_of = [lo, hi, steps](double v) {
            return std::max(0, std::min(steps, (int) std::round((v - lo) / (hi - lo) * double(steps))));
        };
        int idx_low  = 0;
        int idx_high = steps;
        const double mem_low  = m_memory_mm[axis][0] + label_offset(axis);
        const double mem_high = m_memory_mm[axis][1] + label_offset(axis);
        if (m_memory_active[axis][0] && mem_low > lo && mem_low < hi)
            idx_low = index_of(mem_low);
        if (m_memory_active[axis][1] && mem_high > lo && mem_high < hi)
            idx_high = index_of(mem_high);
        if (idx_low > idx_high) {
            idx_low  = 0;
            idx_high = steps;
        }
        slider->SetSelectionSpan(idx_low, idx_high);
    }
}

void PrepareCut::remember(int axis)
{
    IMSlider* slider = m_sliders[axis];
    if (slider == nullptr || !m_extent.defined)
        return;
    m_memory_active[axis][0] = !slider->is_lower_at_min();
    m_memory_active[axis][1] = !slider->is_higher_at_max();
    m_memory_mm[axis][0]     = slider->GetLowerValueD() - label_offset(axis);
    m_memory_mm[axis][1]     = slider->GetHigherValueD() - label_offset(axis);
}

void PrepareCut::reset()
{
    for (int axis = 0; axis < 3; ++axis) {
        IMSlider* slider = m_sliders[axis];
        if (slider != nullptr && !slider->is_full_span())
            slider->SetSelectionSpan(slider->GetMinValue(), slider->GetMaxValue());
        m_memory_active[axis] = { { false, false } };
    }
}

bool PrepareCut::render(int canvas_width, int canvas_height, float band_top, float scale, bool is_dark)
{
    if (!m_visible)
        return false;

    for (IMSlider* slider : m_sliders) {
        slider->set_scale(scale);
        if (is_dark != m_is_dark)
            slider->on_change_color_mode(is_dark);
    }
    m_is_dark = is_dark;

    const float band_height = std::max(1.0f, float(canvas_height) - band_top);
    bool changed = false;
    int  axis    = m_axis;
    if (render_cut_axis_buttons("prepare", canvas_width, band_top, band_height, scale, is_dark, axis, m_active)) {
        reset();
        changed = true;
    }
    if (axis != m_axis) {
        m_axis  = axis;
        changed = true;
    }
    // render_cut() ne renvoie vrai que si l'utilisateur a deplace une poignee
    if (m_sliders[m_axis]->render_cut(canvas_width, int(band_height), 0, band_top)) {
        remember(m_axis);
        changed = true;
    }
    return changed;
}

bool PrepareCut::on_mouse_wheel(wxMouseEvent& evt)
{
    if (!m_visible)
        return false;
    IMSlider* slider = m_sliders[m_axis];
    const int lower  = slider->GetLowerValue();
    const int higher = slider->GetHigherValue();
    slider->on_mouse_wheel(evt);
    if (slider->GetLowerValue() == lower && slider->GetHigherValue() == higher)
        return false;
    remember(m_axis);
    return true;
}

void PrepareCut::apply_uniforms(GLShaderProgram& shader, const Camera* camera) const
{
    shader.set_uniform("u_clip_box_enabled", m_active ? 1.0f : 0.0f);
    set_cap_mode(shader, CapMode::Parts);
    if (!m_active)
        return;
    shader.set_uniform("u_clip_box_min", Vec3f(m_box_min[0], m_box_min[1], m_box_min[2]));
    shader.set_uniform("u_clip_box_max", Vec3f(m_box_max[0], m_box_max[1], m_box_max[2]));
    shader.set_uniform("u_clip_region", m_region);
    if (camera != nullptr) {
        // faces de coupe : rayon de vue depuis l'oeil (perspective) ou selon la direction de visee (ortho)
        if (camera->get_type() == Camera::EType::Perspective) {
            const Vec3d eye = camera->get_position();
            shader.set_uniform("u_clip_eye", Vec4f(float(eye.x()), float(eye.y()), float(eye.z()), 1.0f));
        }
        else {
            const Vec3d dir = camera->get_dir_forward();
            shader.set_uniform("u_clip_eye", Vec4f(float(dir.x()), float(dir.y()), float(dir.z()), 0.0f));
        }
    }
}

void PrepareCut::disable_uniforms(GLShaderProgram& shader)
{
    shader.set_uniform("u_clip_box_enabled", 0.0f);
    set_cap_mode(shader, CapMode::Parts);
}

void PrepareCut::set_cap_mode(GLShaderProgram& shader, CapMode mode)
{
    shader.set_uniform("u_clip_cap_mode", float(int(mode)));
}

bool PrepareCut::on_cut_plate(const GLVolume& volume) const
{
    return m_plate_instances.count({ volume.object_idx(), volume.instance_idx() }) > 0;
}

bool PrepareCut::can_render_caps() const
{
    return m_active && stencil_available();
}

BoundingBoxf3 PrepareCut::cap_box() const
{
    // les faces de coupe sont toutes dans la zone coupee : une borne qui ne coupe pas y est ramenee
    const float far_bound = 0.5f * CUT_FAR;
    const Vec3d lo(m_box_min[0] > -far_bound ? m_box_min[0] : m_region[0],
                   m_box_min[1] > -far_bound ? m_box_min[1] : m_region[1],
                   m_box_min[2] > -far_bound ? m_box_min[2] : m_region_z[0]);
    const Vec3d hi(m_box_max[0] < far_bound ? m_box_max[0] : m_region[2],
                   m_box_max[1] < far_bound ? m_box_max[1] : m_region[3],
                   m_box_max[2] < far_bound ? m_box_max[2] : m_region_z[1]);
    const Vec3d size = (hi - lo).cwiseMax(Vec3d::Constant(1.0e-3));
    return BoundingBoxf3(lo, Vec3d(lo + size));
}

void PrepareCut::render_caps(GLShaderProgram& shader, const Camera& camera, const GLVolumeCollection& volumes,
                             const std::function<bool(const GLVolume&)>& filter,
                             const std::function<void(GLVolume&, size_t)>& draw,
                             const std::function<std::array<float, 4>(const GLVolume&, size_t, const Transform3d&)>& box) const
{
    if (!m_active)
        return;
    // volumes du plateau courant que la boite gardee coupe (ni entierement dedans, ni entierement dehors) : les pieces,
    // puis les modificateurs (bloqueurs / forceurs de supports, volumes negatifs), leur section devant
    const BoundingBoxf3 keep = cap_box();
    std::vector<size_t> cut;
    for (int modifiers = 0; modifiers < 2; ++modifiers)
        for (size_t i = 0; i < volumes.volumes.size(); ++i) {
            const GLVolume* volume = volumes.volumes[i];
            if (volume == nullptr || !volume->is_active || !volume->visible || volume->is_wipe_tower || volume->composite_id.volume_id < 0 ||
                volume->is_modifier != (modifiers == 1) || !on_cut_plate(*volume) || (filter && !filter(*volume)))
                continue;
            const BoundingBoxf3 bbox = volume->transformed_bounding_box();
            if (keep.intersects(bbox) && !keep.contains(bbox))
                cut.push_back(i);
        }
    if (cut.empty())
        return;

    if (!m_cap_box.is_initialized()) {
        // cube unite, faces en sens direct vues de l'exterieur
        GLModel::Geometry init_data;
        init_data.format = { GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(24);
        init_data.reserve_indices(36);
        const std::array<std::array<Vec3f, 4>, 6> faces = { {
            { { Vec3f(0.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(1.f, 1.f, 0.f), Vec3f(1.f, 0.f, 0.f) } }, // -Z
            { { Vec3f(0.f, 0.f, 1.f), Vec3f(1.f, 0.f, 1.f), Vec3f(1.f, 1.f, 1.f), Vec3f(0.f, 1.f, 1.f) } }, // +Z
            { { Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(1.f, 0.f, 1.f), Vec3f(0.f, 0.f, 1.f) } }, // -Y
            { { Vec3f(1.f, 1.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 1.f, 1.f), Vec3f(1.f, 1.f, 1.f) } }, // +Y
            { { Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 0.f), Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 1.f, 1.f) } }, // -X
            { { Vec3f(1.f, 0.f, 0.f), Vec3f(1.f, 1.f, 0.f), Vec3f(1.f, 1.f, 1.f), Vec3f(1.f, 0.f, 1.f) } }  // +X
        } };
        for (const std::array<Vec3f, 4>& face : faces) {
            const unsigned int v = (unsigned int) init_data.vertices_count();
            for (const Vec3f& corner : face)
                init_data.add_vertex(corner);
            init_data.add_triangle(v, v + 1, v + 2);
            init_data.add_triangle(v, v + 2, v + 3);
        }
        m_cap_box.init_from(std::move(init_data));
    }
    const Transform3d box_matrix = Geometry::translation_transform(keep.min) * Geometry::scale_transform(keep.size());

    GLint viewport[4] = { 0, 0, 0, 0 };
    glsafe(::glGetIntegerv(GL_VIEWPORT, viewport));
    GLint depth_func = GL_LESS;
    glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_func));
    GLboolean depth_mask = GL_TRUE;
    glsafe(::glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask));
    const bool depth_test = ::glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    const bool scissor = ::glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    GLint scissor_box[4] = { 0, 0, 0, 0 };
    glsafe(::glGetIntegerv(GL_SCISSOR_BOX, scissor_box));
    const bool cull_face = ::glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    GLint cull_face_mode = GL_BACK;
    glsafe(::glGetIntegerv(GL_CULL_FACE_MODE, &cull_face_mode));
    GLint front_face = GL_CCW;
    glsafe(::glGetIntegerv(GL_FRONT_FACE, &front_face));
    const Matrix4d view_projection = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_STENCIL_TEST));
    glsafe(::glEnable(GL_SCISSOR_TEST));
    for (size_t idx : cut) {
        GLVolume& volume = *volumes.volumes[idx];
        // tout se passe dans le rectangle d'ecran du volume, pochoir remis a zero
        const std::array<GLint, 4> rect = screen_rect(volume.transformed_bounding_box(), view_projection, viewport);
        if (rect[2] <= 0 || rect[3] <= 0)
            continue;
        glsafe(::glScissor(rect[0], rect[1], rect[2], rect[3]));
        glsafe(::glStencilMask(0xFF));
        glsafe(::glClear(GL_STENCIL_BUFFER_BIT));
        // 1. comptage de ses faces au-dela du point de coupe vu, cachees ou non : +1 par face arriere, -1 par face
        //    avant ; non nul la ou ce point est dans le volume, meme quand le rayon ressort de la boite gardee par un
        //    autre plan de coupe avant de toucher une paroi, et meme avec des parties qui se recouvrent
        glsafe(::glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE));
        glsafe(::glDepthMask(GL_FALSE));
        glsafe(::glDepthFunc(GL_ALWAYS));
        glsafe(::glStencilFunc(GL_ALWAYS, 0, 0xFF));
        glsafe(::glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_DECR_WRAP));
        glsafe(::glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_INCR_WRAP));
        set_cap_mode(shader, CapMode::Count);
        draw(volume, idx);
        // 2. faces de la boite gardee tournees vers l'oeil, c'est-a-dire les points de coupe vus, la ou le compte
        //    n'est pas nul : face de coupe de la piece, ou section du modificateur juste devant
        glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glDepthFunc(volume.is_modifier ? GL_LESS : GL_LEQUAL));
        glsafe(::glEnable(GL_CULL_FACE));
        glsafe(::glCullFace(GL_BACK));
        glsafe(::glFrontFace(GL_CCW));
        glsafe(::glStencilMask(0x00));
        glsafe(::glStencilFunc(GL_NOTEQUAL, 0, 0xFF));
        glsafe(::glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP));
        set_cap_mode(shader, volume.is_modifier ? CapMode::Section : CapMode::Cap);
        // GLModel::render_geometry pose uniform_color avec la couleur du modele (noir par defaut) : celle du volume
        m_cap_box.set_color(-1, box(volume, idx, box_matrix));
        m_cap_box.render_geometry();
    }
    set_cap_mode(shader, CapMode::Parts);
    reset_stencil();
    glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    glsafe(::glDepthFunc(GLenum(depth_func)));
    glsafe(::glDepthMask(depth_mask));
    if (!depth_test)
        glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]));
    if (!scissor)
        glsafe(::glDisable(GL_SCISSOR_TEST));
    glsafe(::glCullFace(GLenum(cull_face_mode)));
    glsafe(::glFrontFace(GLenum(front_face)));
    if (cull_face)
        glsafe(::glEnable(GL_CULL_FACE));
    else
        glsafe(::glDisable(GL_CULL_FACE));
}

} // namespace GUI
} // namespace Slic3r
