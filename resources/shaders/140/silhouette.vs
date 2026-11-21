#version 140

uniform mat4 u_model_matrix;
uniform mat4 u_view_projection_matrix;

in vec3 v_position;

// Orio : position monde, pour la coupe de l'onglet Preparation
out vec3 v_world_pos;

void main()
{
    vec4 world_pos = u_model_matrix * vec4(v_position, 1.0);
    v_world_pos = world_pos.xyz;
    gl_Position = u_view_projection_matrix * world_pos;
}
