#version 140

// Orio: extrusions of the supports of the last slicing in the Prepare view, see support_paths.vs.

// Colors of the supports and of their interface.
uniform vec4 uniform_color;
uniform vec4 interface_color;
// Cut X/Y/Z of the Prepare tab, as for the objects (see PrepareCut and gouraud.fs): box kept in world coordinates (a bound
// which does not cut is pushed to +-1e5), only over the current plate (u_clip_region = xmin, ymin, xmax, ymax).
uniform float u_clip_box_enabled;
uniform vec3 u_clip_box_min;
uniform vec3 u_clip_box_max;
uniform vec4 u_clip_region;

// x = diffuse, y = specular
in vec2 intensity;
in vec3 clipping_planes_dots;
in float interface_part;
in vec3 world_pos;

out vec4 frag_color;

void main()
{
    if (any(lessThan(clipping_planes_dots, vec3(0.0))))
        discard;

    vec4 color = interface_part > 0.5 ? interface_color : uniform_color;
    bool in_cut = u_clip_box_enabled > 0.5 &&
        world_pos.x >= u_clip_region.x && world_pos.y >= u_clip_region.y &&
        world_pos.x <= u_clip_region.z && world_pos.y <= u_clip_region.w;
    if (in_cut && (any(lessThan(world_pos, u_clip_box_min)) || any(greaterThan(world_pos, u_clip_box_max))))
        discard;
    // The back faces are only drawn while the cut is on. Seen through the opening, the inside of a cut extrusion is its
    // section: flat and lighter, as the cut faces of the objects.
    if (!gl_FrontFacing) {
        if (!in_cut)
            discard;
        frag_color = vec4(mix(color.rgb, vec3(1.0), 0.2), color.a);
        return;
    }
    frag_color = vec4(vec3(intensity.y) + color.rgb * intensity.x, color.a);
}
