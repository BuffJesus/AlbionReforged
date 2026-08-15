#version 450

// Bright-pass: threshold the HDR scene, keeping only energy above the bloom threshold. Mirror of the
// D3D12 native_tonemap.hlsl ps_bright. params.x = threshold.
layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Push { vec4 params; } pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 c = texture(src, v_uv).rgb;
    out_color = vec4(max(c - pc.params.x, vec3(0.0)), 1.0);
}
