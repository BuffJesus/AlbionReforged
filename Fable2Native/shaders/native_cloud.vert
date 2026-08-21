#version 450

// Scrolling cloud-layer vertex shader (Vulkan mirror of native_cloud_renderer.cpp VS). Vertices
// are already in render space (X, height, Z); the cook bakes the XEX Z-up -> Y-up swap.
layout(set = 0, binding = 0) uniform CloudCB {
    mat4 view_projection;
    vec4 viewer_position;
    vec4 viewer_direction;
    vec4 light_position;
    vec4 light_colour;
    vec4 layer_params;      // x=transparency y=ambient z=brightness w=normal-up
    vec4 uv_scale_offset;   // xy=scale, zw=scroll offset
    vec4 cloud_globals;     // x=global brightness, z=alpha ref
} cb;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec2 v_uv;

void main() {
    v_world = in_position;
    v_uv = in_uv;
    gl_Position = cb.view_projection * vec4(in_position, 1.0);
}
