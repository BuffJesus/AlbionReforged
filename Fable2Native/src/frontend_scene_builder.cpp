#include "f2/frontend_scene_builder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace f2 {
namespace {

std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

std::uint32_t alpha_byte(float value) {
    return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}

}  // namespace

FrontendSceneBuilder::FrontendSceneBuilder(const NativeFont& font, const NativeUiAssets& assets,
                                           TextureAccess access)
    : font_(font), assets_(assets), access_(std::move(access)) {}

NativeUiAsset FrontendSceneBuilder::sparkle_asset(std::uint8_t index) {
    constexpr std::array<NativeUiAsset, 8> kSparkles = {
        NativeUiAsset::Sparkle1, NativeUiAsset::Sparkle2, NativeUiAsset::Sparkle3,
        NativeUiAsset::Sparkle4, NativeUiAsset::Sparkle5, NativeUiAsset::Sparkle6,
        NativeUiAsset::Sparkle7, NativeUiAsset::Sparkle8};
    return kSparkles[index % kSparkles.size()];
}

void FrontendSceneBuilder::emit_text(f2::render::UiDrawList& scene, std::string_view text, float x,
                                     float y, float size, std::uint32_t color) const {
    if (!access_.font_ready || !access_.font_ready() || !font_.ready() ||
        font_.pixel_height() <= 0.0f) {
        return;
    }
    const auto font_id = access_.font_id ? access_.font_id() : f2::render::kInvalidTexture;
    if (!font_id) return;
    const float scale = size / font_.pixel_height();
    float cursor = x;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        std::uint32_t codepoint = '?';
        std::size_t length = 1;
        if (first < 0x80) {
            codepoint = first;
        } else if ((first & 0xe0) == 0xc0 && offset + 1 < text.size()) {
            codepoint = ((first & 0x1f) << 6) | (static_cast<unsigned char>(text[offset + 1]) & 0x3f);
            length = 2;
        } else if ((first & 0xf0) == 0xe0 && offset + 2 < text.size()) {
            codepoint = ((first & 0x0f) << 12) |
                        ((static_cast<unsigned char>(text[offset + 1]) & 0x3f) << 6) |
                        (static_cast<unsigned char>(text[offset + 2]) & 0x3f);
            length = 3;
        } else if ((first & 0xf8) == 0xf0 && offset + 3 < text.size()) {
            codepoint = ((first & 0x07) << 18) |
                        ((static_cast<unsigned char>(text[offset + 1]) & 0x3f) << 12) |
                        ((static_cast<unsigned char>(text[offset + 2]) & 0x3f) << 6) |
                        (static_cast<unsigned char>(text[offset + 3]) & 0x3f);
            length = 4;
        }
        offset += length;
        const auto& glyph = font_.glyph(codepoint);
        if (!glyph.valid) continue;
        if (glyph.u1 > glyph.u0 && glyph.v1 > glyph.v0) {  // has a non-empty sprite
            scene.add_sprite(font_id, cursor + glyph.x0 * scale, y + glyph.y0 * scale,
                             cursor + glyph.x1 * scale, y + glyph.y1 * scale, glyph.u0, glyph.v0,
                             glyph.u1, glyph.v1, color);
        }
        cursor += glyph.advance * scale;
    }
}

void FrontendSceneBuilder::build_loading(f2::render::UiDrawList& scene, float width,
                                         float height) const {
    const char* text = "Loading native world...";
    const float size = 26.0f * (width / 1280.0f);
    emit_text(scene, text, (width - text_width(text, size)) * 0.5f, height * 0.5f - size * 0.5f, size,
              0xffffffffu);
}

void FrontendSceneBuilder::ensure_title_sparkles() {
    if (!title_sparkles_.empty()) return;
    constexpr std::size_t kCount = 160;
    title_sparkles_.reserve(kCount);
    const NativeTexture* logo = assets_.texture(NativeUiAsset::Logo);
    const bool have_mask = logo && logo->width > 0 && logo->height > 0 &&
                           logo->rgba8.size() >=
                               static_cast<std::size_t>(logo->width) * logo->height * 4;
    std::uint32_t seed = 0xF2A11CEu;
    const auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed & 0x00ffffffu) / 16777215.0f;
    };
    for (std::size_t i = 0; i < kCount; ++i) {
        float sx = rnd();
        float sy = rnd();
        if (have_mask) {
            for (int attempt = 0; attempt < 48; ++attempt) {
                sx = rnd();
                sy = rnd();
                const auto px = (static_cast<std::size_t>(sy * logo->height) * logo->width +
                                 static_cast<std::size_t>(sx * logo->width)) *
                                4;
                if (px + 3 < logo->rgba8.size() && logo->rgba8[px + 3] > 24) break;
            }
        }
        TitleSparkle s;
        s.x = std::clamp(sx + (rnd() - 0.5f) * 0.05f, 0.0f, 1.0f);
        s.y = std::clamp(sy + (rnd() - 0.5f) * 0.05f, 0.0f, 1.0f);
        s.delay = rnd();
        s.life = 0.8f + rnd() * 1.4f;
        s.size = 8.0f + rnd() * 12.0f;
        s.texture = static_cast<std::uint8_t>(rnd() * 8.0f);
        title_sparkles_.push_back(s);
    }
}

void FrontendSceneBuilder::build_title(f2::render::UiDrawList& scene, float width, float height,
                                       double state_time, std::string_view accept_prompt,
                                       bool using_controller_prompts) {
    const auto add = [&](NativeUiAsset asset, float x0, float y0, float x1, float y1, float u0,
                         float v0, float u1, float v1, std::uint32_t color) {
        const auto tid = access_.id(asset);
        if (!tid) return;
        scene.add_sprite(tid, x0, y0, x1, y1, u0, v0, u1, v1, color);
    };

    const float title_time = static_cast<float>(state_time);
    if (const NativeTexture* background = assets_.texture(NativeUiAsset::TitleBackground);
        background && access_.id(NativeUiAsset::TitleBackground)) {
        const float fade = std::clamp((title_time - 5.90f) / 1.10f, 0.0f, 1.0f);
        const float scale = height / static_cast<float>(background->height);
        const float image_width = background->width * scale;
        const float offset = std::fmod(title_time * 29.0f, image_width);
        const auto background_id = access_.id(NativeUiAsset::TitleBackground);
        for (float x = -offset; x < width; x += image_width) {
            scene.add_sprite(background_id, x, 0.0f, x + image_width, height, 0.0f, 0.0f, 1.0f, 1.0f,
                             rgba(255, 255, 255, alpha_byte(fade)));
        }
    }

    const float logo_fade = std::clamp((title_time - 0.86f) / 0.75f, 0.0f, 1.0f);
    const NativeTexture* logo = assets_.texture(NativeUiAsset::Logo);
    if (logo && access_.id(NativeUiAsset::Logo) && logo_fade > 0.0f) {
        const float logo_scale = width / (logo->width * 1.8f);
        const float logo_width = logo->width * logo_scale;
        const float logo_height = logo->height * logo_scale;
        const float logo_x = (width - logo_width) * 0.5f;
        const float logo_y = height * 0.47f - logo_height * 0.5f;
        add(NativeUiAsset::Logo, logo_x, logo_y, logo_x + logo_width, logo_y + logo_height, 0.0f,
            0.0f, 1.0f, 1.0f, rgba(255, 255, 255, alpha_byte(logo_fade)));

        // Continuous title sparkle burst over the wordmark (retail's GPU particle system): white
        // star/mist sprites, additive so cores blow to white, seeded from the logo mask. Never
        // one-shots to nothing (user: "the sparkles keep going … essentially white").
        ensure_title_sparkles();
        const float spark_appear = std::clamp((title_time - 0.86f) / 0.9f, 0.0f, 1.0f);
        const float unit = width / 1280.0f;
        for (const auto& s : title_sparkles_) {
            const auto sparkle_id = access_.id(sparkle_asset(s.texture));
            if (!sparkle_id) continue;
            const float phase = std::fmod(title_time * 0.9f + s.delay * s.life, s.life);
            const float t = phase / s.life;
            const float life_alpha = t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
            const float a = life_alpha * spark_appear;
            if (a <= 0.02f) continue;
            const float px = logo_x + s.x * logo_width;
            const float py = logo_y + s.y * logo_height;
            const float sz = s.size * unit;
            // Sparkle sprites are pink stars; alpha_mask takes only their shape (alpha) and paints it
            // white so the burst reads "essentially white" like retail, with a faint cool-white halo.
            scene.add_sprite(sparkle_id, px - sz * 0.95f, py - sz * 0.95f, px + sz * 0.95f,
                             py + sz * 0.95f, 0.0f, 0.0f, 1.0f, 1.0f,
                             rgba(210, 225, 255, alpha_byte(a * 0.40f)));
            scene.set_last_blend(f2::render::BlendMode::Additive);
            scene.set_last_alpha_mask(true);
            scene.add_sprite(sparkle_id, px - sz * 0.5f, py - sz * 0.5f, px + sz * 0.5f,
                             py + sz * 0.5f, 0.0f, 0.0f, 1.0f, 1.0f,
                             rgba(255, 255, 255, alpha_byte(a * 1.0f)));
            scene.set_last_blend(f2::render::BlendMode::Additive);
            scene.set_last_alpha_mask(true);
        }
    }

    if (access_.id(NativeUiAsset::AmbientBaseline) && title_time >= 5.90f) {
        add(NativeUiAsset::AmbientBaseline, 0.0f, 0.0f, width, height, 0.0f, 0.0f, 1.0f, 1.0f,
            rgba(255, 255, 255, 255));
    }
    if (access_.shader_ready(NativeUiAsset::AmbientAtlas) && title_time >= 5.90f + 2.0f / 60.0f) {
        // The native equivalent of the recovered 12-draw sidecar block; frame-relative interpolation
        // kept identical to the Vulkan/ImGui path so both backends share one timing interpretation.
        constexpr std::array<float, 10> slide_left_ndc = {
            -1.8962f, -1.6970f, -1.3355f, -1.1485f, -1.0475f,
            -0.9409f, -0.8442f, -0.7700f, -0.7247f, -0.7152f,
        };
        constexpr std::array<float, 4> row_bottom_ndc = {-0.1018f, 0.0605f, 0.2229f, 0.3936f};
        constexpr std::array<float, 4> row_top_ndc = {0.0236f, 0.1859f, 0.3483f, 0.5190f};
        constexpr std::array<float, 3> u0 = {0.740f, 0.871f, 0.875f};
        constexpr std::array<float, 3> u1 = {0.865f, 0.873f, 1.000f};
        constexpr float start_time = 5.90f + 2.0f / 60.0f;
        constexpr float final_left = -0.7152f;
        constexpr float middle_right = -0.0746f;
        constexpr float right_edge = -0.0371f;
        if (title_time >= start_time) {
            const float sample = std::clamp((title_time - start_time) * 60.0f, 0.0f,
                                            static_cast<float>(slide_left_ndc.size() - 1));
            const auto lower = static_cast<std::size_t>(std::floor(sample));
            const auto upper = std::min(lower + 1, slide_left_ndc.size() - 1);
            const float fraction = sample - static_cast<float>(lower);
            const float left_ndc = std::lerp(slide_left_ndc[lower], slide_left_ndc[upper], fraction);
            const float shift_ndc = left_ndc - final_left;
            const float middle_left = final_left + 0.0375f + shift_ndc;
            const float right_left = middle_right + shift_ndc;
            const std::array<float, 3> x0 = {left_ndc, middle_left, right_left};
            const std::array<float, 3> x1 = {middle_left, right_left, right_edge + shift_ndc};
            const auto to_x = [width](float ndc) { return (ndc + 1.0f) * width * 0.5f; };
            const auto to_y = [height](float ndc) { return (1.0f - ndc) * height * 0.5f; };
            for (std::size_t row = 0; row < row_bottom_ndc.size(); ++row) {
                for (std::size_t column = 0; column < x0.size(); ++column) {
                    add(NativeUiAsset::AmbientAtlas, to_x(x0[column]), to_y(row_top_ndc[row]),
                        to_x(x1[column]), to_y(row_bottom_ndc[row]), u0[column], 0.250f, u1[column],
                        0.500f, rgba(255, 255, 255, 255));
                }
            }
        }
    }

    // Native prompt + legal text. emit_text positions by the text top-left, matching the retired
    // ImGui::AddText calls one-for-one.
    {
        const std::string prompt_text = "Press " + std::string(accept_prompt) + " to start";
        constexpr float prompt_font_size = 22.0f;
        constexpr float legal_font_size = 18.0f;
        // The prompt fades in, then SLOWLY FLASHES (pulses) forever while the title idles (retail).
        // Period ~2.6 s, brightness ~0.25..1.0 (never fully off so it reads as a pulse, not a blink).
        const float prompt_in = std::clamp((title_time - 0.5f) / 0.7f, 0.0f, 1.0f);
        const float prompt_pulse =
            0.25f + 0.75f * (0.5f + 0.5f * std::sin(title_time * 2.4f - 1.5708f));
        const std::uint32_t prompt_a = alpha_byte(prompt_in * prompt_pulse);
        const float prompt_y = height * 0.56f;
        const bool use_accept_glyph =
            using_controller_prompts && access_.shader_ready(NativeUiAsset::Accept);
        const auto text_col = rgba(235, 235, 235, prompt_a);
        const auto shadow_col = rgba(0, 0, 0, prompt_a * 3 / 5);
        if (use_accept_glyph && accept_prompt == "A") {
            const float icon_size = 28.0f;
            const float prefix_w = text_width("Press ", prompt_font_size);
            const float prompt_w = prefix_w + icon_size + text_width(" to start", prompt_font_size);
            const float prompt_x = (width - prompt_w) * 0.5f;
            emit_text(scene, "Press ", prompt_x + 2.0f, prompt_y + 2.0f, prompt_font_size, shadow_col);
            emit_text(scene, "Press ", prompt_x, prompt_y, prompt_font_size, text_col);
            add(NativeUiAsset::Accept, prompt_x + prefix_w, prompt_y - 1.0f,
                prompt_x + prefix_w + icon_size, prompt_y - 1.0f + icon_size, 0.0f, 0.0f, 0.25f, 0.25f,
                text_col);
            emit_text(scene, " to start", prompt_x + prefix_w + icon_size + 2.0f, prompt_y + 2.0f,
                      prompt_font_size, shadow_col);
            emit_text(scene, " to start", prompt_x + prefix_w + icon_size, prompt_y, prompt_font_size,
                      text_col);
        } else {
            const float prompt_x = (width - text_width(prompt_text, prompt_font_size)) * 0.5f;
            emit_text(scene, prompt_text, prompt_x + 2.0f, prompt_y + 2.0f, prompt_font_size,
                      shadow_col);
            emit_text(scene, prompt_text, prompt_x, prompt_y, prompt_font_size, text_col);
        }
        const float legal_in = std::clamp((title_time - 0.20f) / 0.45f, 0.0f, 1.0f);
        const float legal_out = 1.0f - std::clamp((title_time - 5.90f) / 0.65f, 0.0f, 1.0f);
        const std::uint32_t legal_a = alpha_byte(legal_in * legal_out);
        const auto legal_col = rgba(242, 242, 242, legal_a);
        const auto legal_shadow = rgba(0, 0, 0, legal_a * 3 / 5);
        const auto legal_line = [&](std::string_view text, float y) {
            const float x = (width - text_width(text, legal_font_size)) * 0.5f;
            emit_text(scene, text, x + 2.0f, y + 2.0f, legal_font_size, legal_shadow);
            emit_text(scene, text, x, y, legal_font_size, legal_col);
        };
        legal_line("\xC2\xA9 & \xC2\xAE 2008 Microsoft Corporation. All rights reserved. Developed by",
                   height * 0.73f);
        legal_line("Lionhead Studios.", height * 0.79f);
        legal_line("Online Interactions Not Rated by the ESRB", height * 0.87f);
    }
}

}  // namespace f2
