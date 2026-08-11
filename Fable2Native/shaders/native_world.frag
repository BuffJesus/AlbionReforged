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
    vec4 eye_time;   // xyz = camera eye (world), w = elapsed seconds
} camera;
layout(set = 0, binding = 1) uniform sampler2D albedo;
layout(set = 0, binding = 2) uniform sampler2D normalTex;
layout(set = 0, binding = 3) uniform Lights {
    uint light_count; vec3 _pad;
    vec4 light_pos_range[64];        // xyz = pos, w = range
    vec4 light_color_intensity[64];  // rgb = colour, w = intensity
} lights;
layout(set = 0, binding = 4) uniform sampler2D specTex;  // spec/"material" mask (t2)
layout(push_constant) uniform Push { uint is_water; } pc;
layout(location = 0) out vec4 out_color;

// Animated translucent water (mirror of native_world_renderer.cpp ps_water): dual-scrolled
// procedural ripple normal, fresnel deep<->surface colour, sky reflection, sun glitter.
vec4 water() {
    vec3 wp = world_pos;
    float t = camera.eye_time.w;
    vec2 p = wp.xz;
    vec2 uv0 = p * 0.188 + vec2(0.052, 0.011) * t;
    vec2 uv1 = p * 0.220 + vec2(-0.019, 0.019) * t;
    vec2 n0 = vec2(sin(uv0.x * 6.2831853), sin(uv0.y * 6.2831853));
    vec2 n1 = vec2(sin(uv1.x * 6.2831853 + 1.7), sin(uv1.y * 6.2831853 + 1.7));
    vec2 nxy = (n0 + n1) * 0.12;
    vec3 N = normalize(vec3(nxy.x, 1.0, nxy.y));
    vec3 V = normalize(camera.eye_time.xyz - wp);
    float fres = 0.20 + 0.80 * pow(1.0 - clamp(dot(V, N), 0.0, 1.0), 5.0);
    vec3 watercol = mix(vec3(0.370, 0.470, 0.750), vec3(0.000, 0.1275, 0.1913), fres);
    vec3 col = mix(watercol, vec3(0.6549, 0.8157, 1.0), 0.75 * fres);  // sky reflection
    vec3 L = -normalize(camera.sun_direction.xyz);
    col += pow(clamp(dot(V, reflect(-L, N)), 0.0, 1.0), 128.0) * 5.0;  // sun glitter
    return vec4(col, clamp(0.72 + 0.22 * fres, 0.0, 1.0));
}

void main() {
    if (pc.is_water != 0u) { out_color = water(); return; }
    vec4 base = color * texture(albedo, uv);
    if (base.a < 0.5) discard;  // foliage alpha cutout
    // Normal mapping via a derivative cotangent frame (no per-vertex tangent).
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
    float ndl = max(dot(N, -camera.sun_direction.xyz), 0.0);
    float hemi = 0.5 + 0.5 * N.y;
    vec3 ambient = mix(vec3(0.18, 0.20, 0.24), vec3(0.55, 0.58, 0.62), hemi);
    if (probe.w > 0.5) ambient = probe.rgb;
    vec3 lit = base.rgb * (ambient + ndl * camera.sun_color.rgb);
    // Specular highlight (world_shading_model_re.txt §7, ladder step 3): Blinn-Phong gated by the
    // grayscale spec/"material" mask (t2). Mirrors the D3D12 world PS. Default mask=0 -> no spec.
    float specMask = texture(specTex, uv).r;
    vec3 Vdir = normalize(camera.eye_time.xyz - world_pos);
    vec3 Hdir = normalize(-camera.sun_direction.xyz + Vdir);
    float spec = pow(max(dot(N, Hdir), 0.0), 32.0) * specMask;
    lit += spec * camera.sun_color.rgb;
    // Additive local point lights (lamp posts/lanterns/braziers), N.L with a soft
    // linear-squared falloff clamped at each light's range — mirrors the D3D12 world PS.
    for (uint li = 0u; li < lights.light_count; ++li) {
        vec3 d = lights.light_pos_range[li].xyz - world_pos;
        float r = lights.light_pos_range[li].w;
        float dist = length(d);
        if (dist < r) {
            vec3 Lp = d / max(dist, 1e-3);
            float ndlp = max(dot(N, Lp), 0.0);
            float atten = clamp(1.0 - dist / r, 0.0, 1.0);
            atten *= atten;
            lit += base.rgb * lights.light_color_intensity[li].rgb *
                   (ndlp * atten * lights.light_color_intensity[li].w);
        }
    }
    out_color = vec4(lit, 1.0);  // opaque -> alpha 1 makes the global blend a no-op
}
