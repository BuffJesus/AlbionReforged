#version 450

layout(location = 0) in vec2 ndc;
layout(set = 0, binding = 0) uniform Sky {
    vec4 sky_color;
    vec4 horizon_color;
} sky;
layout(location = 0) out vec4 out_color;

void main() {
    float v = clamp(ndc.y * 0.5 + 0.5, 0.0, 1.0);
    out_color = vec4(mix(sky.horizon_color.rgb, sky.sky_color.rgb, v), 1.0);
}
