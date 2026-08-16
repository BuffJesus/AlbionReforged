#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f2 {

struct NativeVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
    std::array<float, 2> uv{};
};

struct NativeMaterial {
    std::string name;
    std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
    std::string albedo;
    std::string normal;
    std::string material;
    // Authored WaterFile::params[37] (water_system_re.txt), retained per material instead of
    // baking chapter2slums values into either backend shader. water_opacity is the resolved
    // WaterTheme value, separate because it is not part of the .water body parameter array;
    // retail program 57 carries it in its water constant block but does not consume it directly.
    bool has_water_params = false;
    // Retail defaults keep older F2SCENE packages (which predate water_params=) valid. An
    // authored .water body overwrites this array during cooking/loading.
    std::array<float, 37> water_params{
        0.20f, 0.0f, 0.052f, 0.011f, -0.019f, 0.019f, 0.188f, 0.188f,
        0.220f, 0.220f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.1275f, 0.1913f, 0.370f, 0.470f, 0.750f, 0.05f, 2.0f,
        2.0f, 2.0f, 2.0f, 0.75f, 0.10f, 0.15f, 0.10f, 2.0f, 0.30f,
        5.0f, 128.0f};
    float water_opacity = 0.42f;
};

struct NativeMesh {
    std::string name;
    std::vector<NativeVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t material = 0;
};

struct NativeInstance {
    std::uint32_t mesh = 0;
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
    float scale = 1.0f;
    // Optional per-instance baked order-1 SH lighting probe (.lmp LightmapFile), 12 coeffs
    // channel-major [R0 R1 R2 R3][G0..][B0..]. The renderer evaluates amb = C0 + N.(C1,C2,C3)
    // per channel against the object-space normal (the exact game shader eval). has_probe=false
    // -> the renderer falls back to the global hemisphere ambient.
    bool has_probe = false;
    std::array<float, 12> sh{};
    // Per-instance max draw distance in world units (from the entity's DrawDistance GDB field, set
    // by the cook). 0 = never cull (default; existing scenes unchanged). The world renderers skip
    // this instance's draw range when its bounding sphere lies beyond this distance from the camera
    // — the retail draw-distance LOD gate. See fable2-npc-popin-drawdistance-fix.
    float max_draw_distance = 0.0f;
};

// A local point light (lamp post, lantern, brazier, placeable accent) — cooked from
// the level's .save/.gdb light entities (ghidra_out/level_lights_effects_re.txt §1).
// position = render space (game {x,z,y}); color = linear-ish RGB (source 0..255 / 255);
// range = falloff radius in world units; intensity = brightness multiplier.
struct NativeLight {
    std::array<float, 3> position{};
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    float range = 0.0f;
    float intensity = 1.0f;
};

// A scrolling cloud layer resolved from the theme Clouds record (SkyboxRenderer.cpp cloud pass).
// Drawn as one flat quad at `height` (render-space Y), spanning ±size in render X/Z and centred at
// the world origin, alpha-blended behind the world. `density_map` is a cooked DDS whose .a is the
// density mask and .rgb the cloud tint. Velocity is the authored scroll speed (the renderer applies
// the retail *0.001 velocity scale + ShaderNormalStrength(normal_strength)). See cook_levels.py
// `cloud_layer` emit. An empty clouds vector -> no cloud pass, so non-cloud scenes are unchanged.
struct NativeCloudLayer {
    std::string density_map;
    float height = 0.0f;
    float size_x = 0.0f;
    float size_y = 0.0f;
    float texture_scale_x = 0.001f;
    float texture_scale_y = 0.001f;
    float velocity_x = 0.0f;
    float velocity_y = 0.0f;
    float transparency = 0.0f;   // layer_params.x  (alpha multiplier)
    float brightness = 1.0f;     // layer_params.z  (colour scale)
    float ambient = 0.0f;        // layer_params.y  (ambient add)
    float normal_strength = 0.0f;  // authored; runtime -> ShaderNormalStrength -> layer_params.w
};

// Celestial moon billboard (theme Sky element pass; SkyboxRenderer.cpp draw_billboard). Present
// only when the theme authors a moon (moon_intensity>0 → night). `texture` = the MoonPhases DDS
// (an 8-phase horizontal strip), `glare_texture` = the moon-glare DDS (additive halo). `direction`
// = render-space toward the moon; the billboard is a camera-facing quad drawn after the clouds and
// behind the world. has_moon=false → no moon pass, so daytime scenes are unchanged.
struct NativeMoon {
    std::string texture;
    std::string glare_texture;
    std::array<float, 3> direction{0.0f, 1.0f, 0.0f};
    float intensity = 0.0f;
    float size = 1.0f;
    float transparency = 0.0f;
    float glare_intensity = 0.0f;
    float glare_size = 1.0f;
    float exposure = 10.0f;  // retail dome HDR scale (dome_misc.z); billboard colour is tonemapped
    int phase = 0;           // 0..7 lunar-cycle phase = the horizontal cell of the 8-phase strip
};

struct NativeScene {
    std::vector<NativeMaterial> materials;
    std::vector<NativeMesh> meshes;
    std::vector<NativeInstance> instances;
    std::vector<NativeLight> lights;
    // Celestial moon (theme Sky element pass). Present only at night.
    bool has_moon = false;
    NativeMoon moon;
    float star_brightness = 0.0f;  // carried for a future procedural star field (not yet rendered)
    // Cloud layers (theme Clouds). Empty by default so scenes without a `cloud_layer` opcode
    // render exactly as before. cloud_global.x = global brightness, .z = alpha-test reference.
    std::vector<NativeCloudLayer> clouds;
    float cloud_global_brightness = 1.0f;
    float cloud_alpha_ref = 0.019608f;
    std::array<float, 3> sun_direction{0.3f, -1.0f, 0.2f};
    // Directional sun colour (theme main_light_colour). Default warm white; the world PS
    // tints the N.L sun term with it. env_theme_colors_re.txt: chapter2slums = (1.0,0.902,0.435).
    std::array<float, 3> sun_color{1.0f, 1.0f, 1.0f};
    std::array<float, 4> sky_color{0.35f, 0.48f, 0.68f, 1.0f};
    // Sky-gradient horizon (bottom) tint = theme complementary_colour, display-mapped at
    // cook time. Default = the RE'd chapter2slums hardcoded horizon so scenes without a
    // `sky_horizon` opcode render exactly as before. Fed to the sky pass as the v=0 end of
    // the gradient lerp on both backends. See cook_levels.py `sky_horizon` emit.
    std::array<float, 3> sky_horizon_color{0.222f, 0.5789f, 1.11f};
    // Sky-gradient sunset tint = theme sunset_colour. A warm Mie-forward-lobe halo added
    // toward the sun ONLY when the sun is near the horizon (dawn/dusk). Gated by
    // has_sky_sunset so scenes without a `sky_sunset` opcode render exactly as before
    // (the sunset term contributes nothing). See cook_levels.py `sky_sunset` emit.
    std::array<float, 3> sky_sunset_color{0.0f, 0.0f, 0.0f};
    bool has_sky_sunset = false;
    // Sky-gradient ramp bias = theme complementary_bias (0..1). Reshapes the horizon->zenith
    // ramp so the horizon tint extends further up at higher bias. 0 = the old linear ramp, so
    // scenes without a `sky_bias` opcode are unchanged. See cook_levels.py `sky_bias` emit.
    float sky_bias = 0.0f;
    // Analytic-atmosphere params (SkyboxRenderer PS). sky_rayleigh > 0 switches the sky from the
    // flat gradient stand-in to the retail single-scattering atmosphere; 0 (default) keeps the
    // gradient, so scenes without a `sky_atmos` opcode are unchanged.
    float sky_sun_intensity = 0.0f;
    float sky_rayleigh = 0.0f;
    float sky_mie = 0.83f;
    // Distance fog on world geometry (theme fogging; env_theme_colors_re.txt §6 "ties world to
    // horizon"). Linear from fog_start to fog_end reaching fog_max toward fog_color. fog_max = 0
    // (default) disables it, so scenes without `fog_color`/`fog_range` are unchanged.
    std::array<float, 3> fog_color{0.0f, 0.0f, 0.0f};
    float fog_start = 0.0f;
    float fog_end = 1.0f;
    float fog_max = 0.0f;
    // Optional camera-fit override (render space) cooked over the town geometry only, so
    // horizon backdrop props (the Tattered Spire vista at ~1000wu) don't blow up the auto-fit.
    // When has_focus, the world renderers frame focus_center/focus_radius instead of the full
    // vertex AABB. See cook_levels.py `focus` emit.
    bool has_focus = false;
    std::array<float, 3> focus_center{0.0f, 0.0f, 0.0f};
    float focus_radius = 1.0f;
    // Optional render-space PlayerStart for hero inspection scenes. This is separate from `focus`
    // because a town-wide camera fit and a close hero inspection view serve different purposes.
    bool has_hero_start = false;
    std::array<float, 3> hero_start{0.0f, 0.0f, 0.0f};
    float hero_yaw = 0.0f;

    bool validate(std::string* error = nullptr) const;
};

bool load_native_scene(const std::filesystem::path& path,
                       NativeScene& scene,
                       std::string& error);

// Write a NativeScene as a text F2SCENE package (the output stage of the level cooker; the exact
// format load_native_scene reads). Names (material/mesh) must be whitespace-free — the reader tokenizes
// them with >>. Returns false + fills error on an invalid scene or write failure.
bool save_native_scene(const std::filesystem::path& path,
                       const NativeScene& scene,
                       std::string& error);

}  // namespace f2
