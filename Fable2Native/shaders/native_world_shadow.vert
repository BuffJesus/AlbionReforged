#version 450

// Depth-only sun shadow pass (retail "Render ShadowBuffers"): replay opaque world geometry from
// the sun's ortho POV into the shadow depth map. Mirrors the D3D12 vs_shadow entry point. The
// light view-projection lives in the shared Camera UBO (binding 0), written by the world render().

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_probe;

layout(set = 0, binding = 0) uniform Camera {
    mat4 view_projection;
    vec4 sun_direction;
    vec4 sun_color;
    vec4 eye_time;
    vec4 fog_color;
    vec4 fog_range;
    vec4 sky_zenith;
    vec4 sky_horizon;
    mat4 light_view_projection;  // sun ortho VP the shadow map is rendered with
    vec4 shadow_params;          // x=texel size, y=depth bias, z=enabled, w=strength
} camera;

void main() {
    // make_geometry bakes world-space positions, so project straight through the light VP.
    gl_Position = camera.light_view_projection * vec4(in_position, 1.0);
}
