#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_probe;

layout(set = 0, binding = 0) uniform Camera {
    mat4 view_projection;
    vec4 sun_direction;   // xyz = light dir (world)
    vec4 sun_color;       // rgb = directional sun colour
} camera;

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 uv;
layout(location = 2) out vec3 normal;
layout(location = 3) out vec4 probe;

void main() {
    gl_Position = camera.view_projection * vec4(in_position, 1.0);
    color = in_color;
    uv = in_uv;
    normal = in_normal;
    probe = in_probe;
}
