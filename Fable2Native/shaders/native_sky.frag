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
    vec4 theme_params;     // x=sun_intensity y=bias z=rayleigh(>0 => atmosphere) w=mie
} sky;
layout(location = 0) out vec4 out_color;

// Per-pixel view ray from the camera basis (matches the D3D12 sky).
vec3 view_ray() {
    return normalize(sky.camera_forward.xyz +
                     sky.camera_right.xyz * (ndc.x * sky.camera_right.w) +
                     sky.camera_up.xyz * (ndc.y * sky.camera_up.w));
}

void main() {
    // ---- Analytic atmosphere (retail reference: SkyboxRenderer.cpp PS) when rayleigh > 0 ----
    if (sky.theme_params.z > 0.0) {
        vec3 ray = view_ray();
        vec3 sun_dir = normalize(sky.sun_direction.xyz);
        float rayleigh = max(sky.theme_params.z, 0.05);
        float mie = max(sky.theme_params.w, 0.05);
        vec3 betaR = vec3(0.007337, 0.009459, 0.0257276) * rayleigh;
        vec3 betaM = vec3(0.0056149, 0.0063754, 0.0105143) * mie;
        float cosT = dot(ray, sun_dir);
        float phaseR = 0.059683103 * (1.0 + cosT * cosT);
        float gm = 0.80;
        float hg = 1.0 + gm * gm - 2.0 * gm * cosT;
        float phaseM = 0.079577468 * (1.0 - gm * gm) / max(pow(abs(hg), 1.5), 0.0001);
        float elev = max(ray.y, 0.004);
        float path = 1.0 / (elev + 0.09);
        vec3 od = (betaR + betaM) * path * 26.0;
        vec3 extinct = exp(-od);
        vec3 beta_sum = max(betaR + betaM, vec3(0.00001));
        vec3 inscatter = (betaR * phaseR + betaM * phaseM) / beta_sum * (1.0 - extinct);
        float sun_h = clamp(sun_dir.y * 2.2 + 0.12, 0.0, 1.0);
        vec3 col = inscatter * (7.2 * sun_h) * sky.sky_color.rgb;
        float horizonf = 1.0 - clamp(elev * 3.2, 0.0, 1.0);
        float bias = clamp(sky.theme_params.y, 0.0, 1.0);
        col = mix(col, sky.horizon_color.rgb * (0.35 + 0.65 * sun_h),
                  horizonf * (0.55 + 0.30 * bias));
        float sunset_w = clamp(1.0 - abs(sun_dir.y) * 5.0, 0.0, 1.0) *
                         clamp(cosT * 0.5 + 0.5, 0.0, 1.0);
        col = mix(col, sky.sunset_color.rgb * (0.4 + 0.8 * phaseM),
                  sunset_w * 0.45 * sky.sunset_color.w);
        float night = clamp((-sun_dir.y + 0.05) / 0.45, 0.0, 1.0);
        col = mix(col, col * 0.22 + vec3(0.010, 0.018, 0.050), night);
        float hh = clamp(ray.y * 0.5 + 0.5, 0.0, 1.0);
        col += sky.sky_color.rgb * (1.0 - hh) * 0.05;
        out_color = vec4(col, 1.0);
        return;
    }

    // ---- Flat gradient stand-in (scenes without atmosphere params) ----
    float v = clamp(ndc.y * 0.5 + 0.5, 0.0, 1.0);
    v = pow(v, 1.0 + 2.0 * sky.horizon_color.w);   // ramp bias
    vec3 col = mix(sky.horizon_color.rgb, sky.sky_color.rgb, v);
    if (sky.sunset_color.w > 0.0) {
        vec3 rd = view_ray();
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
