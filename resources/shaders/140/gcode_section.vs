#version 140

// Orio : vraie section des cordons sur un plan de coupe X/Y de l'apercu.
// Une instance par segment de trajet, avec les memes textures que le shader gcode. Chaque segment
// d'extrusion dont le cordon atteint le plan devient un rectangle pose sur ce plan, qui couvre la
// section du cordon ; gcode_section.fs y decoupe la forme exacte. Les autres segments sont ecrases
// en un point hors champ : rien n'est dessine.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

uniform samplerBuffer s_position_texture;
uniform samplerBuffer s_width_height_data_texture;
uniform samplerBuffer s_segment_texture;
uniform samplerBuffer s_per_move_data_texture;

// (axe coupe : 0 = X, 1 = Y ; position du plan en mm, coordonnees monde)
uniform vec2 u_section_plane;

out vec3 v_world_pos;
out vec3 v_view_pos;
flat out vec3 v_seg_start;
flat out vec3 v_seg_end;
flat out vec2 v_width_height;
flat out float v_move_id;

void main()
{
    vec4 seg_startEnd_hasPrev_prevIndex = texelFetch(s_segment_texture, gl_InstanceID);
    vec3 start_pos = texelFetch(s_position_texture, int(seg_startEnd_hasPrev_prevIndex.x)).xyz;
    vec4 endPos_mid = texelFetch(s_position_texture, int(seg_startEnd_hasPrev_prevIndex.y));
    vec3 end_pos = endPos_mid.xyz;
    int move_id = int(endPos_mid.w + 0.5);
    vec2 width_height = texelFetch(s_width_height_data_texture, move_id).rg;
    float move_type = texelFetch(s_per_move_data_texture, move_id).r;

    v_seg_start = start_pos;
    v_seg_end = end_pos;
    v_width_height = width_height;
    v_move_id = float(move_id);
    v_world_pos = vec3(0.0);
    v_view_pos = vec3(0.0);
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);

    // seules les extrusions (EMoveType::Extrude = 10) ont de la matiere a couper
    if (move_type < 9.5 || move_type > 10.5 || width_height.x <= 0.0 || width_height.y <= 0.0)
        return;

    int axis = int(u_section_plane.x + 0.5);
    float plane = u_section_plane.y;
    // n : axe coupe ; u : l'autre axe horizontal, le long du plan
    float start_n = (axis == 0) ? start_pos.x : start_pos.y;
    float end_n = (axis == 0) ? end_pos.x : end_pos.y;
    float start_u = (axis == 0) ? start_pos.y : start_pos.x;
    float end_u = (axis == 0) ? end_pos.y : end_pos.x;
    float half_width = 0.5 * width_height.x;
    if (plane < min(start_n, end_n) - half_width || plane > max(start_n, end_n) + half_width)
        return;

    // partie du segment a moins d'une demi-largeur du plan -> etendue de la section le long du plan
    float t_lo = 0.0;
    float t_hi = 1.0;
    float delta_n = end_n - start_n;
    if (abs(delta_n) > 1e-6) {
        float t0 = (plane - half_width - start_n) / delta_n;
        float t1 = (plane + half_width - start_n) / delta_n;
        t_lo = clamp(min(t0, t1), 0.0, 1.0);
        t_hi = clamp(max(t0, t1), 0.0, 1.0);
    }
    float u_a = mix(start_u, end_u, t_lo);
    float u_b = mix(start_u, end_u, t_hi);
    // marge pour la frange d'anticrenelage du bord ; le cordon va de z - hauteur a z (sommet = buse)
    float margin = 0.5 * width_height.y;
    float u0 = min(u_a, u_b) - half_width - margin;
    float u1 = max(u_a, u_b) + half_width + margin;
    float z0 = min(start_pos.z, end_pos.z) - width_height.y - margin;
    float z1 = max(start_pos.z, end_pos.z) + margin;

    // coins 0..3 : (u0, z0), (u1, z0), (u1, z1), (u0, z1)
    float u = (gl_VertexID == 1 || gl_VertexID == 2) ? u1 : u0;
    float z = (gl_VertexID >= 2) ? z1 : z0;
    vec3 world_pos = (axis == 0) ? vec3(plane, u, z) : vec3(u, plane, z);

    v_world_pos = world_pos;
    vec4 view_pos = view_model_matrix * vec4(world_pos, 1.0);
    v_view_pos = view_pos.xyz;
    gl_Position = projection_matrix * view_pos;
}
