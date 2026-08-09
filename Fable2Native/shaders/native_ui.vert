#version 450

// Vulkan mirror of the D3D12 native-UI vertex shader (native_ui_renderer.cpp). Maps screen-pixel
// coordinates to clip space. Vulkan NDC has +Y pointing down, so screen-top (y=0) maps to -1 here
// (the D3D12 shader uses the mirrored `1 - y/H*2` because D3D NDC is +Y up) — both put the UI the
// same way up on screen.
layout(push_constant) uniform Push {
    vec2 viewport;
    uint combine_detail;
    uint key_black_matte;
    uint alpha_mask;
} pc;

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec2 in_detail_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec2 v_detail_uv;

void main() {
    gl_Position = vec4(in_position.x / pc.viewport.x * 2.0 - 1.0,
                       in_position.y / pc.viewport.y * 2.0 - 1.0, 0.0, 1.0);
    v_color = in_color;
    v_uv = in_uv;
    v_detail_uv = in_detail_uv;
}
