#version 450

layout(location = 0) in vec2 ndc;
layout(set = 0, binding = 0) uniform Sky {
    vec4 sky_color;
    vec4 horizon_color;
    vec4 sunset_color;     // rgb = sunset tint, w = strength (0 = off)
    vec4 sun_direction;    // toward the sun (xyz)
    vec4 camera_right;     // w = tan(fov_x/2)
    vec4 camera_up;        // w = tan(fov_y/2)
    vec4 camera_forward;
} sky;
layout(location = 0) out vec4 out_color;

void main() {
    float v = clamp(ndc.y * 0.5 + 0.5, 0.0, 1.0);
    // Ramp bias (theme complementary_bias in horizon_color.w): higher bias raises the exponent
    // so the horizon tint extends further up. 0 = the old linear ramp (matches D3D12).
    v = pow(v, 1.0 + 2.0 * sky.horizon_color.w);
    vec3 col = mix(sky.horizon_color.rgb, sky.sky_color.rgb, v);
    // Sunset halo (SkyboxRenderer.cpp:170-174): a Mie-forward-lobe warm tint toward the sun,
    // active ONLY when the sun is near the horizon (dawn/dusk). Gated by sunset_color.w (0
    // when no `sky_sunset` opcode) so the default look is unchanged. Matches the D3D12 sky.
    if (sky.sunset_color.w > 0.0) {
        vec3 rd = normalize(sky.camera_forward.xyz +
                            sky.camera_right.xyz * (ndc.x * sky.camera_right.w) +
                            sky.camera_up.xyz * (ndc.y * sky.camera_up.w));
        vec3 sd = normalize(sky.sun_direction.xyz);
        float cosT = dot(rd, sd);
        float gm = 0.80;
        float hg = 1.0 + gm * gm - 2.0 * gm * cosT;
        float phaseM = 0.079577468 * (1.0 - gm * gm) / max(pow(abs(hg), 1.5), 0.0001);
        float sunset_w = clamp(1.0 - abs(sd.y) * 5.0, 0.0, 1.0) *
                         clamp(cosT * 0.5 + 0.5, 0.0, 1.0);
        col = mix(col, sky.sunset_color.rgb * (0.4 + 0.8 * phaseM),
                  sunset_w * 0.45 * sky.sunset_color.w);
    }
    out_color = vec4(col, 1.0);
}
