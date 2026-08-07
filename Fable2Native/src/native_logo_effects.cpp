#include "f2/native_logo_effects.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace f2 {
namespace {

constexpr std::size_t kNumSparkles = 220;
constexpr float kSparkleSize = 24.0f;

constexpr std::array<float, 10> kCapturedSlideLeftNdc = {
    -1.8962f, -1.6970f, -1.3355f, -1.1485f, -1.0475f,
    -0.9409f, -0.8442f, -0.7700f, -0.7247f, -0.7152f,
};
constexpr std::array<float, 4> kCapturedRowBottomNdc = {
    -0.1018f, 0.0605f, 0.2229f, 0.3936f,
};
constexpr std::array<float, 4> kCapturedRowTopNdc = {
    0.0236f, 0.1859f, 0.3483f, 0.5190f,
};
constexpr std::array<float, 3> kCapturedU0 = {0.740f, 0.871f, 0.875f};
constexpr std::array<float, 3> kCapturedU1 = {0.865f, 0.873f, 1.000f};
constexpr float kCapturedFinalLeftNdc = -0.7152f;
constexpr float kCapturedMiddleRightNdc = -0.0746f;
constexpr float kCapturedRightNdc = -0.0371f;

float random_unit(std::uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed & 0x00ffffffu) / 16777215.0f;
}

float triangle_wave(float value) {
    value = std::fmod(value, 2.0f);
    if (value < 0.0f) value += 2.0f;
    return value < 1.0f ? value : 2.0f - value;
}

}  // namespace

void NativeLogoSparkles::draw_ambient(ImDrawList* draw_list,
                                      ImTextureID logo_texture,
                                      float x,
                                      float y,
                                      float width,
                                      float height,
                                      float time,
                                      float opacity) {
    if (!draw_list || !logo_texture || width <= 0.0f || height <= 0.0f || opacity <= 0.0f) {
        return;
    }

    // Keep the approximation's timelines independent: this is the temporary
    // mask presentation, while draw() below remains the sparkle pass. The
    // source PM4 pass is source-alpha blended and its geometry/constants are
    // recovered; these procedural passes remain until the user-owned atlas
    // and material path is available to the native renderer.
    const float reveal_in = std::clamp((time - 0.18f) / 0.58f, 0.0f, 1.0f);
    const float reveal_out = 1.0f - std::clamp((time - 1.45f) / 1.0f, 0.0f, 1.0f);
    const float reveal = reveal_in * reveal_out;
    const float burst_in = std::clamp((time - 0.16f) / 0.30f, 0.0f, 1.0f);
    const float burst_out = 1.0f - std::clamp((time - 0.44f) / 0.92f, 0.0f, 1.0f);
    const float burst = burst_in * burst_out;
    const ImVec2 center(x + width * 0.5f, y + height * 0.5f);

    const auto add_mask = [&](float scale, int alpha) {
        const float scaled_width = width * scale;
        const float scaled_height = height * scale;
        draw_list->AddImage(logo_texture,
                            ImVec2(center.x - scaled_width * 0.5f,
                                   center.y - scaled_height * 0.5f),
                            ImVec2(center.x + scaled_width * 0.5f,
                                   center.y + scaled_height * 0.5f),
                            ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                            IM_COL32(32, 104, 232,
                                     std::clamp(static_cast<int>(alpha * opacity), 0, 255)));
    };

    // These concentric passes are a resolution-independent approximation only;
    // the source shape remains the selected user's logo mask.
    add_mask(1.24f, static_cast<int>(34.0f * burst));
    add_mask(1.14f, static_cast<int>(52.0f * burst));
    add_mask(1.06f, static_cast<int>(74.0f * burst));
    add_mask(1.12f, static_cast<int>(18.0f * reveal));
    add_mask(1.045f, static_cast<int>(34.0f * reveal));
}

void NativeLogoSparkles::draw_captured_atlas_slices(ImDrawList* draw_list,
                                                    ImTextureID atlas_texture,
                                                    float width,
                                                    float height,
                                                    float time,
                                                    float opacity) {
    if (!draw_list || !atlas_texture || width <= 0.0f || height <= 0.0f || opacity <= 0.0f) {
        return;
    }

    // The live sidecar first emitted the 12 slice draws two 60 Hz frames after
    // the 5.90 s title-card boundary, then reached its settled position after
    // eight further intervals. Interpolate the measured keyframes for a
    // deterministic geometry path around the user-owned, pre-composed atlas.
    constexpr float kCapturedStartTime = 5.90f + 2.0f / 60.0f;
    if (time < kCapturedStartTime) {
        return;
    }
    const float sample = (time - kCapturedStartTime) * 60.0f;
    const float clamped_sample = std::clamp(sample, 0.0f,
                                            static_cast<float>(kCapturedSlideLeftNdc.size() - 1));
    const auto lower = static_cast<std::size_t>(std::floor(clamped_sample));
    const auto upper = std::min(lower + 1, kCapturedSlideLeftNdc.size() - 1);
    const float fraction = clamped_sample - static_cast<float>(lower);
    const float left_ndc = std::lerp(kCapturedSlideLeftNdc[lower],
                                     kCapturedSlideLeftNdc[upper], fraction);
    const float shift_ndc = left_ndc - kCapturedFinalLeftNdc;
    const float middle_left_ndc = kCapturedFinalLeftNdc + 0.0375f + shift_ndc;
    const float right_left_ndc = kCapturedMiddleRightNdc + shift_ndc;
    const std::array<float, 3> x0 = {left_ndc, middle_left_ndc, right_left_ndc};
    const std::array<float, 3> x1 = {
        middle_left_ndc,
        right_left_ndc,
        kCapturedRightNdc + shift_ndc,
    };
    const ImU32 color = IM_COL32(255, 255, 255,
                                 std::clamp(static_cast<int>(opacity * 255.0f), 0, 255));
    const auto to_screen_x = [width](float ndc) { return (ndc + 1.0f) * width * 0.5f; };
    const auto to_screen_y = [height](float ndc) { return (1.0f - ndc) * height * 0.5f; };
    for (std::size_t row = 0; row < kCapturedRowBottomNdc.size(); ++row) {
        for (std::size_t column = 0; column < x0.size(); ++column) {
            draw_list->AddImage(
                atlas_texture,
                ImVec2(to_screen_x(x0[column]), to_screen_y(kCapturedRowTopNdc[row])),
                ImVec2(to_screen_x(x1[column]), to_screen_y(kCapturedRowBottomNdc[row])),
                ImVec2(kCapturedU0[column], 0.250f), ImVec2(kCapturedU1[column], 0.500f),
                color);
        }
    }
}

void NativeLogoSparkles::rebuild(const NativeTexture& logo) {
    particles_.clear();
    particles_.reserve(kNumSparkles);
    logo_width_ = logo.width;
    logo_height_ = logo.height;
    seed_ = 0xF2A11CEu;

    if (logo.width == 0 || logo.height == 0 || logo.rgba8.size() <
        static_cast<std::size_t>(logo.width) * logo.height * 4) {
        return;
    }

    for (std::size_t index = 0; index < kNumSparkles; ++index) {
        float sample_x = 0.5f;
        float sample_y = 0.5f;
        for (int attempt = 0; attempt < 64; ++attempt) {
            sample_x = random_unit(seed_);
            sample_y = random_unit(seed_);
            const auto pixel = (static_cast<std::size_t>(sample_y * logo.height) * logo.width +
                                static_cast<std::size_t>(sample_x * logo.width)) * 4;
            if (logo.rgba8[pixel + 3] > 20) break;
        }

        Particle particle;
        particle.x = std::clamp(sample_x + (random_unit(seed_) - 0.5f) * 0.08f, 0.0f, 1.0f);
        // Keep the temporary particles slightly above the sampled mask. Exact
        // sprite placement remains pending native integration of the recovered
        // atlas slice sequence.
        particle.y = std::clamp(sample_y - 0.08f + (random_unit(seed_) - 0.5f) * 0.06f,
                                0.0f, 1.0f);
        particle.delay = random_unit(seed_) * 0.8f;
        particle.life = 0.4f + random_unit(seed_) * 0.4f;
        particle.falls = random_unit(seed_) < 0.7f;
        particle.fall = particle.falls ? 30.0f : 0.0f;
        particle.texture = static_cast<std::uint8_t>(random_unit(seed_) * 8.0f) % 8;
        particles_.push_back(particle);
    }
}

void NativeLogoSparkles::draw(ImDrawList* draw_list,
                              const NativeTexture& logo,
                              const std::array<ImTextureID, 8>& textures,
                              float x,
                              float y,
                              float width,
                              float height,
                              float time,
                              float opacity) {
    if (!draw_list || opacity <= 0.0f) return;
    const float settled_fade = 1.0f - std::clamp((time - 2.35f) / 1.0f, 0.0f, 1.0f);
    opacity *= settled_fade;
    if (opacity <= 0.0f) return;
    if (logo.width != logo_width_ || logo.height != logo_height_ || particles_.empty()) {
        rebuild(logo);
    }
    if (particles_.empty()) return;

    for (const auto& particle : particles_) {
        // The approximation respawns a particle immediately when its life ends;
        // this is not yet a claim about the original sprite scheduler.
        const float phase = std::fmod(time + particle.delay, particle.life);
        if (phase < 0.0f) continue;
        const float life_alpha = triangle_wave(phase / particle.life);
        const float particle_x = x + particle.x * width;
        const float particle_y = y + particle.y * height +
                                  (particle.falls ? (phase / particle.life) * particle.fall : 0.0f);
        const ImTextureID texture = textures[particle.texture];
        if (!texture) continue;

        const float size = kSparkleSize;
        const ImVec2 min(particle_x - size * 0.5f, particle_y - size * 0.5f);
        const ImVec2 max(particle_x + size * 0.5f, particle_y + size * 0.5f);
        const int sparkle_alpha = static_cast<int>(life_alpha * opacity * 0.7f * 255.0f);
        const int glow_alpha = static_cast<int>(life_alpha * opacity * 100.0f);
        draw_list->AddImage(texture, min, max, ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32(255, 255, 255, std::clamp(sparkle_alpha, 0, 255)));
        draw_list->AddImage(texture,
                            ImVec2(particle_x - size * 0.65f, particle_y - size * 0.65f),
                            ImVec2(particle_x + size * 0.65f, particle_y + size * 0.65f),
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32(255, 255, 200, std::clamp(glow_alpha, 0, 255)));
    }
}

}  // namespace f2
