#version 450

layout(location = 0) in vec4 color;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec4 probe;
layout(location = 4) in vec3 world_pos;

layout(set = 0, binding = 0) uniform Camera {
    mat4 view_projection;
    vec4 sun_direction;
    vec4 sun_color;
} camera;
layout(set = 0, binding = 1) uniform sampler2D albedo;
layout(set = 0, binding = 2) uniform sampler2D normalTex;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 base = color * texture(albedo, uv);
    if (base.a < 0.5) discard;  // foliage alpha cutout
    // Normal mapping via a derivative (ddx/ddy) cotangent frame — no per-vertex tangent needed
    // (mirrors native_world_renderer.cpp ps_main). Flat (128,128,255) default = no perturbation.
    vec3 Ngeo = normalize(normal);
    vec3 dp1 = dFdx(world_pos), dp2 = dFdy(world_pos);
    vec2 du1 = dFdx(uv), du2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, Ngeo), dp1perp = cross(Ngeo, dp1);
    vec3 T = dp2perp * du1.x + dp1perp * du2.x;
    vec3 B = dp2perp * du1.y + dp1perp * du2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    vec2 nxy = texture(normalTex, uv).rg * 2.0 - 1.0;
    float nz = sqrt(clamp(1.0 - dot(nxy, nxy), 0.0, 1.0));
    vec3 N = normalize(nxy.x * T * invmax + nxy.y * B * invmax + nz * Ngeo);
    // Light: hemisphere ambient OR baked .lmp SH probe, + warm N.L sun.
    float ndl = max(dot(N, -camera.sun_direction.xyz), 0.0);
    float hemi = 0.5 + 0.5 * N.y;
    vec3 ambient = mix(vec3(0.18, 0.20, 0.24), vec3(0.55, 0.58, 0.62), hemi);
    if (probe.w > 0.5) ambient = probe.rgb;
    vec3 lit = base.rgb * (ambient + ndl * camera.sun_color.rgb);
    out_color = vec4(lit, base.a);
}
