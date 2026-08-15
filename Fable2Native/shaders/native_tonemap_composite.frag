#version 450

// Retail composite: mul_sat(exposure * scene) + intensity * bloom (the LDR target clamps). Mirror of
// the D3D12 ps_composite. params.x = exposure, params.y = bloom intensity. set0 = scene, set1 = bloom.
layout(set = 0, binding = 0) uniform sampler2D scene_tex;
layout(set = 1, binding = 0) uniform sampler2D bloom_tex;

layout(push_constant) uniform Push { vec4 params; } pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 scene = texture(scene_tex, v_uv).rgb;
    vec3 glow = texture(bloom_tex, v_uv).rgb;
    vec3 ldr = clamp(pc.params.x * scene, vec3(0.0), vec3(1.0)) + pc.params.y * glow;
    out_color = vec4(ldr, 1.0);
}
