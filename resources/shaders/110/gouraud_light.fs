#version 110

uniform vec4 uniform_color;
uniform float emission_factor;
// Orio : coupe X/Y de l'apercu G-code. Desactivee par defaut (u_clip_xy_enabled = 0), donc
// sans effet sur les autres usages de gouraud_light (gizmos, axes, marqueur...). En apercu, elle
// s'applique aux trajectoires du renderer Legacy et aux coques. u_clip_xy = (xmin, xmax, ymin, ymax) ;
// une borne non coupee est repoussee tres loin (aucun rejet de ce cote).
uniform float u_clip_xy_enabled;
uniform vec4 u_clip_xy;

// x = tainted, y = specular;
varying vec2 intensity;
varying vec2 v_clip_world_xy;

void main()
{
    if (u_clip_xy_enabled > 0.5 &&
        (v_clip_world_xy.x < u_clip_xy.x || v_clip_world_xy.x > u_clip_xy.y ||
         v_clip_world_xy.y < u_clip_xy.z || v_clip_world_xy.y > u_clip_xy.w))
        discard;

    gl_FragColor = vec4(vec3(intensity.y) + uniform_color.rgb * (intensity.x + emission_factor), uniform_color.a);
}
