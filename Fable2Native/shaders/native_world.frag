#version 450

layout(location = 0) in vec4 color;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec4 probe;

layout(set = 0, binding = 0) uniform Camera {
    mat4 view_projection;
    vec4 sun_direction;
    vec4 sun_color;
} camera;
layout(set = 0, binding = 1) uniform sampler2D albedo;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 base = color * texture(albedo, uv);
    // Alpha-test cutout for foliage (matches the D3D12 world PS).
    if (base.a < 0.5) discard;
    vec3 N = normalize(normal);
    // Light model (world_shading_model_re.txt §7, matching native_world_renderer.cpp):
    // hemisphere ambient (cool sky above / dim ground below by world-up N.y) OR the baked
    // .lmp SH probe when present, + warm N.L directional sun.
    float ndl = max(dot(N, -camera.sun_direction.xyz), 0.0);
    float hemi = 0.5 + 0.5 * N.y;
    vec3 ambient = mix(vec3(0.18, 0.20, 0.24), vec3(0.55, 0.58, 0.62), hemi);
    if (probe.w > 0.5) ambient = probe.rgb;
    vec3 lit = base.rgb * (ambient + ndl * camera.sun_color.rgb);
    out_color = vec4(lit, base.a);
}
