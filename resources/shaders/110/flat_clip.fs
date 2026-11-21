#version 110

// Orio : voir flat_clip.vs ; memes uniforms de coupe que gouraud.fs

uniform vec4 uniform_color;
uniform float u_clip_box_enabled;
uniform vec3 u_clip_box_min;
uniform vec3 u_clip_box_max;
uniform vec4 u_clip_region;
// faces de coupe (u_clip_cap_mode, voir gouraud.fs) : oeil ou direction de visee
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
    float depth = gl_FragCoord.z;
    if (u_clip_box_enabled > 0.5) {
        if (u_clip_cap_mode > 0.5) {
            // faces de coupe, comme gouraud.fs : comptage (1), face de coupe d'une piece (2), section d'un modificateur (3)
            if (u_clip_cap_mode < 1.5 && !clip_beyond_entry())
                discard;
            if (u_clip_cap_mode > 2.5)
                depth = max(depth - 4.0e-6, 0.0);
        }
        else {
            bool in_region = v_world_pos.x >= u_clip_region.x && v_world_pos.y >= u_clip_region.y &&
                             v_world_pos.x <= u_clip_region.z && v_world_pos.y <= u_clip_region.w;
            if (in_region && (any(lessThan(v_world_pos, u_clip_box_min)) || any(greaterThan(v_world_pos, u_clip_box_max))))
                discard;
        }
    }
    gl_FragDepth = depth;
    gl_FragColor = uniform_color;
}
