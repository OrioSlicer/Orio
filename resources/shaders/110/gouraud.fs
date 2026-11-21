#version 110

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;
const float ONE_OVER_EPSILON = 1e4;

struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

struct SlopeDetection
{
    bool actived;
	float normal_z;
    mat3 volume_world_normal_matrix;
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform vec4 color_clip_plane;
uniform SlopeDetection slope;

//BBS: add outline_color
uniform bool is_outline;

uniform bool offset_depth_buffer;

#ifdef ENABLE_ENVIRONMENT_MAP
    uniform sampler2D environment_tex;
    uniform bool use_environment_tex;
#endif // ENABLE_ENVIRONMENT_MAP

varying vec3 clipping_planes_dots;

// x = diffuse, y = specular;
varying vec2 intensity;

uniform PrintVolumeDetection print_volume;
uniform vec3 extruder_printable_heights;

// Orio : zones exclues du plateau courant (bed_exclude_area), en coordonnees monde
// (xmin, ymin, xmax, ymax) : la partie d'une piece au-dessus est assombrie comme la partie hors du plateau.
#define MAX_EXCLUDE_AREAS 4
uniform int exclude_area_count;
uniform vec4 exclude_areas[MAX_EXCLUDE_AREAS];

bool inside_rect(vec4 r, vec2 p)
{
    return p.x > r.x && p.y > r.y && p.x < r.z && p.y < r.w;
}

bool inside_exclude_area(vec2 p)
{
    return (exclude_area_count > 0 && inside_rect(exclude_areas[0], p)) ||
           (exclude_area_count > 1 && inside_rect(exclude_areas[1], p)) ||
           (exclude_area_count > 2 && inside_rect(exclude_areas[2], p)) ||
           (exclude_area_count > 3 && inside_rect(exclude_areas[3], p));
}
varying vec4 world_pos;
varying float world_normal_z;
varying vec3 eye_normal;

// Orio: threshold angle of the overhangs inside modifiers / height ranges (slope detection).
// slope_zone_matrix maps the world coordinates into the zone: unit cube (type 0), unit cylinder along z (type 1) or unit sphere (type 2).
// The zones are sorted by priority, the last one containing the fragment wins.
#define MAX_SLOPE_ZONES 4
uniform int slope_zone_count;
uniform int slope_zone_type[MAX_SLOPE_ZONES];
uniform mat4 slope_zone_matrix[MAX_SLOPE_ZONES];
uniform float slope_zone_normal_z[MAX_SLOPE_ZONES];

bool inside_slope_zone(int type, mat4 world_to_zone, vec4 position)
{
    vec3 p = (world_to_zone * position).xyz;
    if (type == 2)
        return dot(p, p) <= 1.0;
    if (type == 1)
        return dot(p.xy, p.xy) <= 1.0 && abs(p.z) <= 1.0;
    return all(lessThanEqual(abs(p), vec3(1.0)));
}

float slope_normal_z(vec4 position)
{
    float normal_z = slope.normal_z;
    if (slope_zone_count > 0 && inside_slope_zone(slope_zone_type[0], slope_zone_matrix[0], position))
        normal_z = slope_zone_normal_z[0];
    if (slope_zone_count > 1 && inside_slope_zone(slope_zone_type[1], slope_zone_matrix[1], position))
        normal_z = slope_zone_normal_z[1];
    if (slope_zone_count > 2 && inside_slope_zone(slope_zone_type[2], slope_zone_matrix[2], position))
        normal_z = slope_zone_normal_z[2];
    if (slope_zone_count > 3 && inside_slope_zone(slope_zone_type[3], slope_zone_matrix[3], position))
        normal_z = slope_zone_normal_z[3];
    return normal_z;
}

// Orio : outil de coupe a plusieurs plans, une couleur par section. Un fragment ajoute le poids de chaque
// plan dont il est du cote positif (dot(world_pos, plan) > 0) et prend cut_section_colors[total modulo 7]. Les
// poids viennent de l'outil (GLGizmoAdvancedCut::color_clip_sections) : deux sections voisines n'ont jamais la
// meme couleur. Moins de 2 plans : les deux couleurs de part et d'autre de color_clip_plane, comme avant.
#define MAX_CUT_PLANES 16
#define CUT_SECTION_COLORS 7
uniform int cut_plane_count;
uniform vec4 cut_planes[MAX_CUT_PLANES];
uniform float cut_plane_weights[MAX_CUT_PLANES];
uniform vec4 cut_section_colors[CUT_SECTION_COLORS];

vec4 cut_section_color()
{
    float total = 0.0;
    for (int i = 0; i < MAX_CUT_PLANES; ++i) {
        if (i < cut_plane_count && dot(world_pos, cut_planes[i]) > 0.0)
            total += cut_plane_weights[i];
    }
    float section = mod(total, float(CUT_SECTION_COLORS));
    vec4 color = cut_section_colors[0];
    for (int i = 1; i < CUT_SECTION_COLORS; ++i) {
        if (abs(section - float(i)) < 0.5)
            color = cut_section_colors[i];
    }
    return color;
}

// Orio : coupe X/Y/Z de l'onglet Preparation (PrepareCut). Boite gardee en coordonnees monde, une
// borne qui ne coupe pas etant repoussee a +-1e5 ; seuls les fragments dont la position XY tombe dans
// u_clip_region (zone du plateau courant : xmin, ymin, xmax, ymax) sont concernes. Une face arriere visible
// par l'ouverture est l'interieur de la piece coupee : elle devient la face de coupe, en aplat hachure ; avec
// le pochoir, les faces de coupe de u_clip_cap_mode la recouvrent (et comblent les trous des coupes combinees).
uniform float u_clip_box_enabled;
uniform vec3 u_clip_box_min;
uniform vec3 u_clip_box_max;
uniform vec4 u_clip_region;
// oeil (w = 1, camera perspective) ou direction de visee (w = 0, camera orthographique), en coordonnees monde
uniform vec4 u_clip_eye;
// 0 = rendu normal ; faces de coupe, volume par volume (voir PrepareCut::render_caps) : 1 = comptage (pochoir)
// des faces du volume au-dela de l'entree du rayon de vue dans la boite gardee ; puis faces de la boite gardee,
// la ou ce comptage dit le volume coupe : 2 = face de coupe d'une piece, 3 = section d'un modificateur (hachures
// dans l'autre sens, juste devant)
uniform float u_clip_cap_mode;

float clip_slab_near(float origin, float dir, float lo, float hi)
{
    if (abs(dir) < 1e-8)
        return -1.0e30;
    return min((lo - origin) / dir, (hi - origin) / dir);
}

// Point de la face de coupe vu a travers ce fragment : entree du rayon de vue dans la boite gardee, sur le
// plan de coupe traverse ; renvoye en coordonnees de ce plan (mm). beyond : ce fragment est au-dela de ce point.
vec2 clip_cap_point(out bool beyond)
{
    vec3 dir;
    vec3 origin;
    if (u_clip_eye.w > 0.5) {
        origin = u_clip_eye.xyz;
        dir = normalize(world_pos.xyz - origin);
    }
    else {
        dir = normalize(u_clip_eye.xyz);
        origin = world_pos.xyz - 2.0e3 * dir;
    }
    float tx = clip_slab_near(origin.x, dir.x, u_clip_box_min.x, u_clip_box_max.x);
    float ty = clip_slab_near(origin.y, dir.y, u_clip_box_min.y, u_clip_box_max.y);
    float tz = clip_slab_near(origin.z, dir.z, u_clip_box_min.z, u_clip_box_max.z);
    float t = max(tx, max(ty, tz));
    beyond = dot(world_pos.xyz - origin, dir) > t;
    vec3 cap_world = origin + t * dir;
    if (tx >= ty && tx >= tz)
        return cap_world.yz;
    return (ty >= tz) ? cap_world.xz : cap_world.xy;
}

// Face de coupe : aplat de la couleur de la piece, un peu eclairci, hachures a 45 degres d'environ 1 pixel
// (slope = 1), dans l'autre sens pour la section d'un modificateur (slope = -1).
// Espacement en puissance de 2 de mm, de 10 a 20 pixels : les hachures restent lisibles a tout zoom.
vec3 clip_cap_color(vec3 color, vec2 p, vec2 p_fwidth, float slope)
{
    float pixel = max(max(p_fwidth.x, p_fwidth.y), 1e-6);
    float spacing = exp2(ceil(log2(pixel * 10.0)));
    float dist_px = abs(fract((p.x + slope * p.y) / spacing + 0.5) - 0.5) * spacing * 0.70710678 / pixel;
    float ink_cover = clamp(1.1 - dist_px, 0.0, 1.0);
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    vec3 fill = mix(color, vec3(1.0), 0.2);
    vec3 ink = (luma > 0.35) ? color * 0.35 : mix(color, vec3(1.0), 0.6);
    return mix(fill, ink, ink_cover);
}

void main()
{
    // Orio : coupe de l'onglet Preparation. Point de coupe et derivees calcules sous une condition
    // uniforme, avant tout rejet de fragment (derivees valides).
    vec2 cap_p = vec2(0.0);
    vec2 cap_fwidth = vec2(1.0);
    bool cap_beyond = true;
    if (u_clip_box_enabled > 0.5) {
        cap_p = clip_cap_point(cap_beyond);
        cap_fwidth = fwidth(cap_p);
    }
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;
    if (u_clip_box_enabled > 0.5) {
        if (u_clip_cap_mode > 0.5) {
            // faces de coupe (PrepareCut::render_caps) : comptage des faces du volume au-dela du point de coupe vu
            // (pochoir, couleur coupee), puis faces de la boite gardee, ce point meme : hachurees dans la couleur
            // du volume, la section d'un modificateur juste devant la face de coupe des pieces
            bool count = u_clip_cap_mode < 1.5;
            bool section = u_clip_cap_mode > 2.5;
            if (count && !cap_beyond)
                discard;
            gl_FragColor = count ? vec4(0.0) : vec4(clip_cap_color(uniform_color.rgb, cap_p, cap_fwidth, section ? -1.0 : 1.0), 1.0);
            gl_FragDepth = section ? max(gl_FragCoord.z - 4.0e-6, 0.0) : gl_FragCoord.z;
            return;
        }
        bool in_region = world_pos.x >= u_clip_region.x && world_pos.y >= u_clip_region.y &&
                         world_pos.x <= u_clip_region.z && world_pos.y <= u_clip_region.w;
        if (in_region && (any(lessThan(world_pos.xyz, u_clip_box_min)) || any(greaterThan(world_pos.xyz, u_clip_box_max))))
            discard;
        if (in_region && !gl_FrontFacing && !is_outline) {
            gl_FragColor = vec4(clip_cap_color(uniform_color.rgb, cap_p, cap_fwidth, 1.0), uniform_color.a);
            gl_FragDepth = gl_FragCoord.z;
            return;
        }
    }
    vec3  color = uniform_color.rgb;
    float alpha = uniform_color.a;

    if (use_color_clip_plane){
        vec4 full_color;
        if (cut_plane_count > 1)
            full_color = cut_section_color();
        else {
            float color_clip_plane_dot = dot(world_pos, color_clip_plane);
            full_color = (color_clip_plane_dot < EPSILON) ? uniform_color_clip_plane_1 : uniform_color_clip_plane_2;
        }
        color = full_color.rgb;
        alpha = full_color.a;
    }

    if (slope.actived) {
         if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
                color = LightBlue;
                alpha = 0.8;
         }
         else if( world_normal_z < slope_normal_z(world_pos) - EPSILON)
         {
                color = color * 0.5 + LightRed * 0.5;
                alpha = 0.8;
         }
    }
	// if the fragment is outside the print volume -> use darker color
    vec3 pv_check_min =  (world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x)) * ONE_OVER_EPSILON;
    vec3 pv_check_max = (world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y)) * ONE_OVER_EPSILON;
    bool is_out_print_limit =(any(lessThan(pv_check_min, vec3(1.0))) || any(greaterThan(pv_check_max, vec3(1.0))));
    if (print_volume.type == 0) {// rectangle
        color = is_out_print_limit ? mix(color, ZERO, 0.3333) : color;
    }
    else if (print_volume.type == 1) {
        // circle
        float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
        pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x) * ONE_OVER_EPSILON;
        pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y) * ONE_OVER_EPSILON;
        color = (any(lessThan(pv_check_min, vec3(1.0))) || any(greaterThan(pv_check_max, vec3(1.0)))) ? mix(color, ZERO, 0.3333) : color;
    }
    // Orio : au-dessus d'une zone exclue du plateau -> meme assombrissement que hors du plateau
    if (inside_exclude_area(world_pos.xy))
        color = mix(color, ZERO, 0.3333);
    if(extruder_printable_heights.x >= 1.0 ){
        pv_check_min = (world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, extruder_printable_heights.y)) * ONE_OVER_EPSILON;
        pv_check_max = (world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, extruder_printable_heights.z)) * ONE_OVER_EPSILON;
        bool is_out_printable_height = (all(greaterThan(pv_check_min, vec3(1.0))) && all(lessThan(pv_check_max, vec3(1.0)))) ;
        color = is_out_printable_height ? mix(color, ZERO, 0.7) : color;
    }
	//BBS: add outline_color
	if (is_outline)
		gl_FragColor = uniform_color;
#ifdef ENABLE_ENVIRONMENT_MAP
    else if (use_environment_tex)
        gl_FragColor = vec4(0.45 * texture2D(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + 0.8 * color * intensity.x, alpha);
#endif
	else
        gl_FragColor = vec4(vec3(intensity.y) + color * intensity.x, alpha);

    // In the support painting gizmo and the seam painting gizmo are painted triangles rendered over the already
    // rendered object. To resolved z-fighting between previously rendered object and painted triangles, values
    // inside the depth buffer are offset by small epsilon for painted triangles inside those gizmos.
    gl_FragDepth = gl_FragCoord.z - (offset_depth_buffer ? EPSILON : 0.0);
}