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
    vec4 fog_color;  // rgb = fog colour, w = max density (0 = off)
    vec4 fog_range;  // x = start dist, y = end dist
    vec4 sky_zenith;   // theme sky gradient top (water reflection tracks the real sky)
    vec4 sky_horizon;  // theme sky gradient bottom
    // Sun shadow map (retail "Render ShadowBuffers", rendering_pipeline.txt §A#3/§D.1): the ortho
    // light view-projection the shadow depth was rendered with, plus its params: x=texel size
    // (1/res), y=depth bias, z=enabled (1/0), w=strength (how dark the shadowed sun term goes).
    mat4 light_view_projection;
    vec4 shadow_params;
    // Authored ambient model (theme Lighting sub-record; fable2-theme-ambient-lighting).
    vec4 ambient_flat;        // rgb = flat AmbientColour, w = has_ambient (1/0)
    vec4 sky_bounce_top;      // rgb = hemisphere sky-bounce (up)
    vec4 sky_bounce_bottom;   // rgb = hemisphere sky-bounce (down)
    vec4 fog_curve;           // start, inv_span2, power, amp (amp>0 = exponential fog)
    vec4 fog_mist;            // strength, depth_scale, mist_top_y, falloff
} camera;
// Grounded fog (ModelPreview.cpp apply_env_fog): exponential power-curve distance fog +
// height-based ground mist. Shared by the world and water paths.
float env_fog_factor(float fd, float wy) {
    float f;
    if (camera.fog_curve.w > 0.0) {
        float dn = max(fd - camera.fog_curve.x, 0.0) * camera.fog_curve.y;
        float od = camera.fog_curve.w * pow(min(dn, 1.25), camera.fog_curve.z);
        f = 1.0 - exp(-od);
    } else {
        f = clamp((fd - camera.fog_range.x) / max(camera.fog_range.y - camera.fog_range.x, 1.0),
                  0.0, 1.0) * camera.fog_color.w;
    }
    if (camera.fog_mist.x > 0.0) {
        float below = clamp((camera.fog_mist.z - wy) / max(camera.fog_mist.w, 0.5), 0.0, 1.0);
        f = clamp(f + camera.fog_mist.x * below * clamp(fd / max(camera.fog_mist.y, 1.0), 0.0, 1.0),
                  0.0, 1.0);
    }
    return f;
}
layout(set = 0, binding = 1) uniform sampler2D albedo;
layout(set = 0, binding = 2) uniform sampler2D normalTex;
layout(set = 0, binding = 3) uniform Lights {
    uint light_count; vec3 _pad;
    vec4 light_pos_range[64];        // xyz = pos, w = range
    vec4 light_color_intensity[64];  // rgb = colour, w = intensity
} lights;
layout(set = 0, binding = 4) uniform sampler2D specTex;  // spec/"material" mask (t2)
layout(set = 0, binding = 5) uniform Water {
    vec4 params[10]; // WaterFile::params[37], then WaterTheme opacity in params[9].y
} water_params;
layout(set = 0, binding = 6) uniform sampler2D sceneDepth;
layout(set = 0, binding = 7) uniform sampler2DShadow shadowMap;  // sun shadow depth (t4-equivalent)

// Sun shadow factor at a world position: 1 = lit, 0 = fully shadowed. Projects world_pos into the
// light's ortho clip space, then 3x3 PCF against the shadow depth. Mirrors the D3D12 sun_shadow():
// the shadow map was rendered with the SAME light VP and a negative-height viewport, so the uv
// Y-flip (0.5,-0.5) matches the D3D12 store orientation exactly.
float sun_shadow(vec3 world_pos) {
    if (camera.shadow_params.z < 0.5) return 1.0;
    vec4 lp = camera.light_view_projection * vec4(world_pos, 1.0);  // ortho -> w = 1
    vec2 uv = lp.xy * vec2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || lp.z < 0.0 || lp.z > 1.0) return 1.0;
    float depth = lp.z - camera.shadow_params.y;  // depth bias to kill acne
    float t = camera.shadow_params.x;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(shadowMap, vec3(uv + vec2(x, y) * t, depth));
    return sum / 9.0;
}
layout(push_constant) uniform Push {
    uint is_water;
    uint has_scene_depth;
    uint is_character;
    uint _padding;
    vec4 character_offset;
    vec4 character_motion;
} pc;
layout(location = 0) out vec4 out_color;

// Animated translucent water: authored dual-scrolled bump normal, Fresnel deep<->surface colour,
// sky reflection, and sun glitter. WaterFile params are bound per material at binding 5.
vec4 water() {
    vec3 wp = world_pos;
    float t = camera.eye_time.w;
    vec2 p = wp.xz;
    vec2 uv0 = p * water_params.params[1].zw + water_params.params[0].zw * t;
    vec2 uv1 = p * water_params.params[2].xy + water_params.params[1].xy * t;
    vec2 n0 = texture(normalTex, uv0).xy * 2.0 - 1.0;
    vec2 n1 = texture(normalTex, uv1).xy * 2.0 - 1.0;
    vec2 nxy = (n0 + n1) * 0.35;
    // Retail uses m_ReflectionScale (params[25]) as a scalar for both components; params[26]
    // belongs to the dropped screen-space refraction tile.
    vec3 N = normalize(vec3(nxy.x, 1.0, nxy.y));
    vec3 V = normalize(camera.eye_time.xyz - wp);
    // Retail uses a nearly horizontal Fresnel normal (water_system_re §5, NORMAL_SCALE=0.05).
    // Keep reflection and glitter on a broad upward normal; the high-frequency normal map
    // produces white noise when applied directly to this term.
    vec3 Nf = normalize(vec3(nxy.x, 1.0, nxy.y));
    float fres = water_params.params[0].x +
                 (1.0 - water_params.params[0].x) *
                     pow(1.0 - clamp(dot(V, Nf), 0.0, 1.0), 5.0);
    vec3 surface = vec3(water_params.params[4].z, water_params.params[4].w,
                        water_params.params[5].x);
    vec3 deep = water_params.params[5].yzw;
    vec3 watercol = mix(deep, surface, fres);
    float fres_reflect = clamp(fres + water_params.params[0].y, 0.0, 1.0);
    float distf = clamp(length(camera.eye_time.xyz - wp) / 75.0, 0.0, 1.0);
    vec3 reflection_ray = reflect(-V, N);
    reflection_ray.y = abs(reflection_ray.y);
    float sky_t = clamp(reflection_ray.y * 0.5 + 0.5, 0.0, 1.0);
    // Reflect the ACTUAL theme sky (per time-of-day) so night water goes dark, matching the
    // rendered sky gradient (horizon -> zenith) instead of a hardcoded daytime blue.
    vec3 sky = mix(camera.sky_horizon.rgb, camera.sky_zenith.rgb, sky_t);
    // Apply the SAME night fade the atmosphere sky pass applies, so the water reflects the real
    // rendered night sky (camera.sun_direction = light-travel dir; sun below horizon -> night).
    float wnight = clamp((camera.sun_direction.y + 0.05) / 0.45, 0.0, 1.0);
    sky = mix(sky, sky * 0.22 + vec3(0.010, 0.018, 0.050), wnight);
    float refl_strength = clamp(water_params.params[7].y, 0.0, 1.0);
    float refl = refl_strength * mix(fres_reflect, 1.0, distf);
    vec3 col = watercol * (1.0 - refl_strength) + sky * refl;
    vec3 L = normalize(camera.sun_direction.xyz);
    vec3 Ng = Nf;
    // The authored PF40 normal map supplies the water ripple detail and glitter response.
    // Keep the authored glitter power, but attenuate its brightness for the native HDR-less
    // target; the retail compositor applies an exposure stage that is not present here.
    col += camera.sun_color.rgb * pow(clamp(dot(V, reflect(L, Ng)), 0.0, 1.0),
                                      water_params.params[9].x) * water_params.params[8].w * 0.25;
    // Retail emits the refraction coefficient as alpha; the ONE/SRC_ALPHA blend lets the
    // framebuffer behind the surface supply the scene/refraction term. Vulkan optionally adds
    // the resolved scene-depth edge factor below when MSAA depth resolve is supported.
    float refr_k = (1.0 - distf) * refl_strength * (1.0 - fres_reflect);
    if (pc.has_scene_depth != 0u) {
        vec2 screen_uv = clamp(gl_FragCoord.xy / vec2(textureSize(sceneDepth, 0)),
                               vec2(0.0), vec2(1.0));
        float scene_z = texture(sceneDepth, screen_uv).r;
        // Reversed-Z: opaque geometry beneath water has a larger depth value than the water
        // surface. Fade the refraction term at the resolved shoreline while leaving open water
        // (resolved far clear = 0) fully visible.
        float shoreline = mix(0.05, 1.0, clamp((gl_FragCoord.z - scene_z) * 256.0, 0.0, 1.0));
        refr_k *= shoreline;
    }
    // Distance fog + ground mist on the water surface too (coherent with opaque geometry).
    if (camera.fog_color.w > 0.0) {
        float fd = length(camera.eye_time.xyz - world_pos);
        float f = env_fog_factor(fd, world_pos.y);
        vec3 fog_tint = (camera.fog_curve.w > 0.0) ? camera.sky_horizon.rgb : camera.fog_color.rgb;
        col = mix(col, fog_tint, f);
    }
    return vec4(col, clamp(refr_k, 0.0, 1.0));
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
    // Cast shadows: attenuate ONLY the sun (N·L + spec) term, never the ambient/baked term, so
    // shadowed surfaces keep their baked/hemisphere fill (retail multiplies only the sun
    // contribution by the sampled shadow buffer). strength (shadow_params.w) sets how dark.
    float shadow = mix(1.0, sun_shadow(world_pos), camera.shadow_params.w);
    ndl *= shadow;
    float hemi = 0.5 + 0.5 * N.y;
    // Ambient: theme-authored model when cooked (flat AmbientColour + SkyColourFinalBounce
    // hemisphere), else the old hardcoded hemisphere. Mirrors the D3D12 world PS.
    vec3 ambient;
    if (camera.ambient_flat.w > 0.5)
        ambient = camera.ambient_flat.rgb +
                  mix(camera.sky_bounce_bottom.rgb, camera.sky_bounce_top.rgb, hemi);
    else
        ambient = mix(vec3(0.18, 0.20, 0.24), vec3(0.55, 0.58, 0.62), hemi);
    if (probe.w > 0.5) ambient = probe.rgb;
    vec3 lit = base.rgb * (ambient + ndl * camera.sun_color.rgb);
    // Specular highlight (world_shading_model_re.txt §7, ladder step 3): Blinn-Phong gated by the
    // grayscale spec/"material" mask (t2). Mirrors the D3D12 world PS. Default mask=0 -> no spec.
    float specMask = texture(specTex, uv).r;
    vec3 Vdir = normalize(camera.eye_time.xyz - world_pos);
    vec3 Hdir = normalize(-camera.sun_direction.xyz + Vdir);
    float spec = pow(max(dot(N, Hdir), 0.0), 32.0) * specMask;
    lit += spec * camera.sun_color.rgb * shadow;  // sun specular is shadowed with the diffuse term
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
    // Distance fog toward the theme fog colour (fog_color.w = max density, 0 = off). Ties distant
    // world geometry to the horizon/backdrop. Matches the D3D12 world PS.
    if (camera.fog_color.w > 0.0) {
        float fd = length(camera.eye_time.xyz - world_pos);
        vec3 fog_tint = (camera.fog_curve.w > 0.0) ? camera.sky_horizon.rgb : camera.fog_color.rgb;
        lit = mix(lit, fog_tint, env_fog_factor(fd, world_pos.y));
    }
    out_color = vec4(lit, 1.0);  // opaque -> alpha 1 makes the global blend a no-op
}
