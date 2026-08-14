#version 450

// Faithful port of SkyboxRenderer.cpp kCloudPixelShader (Vulkan mirror of the D3D12 cloud PS):
// density in .a, distance fade beyond 1000u, 4-neighbour gradient-normal lighting, alpha-test
// discard at the retail 5/255 reference.
layout(set = 0, binding = 0) uniform CloudCB {
    mat4 view_projection;
    vec4 viewer_position;
    vec4 viewer_direction;
    vec4 light_position;
    vec4 light_colour;
    vec4 layer_params;      // x=transparency y=ambient z=brightness w=normal-up
    vec4 uv_scale_offset;   // xy=scale, zw=scroll offset
    vec4 cloud_globals;     // x=global brightness, z=alpha ref
} cb;

layout(set = 0, binding = 1) uniform sampler2D cloud_density;

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

void main() {
    vec2 uv = v_uv * cb.uv_scale_offset.xy + cb.uv_scale_offset.zw;
    vec4 centre = texture(cloud_density, uv);
    float density = centre.a;

    float distance_xy = length(v_world.xz - cb.viewer_position.xz);
    float distance_fade = 1.0 - clamp((distance_xy - 1000.0) * 0.001, 0.0, 1.0);
    float alpha = density * cb.layer_params.x * distance_fade;

    vec3 rgb = vec3(0.0);
    if (alpha > 0.001) {
        float plus_x  = textureOffset(cloud_density, uv, ivec2( 1,  0)).a;
        float plus_y  = textureOffset(cloud_density, uv, ivec2( 0,  1)).a;
        float minus_x = textureOffset(cloud_density, uv, ivec2(-1,  0)).a;
        float minus_y = textureOffset(cloud_density, uv, ivec2( 0, -1)).a;
        vec3 normal = normalize(vec3(plus_x - minus_x, cb.layer_params.w, plus_y - minus_y));
        vec3 light_direction = normalize(v_world - cb.light_position.xyz);
        float grazing = 1.0 - clamp(dot(normal, -cb.viewer_direction.xyz), 0.0, 1.0);
        float back  = clamp(dot(light_direction, -normal), 0.0, 1.0);
        float front = clamp(dot(light_direction, normal), 0.0, 1.0);
        float lighting = front * (1.0 + pow(grazing, 32.0)) + back * back * (1.0 - density);
        rgb = ((lighting * cb.light_colour.rgb + vec3(cb.layer_params.y)) *
               vec3(cb.layer_params.z) * centre.rgb) * cb.cloud_globals.x;
    }
    if (alpha <= cb.cloud_globals.z) discard;
    o_color = vec4(rgb, alpha);
}
