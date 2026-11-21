#version 140

// Orio : shader "flat" avec la coupe X/Y/Z de l'onglet Preparation, pour la selection a la souris :
// une partie masquee par la coupe ne se selectionne plus, une face de coupe selectionne sa piece.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat4 volume_world_matrix;

in vec3 v_position;

out vec3 v_world_pos;

void main()
{
    v_world_pos = (volume_world_matrix * vec4(v_position, 1.0)).xyz;
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
