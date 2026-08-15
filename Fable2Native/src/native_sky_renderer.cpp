#include "f2/native_sky_renderer.h"

#include "f2/native_scene_color.h"

#include <d3dcompiler.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

// -----------------------------------------------------------------------------
// Fable II procedural sky (native D3D12 port).
//
// PORT SOURCE (do-not-re-derive): Fable2AssetBrowser SkyDomeXex.{h,cpp} — the retail
// Xenos atmosphere PS + CPU in-scatter LUT builder + fullscreen-triangle VS. RE authority:
// ghidra_out/sky_system_re.txt (the sky is a PROCEDURAL analytic single-scatter atmosphere,
// NOT a mesh/cubemap; drawn on a camera-centred fullscreen pass behind the world).
//
// This module vendors the CPU LUT math (SkyDomeXex::ComputeAtmosphereState /
// BuildInScatterLutFloat) and the fullscreen VS + atmosphere PS as self-contained HLSL.
// The retail SKY_OVERLAY cross-fade and background-map/mist reprojection branch are dropped
// (both gate to a no-op for a first sky — overlay_blend=0, dome_misc.w=0), so this needs no
// sky textures: analytic dome only, wrapped in the AssetBrowser's Reinhard host stand-in.
//
// PHASE: Ships Phase 0 (vertical gradient sky_top<->horizon) as the DEFAULT — a clean blue
// backdrop, a real improvement over the flat sky_color clear. The full Phase 1 analytic
// atmosphere (LUT in-scatter, exp2 transmittance) is also ported into the same PS and can be
// enabled with FABLE2NATIVE_SKY_ATMOSPHERE=1; it renders a real in-scattered sky but its LUT
// colour ramp still reads browner than retail (TODO below), so it is not the default yet.
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

// ---- Vendored constants (SkyDomeXex.h) --------------------------------------
constexpr double kBetaRayleighCommon = 248.05023415027725;
constexpr double kBetaRayleighPowBase = 0.000580084099999878;
constexpr double kBetaRayleighChannel[3] = {
    0.007337032921211229, 0.009459203185216139, 0.025727610716641394};
constexpr float kBetaMieChannel[3] = {0.0056148912f, 0.0063754139f, 0.010514311f};
constexpr float kRayleighMultiplierScale = 500.0f;
constexpr float kRayleighPhaseNorm = 0.059683103f;
constexpr float kMiePhaseNorm = 0.079577468f;
constexpr float kMieG_A0 = 0.10000000149011612f;
constexpr float kMieG_A1 = 0.03180000185966492f;
constexpr float kMieG_B0 = 0.4440000057220459f;
constexpr float kMieG_B1 = 0.05984000116586685f;
constexpr float kOpticalScaleRayleigh = 150.0f;
constexpr float kOpticalScaleMie = 100.0f;
constexpr float kHorizonDistance = 1900.0f;
constexpr float kRecipMaxFogDistance = 0.00050000002f;
constexpr int kLutWidth = 64;

// A resolved theme keyframe. Defaults = the RE'd chapter2slums (childhood) clear-day MIDDAY
// keyframe, parsed byte-exact from environmentthemes.gdb (ghidra_out/env_theme_colors_re.txt §0).
// This replaces the old EnvironmentTheme.h engine placeholders whose complementary {0.72,0.64,0.54}
// rendered a BROWN horizon; retail's horizon tint is the blue complementary below. (Per-level theme
// plumbing through the F2SCENE is the general follow-up; these are the values for the level we cook.)
struct ThemeInputs {
    float sun_intensity = 8.94f;
    float beta_rayleigh_multiplier = 1.0f;
    float beta_mie_multiplier = 0.83f;
    float sky_colour[3] = {0.6549f, 0.8157f, 1.0f};        // zenith blue (raw 167,208,255)
    float complementary_colour[3] = {0.222f, 0.5789f, 1.11f};  // horizon tint (raw 56.6,147.6,283; HDR)
    float complementary_bias = 0.43f;
    float sunset_colour[3] = {0.8902f, 0.8118f, 0.5137f};  // warm sun-halo (raw 227,207,131)
    float fogging_start = 35.43f;
    float close_fog_max_distance = 0.0f;
    float sun_direction[3] = {0.9135f, 0.4067f, 0.0048f};  // sun_toward, Y-up render space (elev 24°)
};

struct AtmosphereState {
    float beta_rayleigh[3] = {};
    float beta_mie[3] = {};
    float scattering_misc[4] = {};
    float lut_beta_rayleigh_phase[3] = {};
    float lut_beta_mie_phase[3] = {};
    float lut_cos_sun_elevation = 1.0f;
    float lut_hg[4] = {};
    float lut_scale = 1.0f;  // == sun_intensity (channels 0..2 are 1.0)
    float lut_colour_a[3] = {};
    float lut_colour_b[3] = {};
    float lut_bias_plus_half = 0.5f;
};

float Dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float VectorAngle(const float* a, const float* b) {
    const float len = std::sqrt(Dot3(a, a)) * std::sqrt(Dot3(b, b));
    if (len <= 0.000099999997f) return 0.0f;
    const float c = Dot3(a, b) / len;
    if (std::fabs(c - 1.0f) < 0.000099999997f) return 0.0f;
    if (std::fabs(c + 1.0f) >= 0.000099999997f) return static_cast<float>(std::acos(c));
    return 3.1415927f;
}

float SunElevationAngle(const float sun_direction[3]) {
    const float negated[3] = {-sun_direction[0], -sun_direction[1], -sun_direction[2]};
    const float horizontal[3] = {negated[0], negated[1], 0.0f};
    return VectorAngle(negated, horizontal);
}

std::uint16_t FloatToHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    std::uint32_t abs_bits = bits & 0x7fffffffu;
    if (abs_bits >= 0x7f800000u) {
        return static_cast<std::uint16_t>(sign | 0x7c00u |
                                          ((abs_bits > 0x7f800000u) ? 0x200u : 0u));
    }
    if (abs_bits >= 0x47800000u) return static_cast<std::uint16_t>(sign | 0x7c00u);
    if (abs_bits < 0x38800000u) {
        const std::uint32_t shift = 126u - (abs_bits >> 23);
        std::uint32_t mantissa = (abs_bits & 0x7fffffu) | 0x800000u;
        if (shift > 24u) return static_cast<std::uint16_t>(sign);
        const std::uint32_t rounded =
            (mantissa >> shift) +
            (((mantissa >> (shift - 1)) & 1u) &
             (((mantissa & ((1u << (shift - 1)) - 1u)) != 0u) | ((mantissa >> shift) & 1u)));
        return static_cast<std::uint16_t>(sign | (rounded >> 13));
    }
    const std::uint32_t half = ((abs_bits >> 13) - 0x1c000u) & 0x7fffu;
    const std::uint32_t remainder = abs_bits & 0x1fffu;
    std::uint32_t rounded = half;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) ++rounded;
    return static_cast<std::uint16_t>(sign | rounded);
}

AtmosphereState ComputeAtmosphereState(const ThemeInputs& theme) {
    AtmosphereState state{};
    const double rayleigh_common =
        std::pow(kBetaRayleighPowBase, 2.0) * kBetaRayleighCommon;
    const float rayleigh_scale = theme.beta_rayleigh_multiplier * kRayleighMultiplierScale;
    const float mie_scale = theme.beta_mie_multiplier;
    for (int c = 0; c < 3; ++c) {
        state.beta_rayleigh[c] =
            static_cast<float>(rayleigh_common * kBetaRayleighChannel[c]) * rayleigh_scale;
        state.beta_mie[c] = kBetaMieChannel[c] * mie_scale;
    }
    state.scattering_misc[0] = theme.fogging_start;
    state.scattering_misc[1] = kHorizonDistance;
    state.scattering_misc[2] =
        theme.close_fog_max_distance > 0.0f ? 1.0f / theme.close_fog_max_distance : 1.0f;
    state.scattering_misc[3] = kRecipMaxFogDistance;
    for (int c = 0; c < 3; ++c) {
        state.lut_beta_rayleigh_phase[c] = state.beta_rayleigh[c] * kRayleighPhaseNorm;
        state.lut_beta_mie_phase[c] = state.beta_mie[c] * kMiePhaseNorm;
    }
    const float elevation = SunElevationAngle(theme.sun_direction);
    state.lut_cos_sun_elevation = static_cast<float>(std::cos(elevation));
    const float f7 = -(elevation * kMieG_A1) + kMieG_A0;
    const float f9 = kMieG_B0 - elevation * kMieG_B1;
    const float f6 = f9 - f7;
    const float g =
        static_cast<float>(static_cast<double>(f6) * 0.5 + static_cast<double>(f7));
    const float g2 = g * g;
    state.lut_hg[0] = static_cast<float>(1.0 - static_cast<double>(g2));
    state.lut_hg[1] = g2 + 1.0f;
    state.lut_hg[2] = g * 2.0f;
    state.lut_hg[3] = 0.0f;
    state.lut_scale = theme.sun_intensity;
    for (int c = 0; c < 3; ++c) {
        state.lut_colour_a[c] = theme.sky_colour[c];
        state.lut_colour_b[c] = theme.complementary_colour[c];
    }
    state.lut_bias_plus_half = theme.complementary_bias + 0.5f;
    return state;
}

void BuildInScatterLut(const AtmosphereState& state,
                       std::array<std::uint16_t, kLutWidth * 4>& out) {
    float recip[3];
    for (int c = 0; c < 3; ++c) recip[c] = 1.0f / (state.beta_rayleigh[c] + state.beta_mie[c]);
    float colour_delta[3];
    for (int c = 0; c < 3; ++c) colour_delta[c] = state.lut_colour_a[c] - state.lut_colour_b[c];
    const float ramp_delta[3] = {0.0f, -0.7f, -0.9f};  // {1,0.3,0.1} - 1
    for (int i = 0; i < kLutWidth; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kLutWidth);
        const float one_plus_t2 = t * t + 1.0f;
        const float hg_denominator = std::pow(state.lut_hg[1] - state.lut_hg[2] * t, 1.5f);
        const float t_cos = t * state.lut_cos_sun_elevation;
        const float hg_ratio = state.lut_hg[0] / hg_denominator;
        const float gated =
            (t * 0.5f + state.lut_bias_plus_half < 0.0f) ? 0.0f : hg_ratio;
        const float colour_factor = gated < 1.0f ? gated : 1.0f;
        for (int c = 0; c < 3; ++c) {
            const float phase = state.lut_beta_mie_phase[c] * hg_ratio +
                                state.lut_beta_rayleigh_phase[c] * one_plus_t2;
            const float ramp = ramp_delta[c] * t_cos + 1.0f;
            const float colour = colour_delta[c] * colour_factor + state.lut_colour_b[c];
            const float scaled = ramp * state.lut_scale;
            out[std::size_t(i) * 4 + c] =
                FloatToHalf(scaled * (phase * recip[c]) * colour);
        }
        out[std::size_t(i) * 4 + 3] = FloatToHalf(0.0f);
    }
}

// GPU-side cbuffer (matches the HLSL below). 16-byte aligned rows.
struct SkyConstants {
    float sun_direction[4]{};      // engine/render space (xyz), normalised
    float beta_rayleigh[4]{};
    float beta_mie[4]{};
    float scattering_misc[4]{};
    float dome_misc[4]{kOpticalScaleRayleigh, kOpticalScaleMie, 1.0f, 0.0f};  // z=HDR scale, w=mode
    float camera_right[4]{};       // w = tan(fov_x/2)
    float camera_up[4]{};          // w = tan(fov_y/2)
    float camera_forward[4]{};
    float sky_colour[4]{};         // for the Phase 0 gradient fallback
    float complementary_colour[4]{};
    float sunset_colour[4]{};       // rgb = theme sunset tint, w = strength (0 = off)
    float theme_params[4]{};        // x=sun_intensity y=bias z=rayleigh(>0 => atmosphere) w=mie
};

// Vertex shader: SkyDomeXex fullscreen triangle. Reconstructs the engine-space ray from the
// camera basis (VSMainFullscreen in SkyDomeXex.cpp). Depth 0.999 (drawn behind everything;
// here depth test/write are OFF so it fills all uncovered pixels).
constexpr char kSkyShaderSource[] = R"(
cbuffer SkyCB : register(b0) {
    float4 sun_direction;
    float4 beta_rayleigh;
    float4 beta_mie;
    float4 scattering_misc;   // y = 1900 horizon distance
    float4 dome_misc;         // x=150 y=100 z=HDR scale w=mode(0 atmosphere / 2 gradient)
    float4 camera_right;      // w = tan(fov_x/2)
    float4 camera_up;         // w = tan(fov_y/2)
    float4 camera_forward;
    float4 sky_colour;
    float4 complementary_colour;
    float4 sunset_colour;     // rgb = sunset tint, w = strength (0 = off)
    float4 theme_params;      // x=sun_intensity y=bias z=rayleigh(>0 => atmosphere) w=mie
}
Texture2D in_scatter_lut : register(t0);   // 64x1 RGBA16F
SamplerState clamp_sampler : register(s0);

struct VSOUT {
    float4 position : SV_Position;
    float3 ray : TEXCOORD0;   // engine-space, unnormalised
    float2 ndc : TEXCOORD1;   // for the gradient fallback
};

VSOUT vs_main(uint id : SV_VertexID) {
    float2 corner;
    if (id == 0)      corner = float2(-1.0, -1.0);
    else if (id == 1) corner = float2(-1.0,  3.0);
    else              corner = float2( 3.0, -1.0);
    VSOUT o;
    o.position = float4(corner, 0.999, 1.0);
    float3 ray = camera_forward.xyz +
                 camera_right.xyz * (corner.x * camera_right.w) +
                 camera_up.xyz    * (corner.y * camera_up.w);
    o.ray = ray * 2850.0;   // magnitude parity with the retail dome interpolant
    o.ndc = corner;
    return o;
}

// Atmosphere PS: faithful port of SkyDomeXex kDomePixelShaderHlsl packets 10..58 + 100,
// with the SKY_OVERLAY cross-fade and background-map branch dropped (both no-ops for a
// first sky). Output wrapped in Reinhard (host stand-in until a tone-map pass exists).
float4 ps_main(VSOUT input) : SV_Target {
    // ---- Analytic atmosphere (retail reference: SkyboxRenderer.cpp PS single scattering) ----
    // Enabled when the cooked theme supplies scattering params (theme_params.z = rayleigh > 0);
    // this is the retail sky. Falls through to the flat gradient stand-in for scenes without it.
    if (dome_misc.w >= 1.5 && theme_params.z > 0.0) {
        float3 ray = normalize(input.ray);
        float3 sun_dir = normalize(sun_direction.xyz);
        float rayleigh = max(theme_params.z, 0.05);
        float mie = max(theme_params.w, 0.05);
        float3 betaR = float3(0.007337, 0.009459, 0.0257276) * rayleigh;
        float3 betaM = float3(0.0056149, 0.0063754, 0.0105143) * mie;
        float cosT = dot(ray, sun_dir);
        float phaseR = 0.059683103 * (1.0 + cosT * cosT);
        float gm = 0.80;
        float hg = 1.0 + gm * gm - 2.0 * gm * cosT;
        float phaseM = 0.079577468 * (1.0 - gm * gm) / max(pow(abs(hg), 1.5), 0.0001);
        float elev = max(ray.y, 0.004);
        float path = 1.0 / (elev + 0.09);
        float3 od = (betaR + betaM) * path * 26.0;
        float3 extinct = exp(-od);
        float3 beta_sum = max(betaR + betaM, 0.00001);
        float3 inscatter = (betaR * phaseR + betaM * phaseM) / beta_sum * (1.0 - extinct);
        float sun_h = saturate(sun_dir.y * 2.2 + 0.12);
        float3 col = inscatter * (7.2 * sun_h) * sky_colour.rgb;
        float horizonf = 1.0 - saturate(elev * 3.2);
        float bias = saturate(theme_params.y);
        col = lerp(col, complementary_colour.rgb * (0.35 + 0.65 * sun_h),
                   horizonf * (0.55 + 0.30 * bias));
        float sunset_w = saturate(1.0 - abs(sun_dir.y) * 5.0) * saturate(cosT * 0.5 + 0.5);
        col = lerp(col, sunset_colour.rgb * (0.4 + 0.8 * phaseM),
                   sunset_w * 0.45 * sunset_colour.w);
        float night = saturate((-sun_dir.y + 0.05) / 0.45);
        col = lerp(col, col * 0.22 + float3(0.010, 0.018, 0.050), night);
        float hh = saturate(ray.y * 0.5 + 0.5);
        col += sky_colour.rgb * (1.0 - hh) * 0.05;
        return float4(col, 1.0);
    }
    // ---- Phase 0 gradient fallback (dome_misc.w >= 1.5) ----
    if (dome_misc.w >= 1.5) {
        float v = saturate(input.ndc.y * 0.5 + 0.5);   // 0 = bottom, 1 = top
        // Ramp bias (theme complementary_bias in complementary_colour.w): higher bias raises
        // the exponent so the horizon tint extends further up. 0 = the old linear ramp.
        v = pow(v, 1.0 + 2.0 * complementary_colour.w);
        float3 g = lerp(complementary_colour.rgb, sky_colour.rgb, v);
        // Sunset halo (SkyboxRenderer.cpp:170-174): a Mie-forward-lobe warm tint toward the
        // sun, active ONLY when the sun is near the horizon (dawn/dusk). Gated by
        // sunset_colour.w (0 when no `sky_sunset` opcode) so the default look is unchanged.
        if (sunset_colour.w > 0.0) {
            float3 rd = normalize(input.ray);
            float3 sd = normalize(sun_direction.xyz);     // toward the sun
            float cosT = dot(rd, sd);
            float gm = 0.80;
            float hg = 1.0 + gm * gm - 2.0 * gm * cosT;
            float phaseM = 0.079577468 * (1.0 - gm * gm) / max(pow(abs(hg), 1.5), 0.0001);
            float sunset_w = saturate(1.0 - abs(sd.y) * 5.0) * saturate(cosT * 0.5 + 0.5);
            g = lerp(g, sunset_colour.rgb * (0.4 + 0.8 * phaseM),
                     sunset_w * 0.45 * sunset_colour.w);
        }
        return float4(g, 1.0);
    }

    // ---- Phase 1 analytic atmosphere ----
    // Port of SkyDomeXex kDomePixelShaderHlsl's dominant visible terms (packets 15,40,
    // 48..56): normalise the ray, key the 64x1 in-scatter LUT by dot(ray,sun), build a
    // horizon-length optical depth, and composite (1-transmittance)*LUT. The retail
    // screen-relative angular optical-depth (p13..p48) is condensed to a view-elevation
    // path-length term; the LUT already carries the Rayleigh+Mie phase colour ramp.
    float3 ray = input.ray;
    float inv_len = rsqrt(max(dot(ray, ray), 1e-8));
    float3 n = ray * inv_len;                          // normalised ray
    float u = saturate(dot(sun_direction.xyz, n));     // LUT coord (p15 dp3_sat)
    float3 lut = in_scatter_lut.SampleLevel(clamp_sampler, float2(u, 0.5), 0).xyz;

    // Air-mass style path length: ~1 looking up, growing toward the horizon (n.y -> 0).
    // Scaled by kHorizonDistance so beta * path lands in a sensible exp2 range.
    float up_amount = saturate(n.y);
    float air_mass = 1.0 / max(up_amount + 0.15, 0.15);   // 1 (zenith) .. ~6.7 (horizon)
    float path = air_mass * scattering_misc.y * 0.02;     // scattering_misc.y = 1900
    float tau_r = path * (dome_misc.x / 150.0);           // dome_misc.x = 150
    float tau_m = path * (dome_misc.y / 100.0);           // dome_misc.y = 100

    float3 transmittance = exp2(-(tau_r * beta_rayleigh.xyz + tau_m * beta_mie.xyz) * 1.4427);
    float3 sky = (1.0 - transmittance) * lut;          // in-scatter (p55..p56)

    // Below the horizon fades to the warm complementary tint (a ground haze stand-in until
    // the fog pass exists), so the lower hemisphere is not pure black.
    float below = saturate(-n.y * 4.0);
    sky = lerp(sky, complementary_colour.rgb * 0.6, below);

    float3 hdr = sky * dome_misc.z;
    return float4(hdr / (1.0 + hdr), 1.0);             // Reinhard host stand-in
}
)";

D3D12_HEAP_PROPERTIES upload_heap() {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = D3D12_HEAP_TYPE_UPLOAD;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_DESC buffer_desc(std::size_t size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

std::array<float, 3> normalise3(std::array<float, 3> v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l <= 1e-6f) return {0.0f, 0.4f, 1.0f};
    return {v[0] / l, v[1] / l, v[2] / l};
}

}  // namespace

bool NativeSkyRenderer::build_lut_texture(ID3D12Device* device, ID3D12CommandQueue* queue,
                                          std::string& error) {
    // The LUT depends on the theme; for a static first sky we bake ONE keyframe (engine
    // defaults + a mid-day sun) at init. A later day/night pass can rebuild it per frame.
    ThemeInputs theme{};
    const AtmosphereState state = ComputeAtmosphereState(theme);
    std::array<std::uint16_t, kLutWidth * 4> texels{};
    BuildInScatterLut(state, texels);

    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = kLutWidth;
    tex.Height = 1;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    tex.SampleDesc.Count = 1;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    auto default_heap = upload_heap();
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&lut_texture_)))) {
        error = "D3D12 could not allocate the sky in-scatter LUT.";
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0;
    UINT64 row_size = 0, upload_size = 0;
    device->GetCopyableFootprints(&tex, 0, 1, 0, &footprint, &row_count, &row_size, &upload_size);
    const auto up_desc = buffer_desc(upload_size);
    const auto up_heap = upload_heap();
    if (FAILED(device->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&lut_upload_)))) {
        error = "D3D12 could not allocate the sky LUT upload buffer.";
        return false;
    }
    void* mapped = nullptr;
    if (FAILED(lut_upload_->Map(0, nullptr, &mapped))) {
        error = "D3D12 could not map the sky LUT upload buffer.";
        return false;
    }
    std::memcpy(static_cast<std::uint8_t*>(mapped) + footprint.Offset, texels.data(),
                texels.size() * sizeof(std::uint16_t));
    lut_upload_->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&cmd))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        error = "D3D12 could not create the sky LUT upload commands.";
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = lut_texture_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = lut_upload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = lut_texture_.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &barrier);
    if (FAILED(cmd->Close())) {
        error = "D3D12 could not close the sky LUT upload commands.";
        return false;
    }
    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1))) {
        error = "D3D12 could not submit the sky LUT upload.";
        return false;
    }
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        error = "D3D12 could not create the sky LUT upload event.";
        return false;
    }
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
    }
    CloseHandle(event);

    // Own descriptor heap for the LUT SRV (keeps the app's shared heap layout untouched).
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = 1;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap_)))) {
        error = "D3D12 could not create the sky descriptor heap.";
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(lut_texture_.Get(), &srv,
                                     srv_heap_->GetCPUDescriptorHandleForHeapStart());
    lut_gpu_handle_ = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    return true;
}

bool NativeSkyRenderer::initialise(ID3D12Device* device, ID3D12CommandQueue* queue,
                                   std::string& error) {
    if (!build_lut_texture(device, queue, error)) return false;

    // Constant buffer (mapped upload).
    const auto cb_size = (sizeof(SkyConstants) + 255u) & ~255u;
    const auto heap = upload_heap();
    const auto desc = buffer_desc(cb_size);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&constant_buffer_))) ||
        FAILED(constant_buffer_->Map(0, nullptr, &mapped_constants_))) {
        error = "D3D12 could not allocate the sky constant buffer.";
        return false;
    }
    constant_address_ = constant_buffer_->GetGPUVirtualAddress();

    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
    const auto compile = [&](const char* entry, const char* target,
                             Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
        return D3DCompile(kSkyShaderSource, sizeof(kSkyShaderSource) - 1, "native_sky.hlsl",
                          nullptr, nullptr, entry, target, 0, 0, &blob, &errors);
    };
    if (FAILED(compile("vs_main", "vs_5_0", vs)) || FAILED(compile("ps_main", "ps_5_0", ps))) {
        error = "The native sky shaders could not be compiled.";
        return false;
    }

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = params;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3DBlob> root_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
                                           &errors)) ||
        FAILED(device->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                           root_blob->GetBufferSize(),
                                           IID_PPV_ARGS(&root_signature_)))) {
        error = "The native sky root signature could not be created.";
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {nullptr, 0};  // fullscreen triangle from SV_VertexID
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kSceneColorFormat;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = 0xFFFFFFFFu;
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = FALSE;
    pso.RasterizerState = raster;
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState = blend;
    // No depth test and no depth write: the sky fills the whole screen first; world geometry
    // (depth-write ON) draws over it afterwards.
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;  // matches the bound DSV; ignored with DepthEnable=FALSE
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_state_)))) {
        error = "The native sky pipeline could not be created.";
        return false;
    }
    return true;
}

void NativeSkyRenderer::render(ID3D12GraphicsCommandList* command_list,
                               const NativeScene& scene, const SkyCamera& camera,
                               std::uint32_t width, std::uint32_t height, double) {
    if (!pipeline_state_ || width == 0 || height == 0) return;

    // Resolve a theme keyframe from the scene. Sun direction comes from the scene (world Y-up).
    ThemeInputs theme{};
    const auto sun = normalise3({-scene.sun_direction[0], -scene.sun_direction[1],
                                 -scene.sun_direction[2]});  // scene stores light-travel dir
    for (int i = 0; i < 3; ++i) theme.sun_direction[i] = sun[i];
    const AtmosphereState state = ComputeAtmosphereState(theme);

    SkyConstants c{};
    for (int i = 0; i < 3; ++i) c.sun_direction[i] = theme.sun_direction[i];
    for (int i = 0; i < 3; ++i) {
        c.beta_rayleigh[i] = state.beta_rayleigh[i];
        c.beta_mie[i] = state.beta_mie[i];
    }
    std::memcpy(c.scattering_misc, state.scattering_misc, sizeof(c.scattering_misc));
    c.dome_misc[0] = kOpticalScaleRayleigh;
    c.dome_misc[1] = kOpticalScaleMie;
    c.dome_misc[2] = 1.0f;  // HDR scale
    // Sky mode. Phase 0 (gradient, mode 2) ships as the default: it produces a clean,
    // unambiguously sky-like blue-to-horizon backdrop. The full Phase 1 analytic atmosphere
    // (mode 0) is fully ported and renders a real in-scattered sky, but its LUT colour ramp
    // still reads browner than the retail blue (TODO: match SkyDomeXex's exact per-packet
    // optical-depth term / verify complementary_colour bias). Flip to 0.0f to preview it, or
    // env override FABLE2NATIVE_SKY_ATMOSPHERE=1.
    static const bool atmosphere = [] {
        char buf[8]{};
        return GetEnvironmentVariableA("FABLE2NATIVE_SKY_ATMOSPHERE", buf, sizeof(buf)) > 0 &&
               buf[0] == '1';
    }();
    c.dome_misc[3] = atmosphere ? 0.0f : 2.0f;
    for (int i = 0; i < 3; ++i) {
        c.camera_right[i] = camera.right[i];
        c.camera_up[i] = camera.up[i];
        c.camera_forward[i] = camera.forward[i];
    }
    c.camera_right[3] = camera.tan_half_fov_x;
    c.camera_up[3] = camera.tan_half_fov_y;
    for (int i = 0; i < 3; ++i) {
        c.sky_colour[i] = scene.sky_color[i];
        // Gradient horizon = the scene's (theme) horizon tint; defaults to the RE'd
        // hardcoded value so scenes without a `sky_horizon` opcode are unchanged. NOTE:
        // the opt-in Phase-1 atmosphere PS (FABLE2NATIVE_SKY_ATMOSPHERE=1) also reads this
        // field for its below-horizon haze; its prebuilt LUT still uses the hardcoded
        // ThemeInputs value, so a cooked horizon only fully reaches the default gradient path.
        c.complementary_colour[i] = scene.sky_horizon_color[i];
        c.sunset_colour[i] = scene.sky_sunset_color[i];
    }
    c.sunset_colour[3] = scene.has_sky_sunset ? 1.0f : 0.0f;  // strength gate
    c.complementary_colour[3] = scene.sky_bias;               // gradient ramp bias (0 = linear)
    c.theme_params[0] = scene.sky_sun_intensity;
    c.theme_params[1] = scene.sky_bias;
    c.theme_params[2] = scene.sky_rayleigh;  // > 0 selects the analytic atmosphere
    c.theme_params[3] = scene.sky_mie;
    std::memcpy(mapped_constants_, &c, sizeof(c));

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width),
                                  static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetPipelineState(pipeline_state_.Get());
    command_list->SetGraphicsRootSignature(root_signature_.Get());
    command_list->SetGraphicsRootConstantBufferView(0, constant_address_);
    command_list->SetGraphicsRootDescriptorTable(1, lut_gpu_handle_);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 0, nullptr);
    command_list->IASetIndexBuffer(nullptr);
    command_list->DrawInstanced(3, 1, 0, 0);
}

}  // namespace f2
