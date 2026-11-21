#version 140

// Orio : vraie section des cordons sur un plan de coupe X/Y (voir gcode_section.vs).
// Un cordon est le volume balaye le long de son segment par son profil reel : un rectangle a bouts
// arrondis, de la largeur et de la hauteur du trajet (le modele de cordon du trancheur), sommet a la
// hauteur de la buse. Les bouts de segment sont arrondis eux aussi : les segments d'un meme cordon
// se raccordent sans trou ni trait. On garde le fragment s'il tombe dans ce volume ; un relief
// (clair au coeur, sombre au bord) separe les cordons voisins.

uniform mat4 projection_matrix;

uniform vec2 u_isTopLayer_hasCustomOptins;
uniform vec3 u_rangeType_isUnlit_topLayerIndex;
uniform vec4 u_isRangeView_isRangeVaild_topLayerOnly_viewType;
uniform vec2 u_pathDataRange;
uniform sampler2D s_color_range_texture;
uniform samplerBuffer s_per_move_data_texture;

// coupe X/Y : (xmin, xmax, ymin, ymax) en coordonnees monde
uniform float u_clip_xy_enabled;
uniform vec4 u_clip_xy;
// vue "Fusion de couche" : section de la piece cliquee seule (label + 1, 0 : toutes), voir gcode.fs
uniform float u_clip_object;

in vec3 v_world_pos;
in vec3 v_view_pos;
flat in vec3 v_seg_start;
flat in vec3 v_seg_end;
flat in vec2 v_width_height;
flat in float v_move_id;

out vec4 frag_color;

// La section est avancee vers la camera (mm) : un peu au bord, pour passer devant les barres coupees
// au ras du plan ; davantage au coeur, pour que la ou deux cordons se chevauchent le coeur le plus
// proche l'emporte.
#define DEPTH_OFFSET_EDGE 0.005
#define DEPTH_OFFSET_CORE 0.03

// copie de extrusion_color() de gcode.fs : memes couleurs que les barres
vec4 extrusion_color(vec3 type_rangeData_deltaExtruder)
{
    vec4 final_color = vec4(0.25, 0.25, 0.25, 1.0); // Neutral_Color
    float path_data = type_rangeData_deltaExtruder.y;
    if (u_isRangeView_isRangeVaild_topLayerOnly_viewType.x > 0.5)
    {
        float view_type = u_isRangeView_isRangeVaild_topLayerOnly_viewType.w;
        if (view_type < 0.5 || (view_type > 8.5 && view_type < 9.5)) // Summer or ColorPrint
        {
            if (int(path_data + 0.5) > int(u_pathDataRange.y + 0.5))
            {
                final_color = vec4(0.5, 0.5, 0.5, 1.0);
                return final_color;
            }
        }
        else if (view_type > 9.5 && view_type < 10.5) // FilamentId
        {
            float path_data2 = type_rangeData_deltaExtruder.z;
            float id = path_data / (u_pathDataRange.y - u_pathDataRange.x);
            float role = path_data2 / (u_pathDataRange.y - u_pathDataRange.x);
            final_color = vec4(id, role, id, 1.0);
            return final_color;
        }
        vec2 uv = vec2(0.0, 0.5);
        if (u_isRangeView_isRangeVaild_topLayerOnly_viewType.y > 0.5)
        {
            if (u_rangeType_isUnlit_topLayerIndex.x < 0.5)
            {
                uv.x = (path_data - u_pathDataRange.x) / (u_pathDataRange.y - u_pathDataRange.x);
            }
            else
            {
                uv.x = (log(path_data) - u_pathDataRange.x) / (u_pathDataRange.y - u_pathDataRange.x);
            }
            uv.x = clamp(uv.x, 0.0, 1.0);
            // Orio [269] : the legend puts color i at i / (n - 1), the texel centers are at (i + 0.5) / n
            float n = float(textureSize(s_color_range_texture, 0).x);
            uv.x = (uv.x * (n - 1.0) + 0.5) / n;
        }
        final_color = texture(s_color_range_texture, uv);
    }
    return final_color;
}

void main()
{
    // Profil du cordon autour du point du segment le plus proche (en XY, donc bouts arrondis) :
    // p = (ecart horizontal, ecart vertical) au centre du profil, sd = distance signee au bord.
    vec2 seg_xy = v_seg_end.xy - v_seg_start.xy;
    float seg_len2 = dot(seg_xy, seg_xy);
    float t = (seg_len2 > 1e-12) ? clamp(dot(v_world_pos.xy - v_seg_start.xy, seg_xy) / seg_len2, 0.0, 1.0) : 0.0;
    vec3 center = mix(v_seg_start, v_seg_end, t);
    vec2 half_size = 0.5 * v_width_height;
    center.z -= half_size.y;
    vec2 p = vec2(length(v_world_pos.xy - center.xy), v_world_pos.z - center.z);
    float radius = min(half_size.x, half_size.y);
    vec2 q = abs(p) - half_size + vec2(radius);
    float sd = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    // bord anticrenele sur environ un pixel (derivee prise avant tout rejet)
    float coverage = clamp(0.5 - sd / max(fwidth(sd), 1e-5), 0.0, 1.0);

    if (coverage <= 0.0)
        discard;
    if (u_clip_object > 0.5 && abs(texelFetch(s_per_move_data_texture, int(v_move_id + 0.5)).a + 1.0 - u_clip_object) > 0.5)
        discard;
    // coupe sur l'autre axe ; tolerance sur l'axe du plan, ou l'on se trouve exactement
    if (u_clip_xy_enabled > 0.5 &&
        (v_world_pos.x < u_clip_xy.x - 1e-3 || v_world_pos.x > u_clip_xy.y + 1e-3 ||
         v_world_pos.y < u_clip_xy.z - 1e-3 || v_world_pos.y > u_clip_xy.w + 1e-3))
        discard;

    // memes regles que les barres : hors couche du haut, gris neutre en mode "couche du haut seule"
    vec4 base_color = vec4(0.25, 0.25, 0.25, 1.0); // Neutral_Color
    if (u_isRangeView_isRangeVaild_topLayerOnly_viewType.z < 0.5 || u_isTopLayer_hasCustomOptins.x > 0.5)
        base_color = extrusion_color(texelFetch(s_per_move_data_texture, int(v_move_id + 0.5)).rgb);

    // relief : 1 au coeur du cordon, 0 au bord, comme un tube vu de face
    float core = clamp(-sd / max(radius, 1e-6), 0.0, 1.0);
    float relief = sqrt(core * (2.0 - core));
    frag_color = vec4(min(base_color.rgb * (0.6 + 0.45 * relief), vec3(1.0)), base_color.a * coverage);

    vec3 to_eye = (projection_matrix[3][3] > 0.5) ? vec3(0.0, 0.0, 1.0) : normalize(-v_view_pos);
    vec4 clip_pos = projection_matrix * vec4(v_view_pos + to_eye * (DEPTH_OFFSET_EDGE + DEPTH_OFFSET_CORE * core), 1.0);
    gl_FragDepth = 0.5 * (clip_pos.z / clip_pos.w) + 0.5;
}
