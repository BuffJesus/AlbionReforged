#version 450

// Separable 9-tap Gaussian along params.xy (texel step). Mirror of the D3D12 ps_blur; identical
// weights {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216}.
layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Push { vec4 params; } pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    const float w[5] = float[5](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec2 step = pc.params.xy;
    vec3 c = texture(src, v_uv).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        c += texture(src, v_uv + step * float(i)).rgb * w[i];
        c += texture(src, v_uv - step * float(i)).rgb * w[i];
    }
    out_color = vec4(c, 1.0);
}
