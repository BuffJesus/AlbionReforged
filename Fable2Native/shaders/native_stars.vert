#version 450

// Procedural night stars vertex shader (Vulkan mirror of native_sky_stars_renderer.cpp VS;
// retail SkyDomeXex kStarsVertexShaderHlsl). 512 additive point-sprite quads from gl_VertexIndex;
// each star's direction is a hash of its index on the upper hemisphere with a time-scaled twinkle.
// The retail direction's always-positive component (r2.z) is remapped to render-space UP (Y).
layout(set = 0, binding = 0) uniform StarCB {
    vec4 camera_right;    // xyz + tan(fov_x/2) in w
    vec4 camera_up;       // xyz + tan(fov_y/2) in w
    vec4 camera_forward;  // xyz
    vec4 star_params;     // x=time y=brightness z=half_pt_x w=half_pt_y (NDC)
} cb;

layout(location = 0) out float v_colour;

void main() {
    uint star = uint(gl_VertexIndex) / 6u;
    uint corner = uint(gl_VertexIndex) % 6u;
    float index = float(star);
    vec4 r0 = index * vec4(732.051, 236.068, 645.751, 141.421);
    vec4 r2 = fract(r0);
    float phase_rate = 2.5 * cb.star_params.x;
    vec2 dir_xy = r2.xy * 2.0 - 1.0;
    vec4 r3 = r2.xywz + 1.0;
    float r0z = r2.z * r2.z;
    r3 = phase_rate * r3;
    float hash_sq = r2.w * r2.w;
    float len_sq = dot(dir_xy, dir_xy) + r0z;
    r3 = fract(r3);
    float inv_len = inversesqrt(abs(len_sq));
    float twinkle = max(max(r3.x, r3.y), max(r3.z, r3.w));
    vec3 direction = vec3(dir_xy.x * inv_len, r2.z * inv_len, dir_xy.y * inv_len);
    float value = hash_sq * twinkle;
    vec3 world = direction * 2500.0;
    float depth = dot(world, cb.camera_forward.xyz);
    if (depth <= 0.01) {
        gl_Position = vec4(0.0, 0.0, -10.0, 1.0);
        v_colour = 0.0;
        return;
    }
    vec2 ndc = vec2(dot(world, cb.camera_right.xyz) / (depth * cb.camera_right.w),
                    dot(world, cb.camera_up.xyz) / (depth * cb.camera_up.w));
    vec2 corner_offsets[6] = vec2[6](vec2(-1.0, 1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0),
                                     vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
    ndc += corner_offsets[corner] * cb.star_params.zw;
    gl_Position = vec4(ndc, 0.9985, 1.0);
    v_colour = value * cb.star_params.y;
}
