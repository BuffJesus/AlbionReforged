#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(set = 0, binding = 0) uniform Camera {
    mat4 view_projection;
} camera;

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 uv;

void main() {
    gl_Position = camera.view_projection * vec4(in_position, 1.0);
    color = in_color;
    uv = in_uv;
}
