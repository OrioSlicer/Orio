#version 110
uniform vec3 u_base_color;

// Orio : coupe X/Y/Z de l'onglet Preparation (voir gouraud.fs) : le contour de selection suit la
// piece coupee, faces de coupe comprises (u_clip_cap_mode, voir gouraud.fs)
uniform float u_clip_box_enabled;
uniform vec3 u_clip_box_min;
uniform vec3 u_clip_box_max;
uniform vec4 u_clip_region;
uniform vec4 u_clip_eye;
uniform float u_clip_cap_mode;

varying vec3 v_world_pos;

float clip_slab_near(float origin, float dir, float lo, float hi)
{
    if (abs(dir) < 1e-8)
        return -1.0e30;
    return min((lo - origin) / dir, (hi - origin) / dir);
}

// Ce fragment est-il au-dela de l'entree de son rayon de vue dans la boite gardee ? (comme gouraud.fs)
bool clip_beyond_entry()
{
    vec3 dir;
    vec3 origin;
    if (u_clip_eye.w > 0.5) {
        origin = u_clip_eye.xyz;
        dir = normalize(v_world_pos - origin);
    }
    else {
        dir = normalize(u_clip_eye.xyz);
        origin = v_world_pos - 2.0e3 * dir;
    }
    float tx = clip_slab_near(origin.x, dir.x, u_clip_box_min.x, u_clip_box_max.x);
    float ty = clip_slab_near(origin.y, dir.y, u_clip_box_min.y, u_clip_box_max.y);
    float tz = clip_slab_near(origin.z, dir.z, u_clip_box_min.z, u_clip_box_max.z);
    return dot(v_world_pos - origin, dir) > max(tx, max(ty, tz));
}

void main()
{
    if (u_clip_box_enabled > 0.5) {
        if (u_clip_cap_mode > 0.5) {
            // comptage (1) puis faces de la boite gardee (2, 3), comme gouraud.fs
            if (u_clip_cap_mode < 1.5 && !clip_beyond_entry())
                discard;
        }
        else if (v_world_pos.x >= u_clip_region.x && v_world_pos.y >= u_clip_region.y &&
                 v_world_pos.x <= u_clip_region.z && v_world_pos.y <= u_clip_region.w &&
                 (any(lessThan(v_world_pos, u_clip_box_min)) || any(greaterThan(v_world_pos, u_clip_box_max))))
            discard;
    }
    gl_FragColor = vec4(u_base_color, 1.0);
}
