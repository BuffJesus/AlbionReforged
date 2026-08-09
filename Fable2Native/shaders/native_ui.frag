#version 450

// Vulkan mirror of the D3D12 native-UI pixel shader (native_ui_renderer.cpp ps_main). Same three
// material modes so both backends draw the shared f2::render::UiQuad scene identically:
//   key_black_matte : kill near-black export matte (keeps the metallic rim opaque)
//   alpha_mask      : RGB from the vertex color, coverage from the texture alpha (white sparkles)
//   combine_detail  : leather grain (detail.rgb) masked by the shape alpha (sampled.a)
layout(push_constant) uniform Push {
    vec2 viewport;
    uint combine_detail;
    uint key_black_matte;
    uint alpha_mask;
} pc;

layout(set = 0, binding = 0) uniform sampler2D ui_texture;
layout(set = 0, binding = 1) uniform sampler2D detail_texture;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec2 v_detail_uv;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 sampled = texture(ui_texture, v_uv);
    if (pc.key_black_matte != 0u && max(sampled.r, max(sampled.g, sampled.b)) < 0.10) {
        out_color = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    if (pc.alpha_mask != 0u) {
        out_color = vec4(v_color.rgb, v_color.a * sampled.a);
        return;
    }
    if (pc.combine_detail != 0u) {
        vec4 detail = texture(detail_texture, v_detail_uv);
        out_color = v_color * vec4(detail.rgb, sampled.a);
        return;
    }
    out_color = v_color * sampled;
}
