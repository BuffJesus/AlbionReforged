#include "f2/native_logo_effects.h"

#include <algorithm>
#include <cmath>

namespace f2 {
namespace {

constexpr std::size_t kNumSparkles = 400;
constexpr float kSparkleSize = 32.0f;

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
        // This vertical offset is present in the reference sparkle authoring
        // and keeps the particles sitting just above the letter mask.
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
    if (logo.width != logo_width_ || logo.height != logo_height_ || particles_.empty()) {
        rebuild(logo);
    }
    if (particles_.empty()) return;

    for (const auto& particle : particles_) {
        // The reference implementation respawns a particle immediately when
        // its life ends; there is no inactive gap between cycles.
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
