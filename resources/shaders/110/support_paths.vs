#version 110

// Orio: extrusions of the supports of the last slicing in the Prepare view, see GLCanvas3D::_render_sliced_supports().
// The model holds two boxes of unit length (x in [0, 1] and in [2, 3]), width (y in [-0.5, 0.5]) and height (z in [-1, 0]).
// Each instance stretches them over two segments: i_data0 = start (x, y, z) and width of the first one (negative for the
// interface), i_data1 = its end (x, y, z) and its height, i_data2 and i_data3 the same for the second one. z is the top
// of the layer. An empty segment (width 0) collapses its box.

#define INTENSITY_CORRECTION 0.6

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)

#define INTENSITY_AMBIENT    0.3

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 normal_matrix;
// Object coordinates to world, for the clipping.
uniform mat4 volume_world_matrix;
// Clipping of the Prepare view, as for the objects (see gouraud.vs): x = min z, y = max z, and a plane.
uniform vec2 z_range;
uniform vec4 clipping_plane;

attribute vec3 v_position;
attribute vec3 v_normal;

// per instance data
attribute vec4 i_data0;
attribute vec4 i_data1;
attribute vec4 i_data2;
attribute vec4 i_data3;

// x = diffuse, y = specular
varying vec2 intensity;
varying vec3 clipping_planes_dots;
// 1 for the interface
varying float interface_part;
// World position, for the cut of the Prepare tab.
varying vec3 world_pos;

void main()
{
    bool  second = v_position.x > 1.5;
    vec4  start  = second ? i_data2 : i_data0;
    vec4  end    = second ? i_data3 : i_data1;
    float width  = abs(start.w);
    vec2  d      = end.xy - start.xy;
    float len    = length(d);
    vec2  dir    = len > 0.0 ? d / len : vec2(1.0, 0.0);
    vec2  side   = vec2(-dir.y, dir.x);
    // Lengthened by half the width at both ends: the boxes of the segments of a path overlap at the corners.
    float along  = (second ? v_position.x - 2.0 : v_position.x) * (len + width) - 0.5 * width;
    vec3  model_position = vec3(start.xy + dir * along + side * (v_position.y * width), start.z + v_position.z * end.w);
    vec3  model_normal   = vec3(dir * v_normal.x + side * v_normal.y, v_normal.z);
    interface_part = start.w < 0.0 ? 1.0 : 0.0;

    // First transform the normal into camera space and normalize the result.
    vec3 eye_normal = normalize(normal_matrix * model_normal);

    // Compute the cos of the angle between the normal and lights direction. The light is directional so the direction is constant for every vertex.
    // Since these two are normalized the cosine is the dot product. We also need to clamp the result to the [0,1] range.
    float NdotL = max(dot(eye_normal, LIGHT_TOP_DIR), 0.0);

    intensity.x = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    vec4 position = view_model_matrix * vec4(model_position, 1.0);
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position.xyz), reflect(-LIGHT_TOP_DIR, eye_normal)), 0.0), LIGHT_TOP_SHININESS);

    // Perform the same lighting calculation for the 2nd light source (no specular applied).
    NdotL = max(dot(eye_normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;

    vec4 world_position = volume_world_matrix * vec4(model_position, 1.0);
    clipping_planes_dots = vec3(dot(world_position, clipping_plane), world_position.z - z_range.x, z_range.y - world_position.z);
    world_pos = world_position.xyz;

    gl_Position = projection_matrix * position;
}
