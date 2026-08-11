#include "f2/frontend_scene_builder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace f2 {
namespace {

std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

std::uint32_t alpha_byte(float value) {
    return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}

// Retail main-menu slot table (position/opacity/scale per carousel slot), from the captured 1280x720
// front end. retail_menu_slot maps a display index to its slot given the selected index.
struct RetailMenuSlot {
    float x;
    float y;
    float opacity;
    float scale;
};

constexpr std::array<RetailMenuSlot, 12> kRetailMenuSlots = {{
    {0.0f, 85.0f, 0.0f, 0.65f},     {22.0f, 69.0f, 50.0f, 0.75f},
    {42.0f, 15.0f, 75.0f, 0.85f},   {56.0f, -45.0f, 100.0f, 1.0f},
    {56.0f, -106.0f, 100.0f, 1.0f}, {56.0f, -164.0f, 100.0f, 1.0f},
    {56.0f, -222.0f, 100.0f, 1.0f}, {56.0f, -280.0f, 100.0f, 1.0f},
    {50.0f, -338.0f, 100.0f, 1.0f}, {42.0f, -396.0f, 75.0f, 0.85f},
    {22.0f, -454.0f, 50.0f, 0.75f}, {0.0f, -475.0f, 0.0f, 0.65f},
}};

constexpr int kRetailHighlightSlot = 4;
constexpr float kRetailCenterSlotX = 56.0f;
constexpr float kRetailCenterSlotY = -45.0f;

const RetailMenuSlot* retail_menu_slot(std::size_t display_index, std::size_t selected_index) {
    const int slot = kRetailHighlightSlot + static_cast<int>(display_index) -
                     static_cast<int>(selected_index);
    if (slot < 1 || slot > static_cast<int>(kRetailMenuSlots.size())) return nullptr;
    return &kRetailMenuSlots[static_cast<std::size_t>(slot - 1)];
}

}  // namespace

FrontendSceneBuilder::FrontendSceneBuilder(const NativeFont& font, const NativeUiAssets& assets,
                                           TextureAccess access)
    : font_(font), assets_(assets), access_(std::move(access)) {}

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
    constexpr std::size_t kCount = 600;  // measured burst spawns hundreds; a dense band, not sparse dots
    title_sparkles_.reserve(kCount);
    const NativeTexture* logo = assets_.texture(NativeUiAsset::Logo);
    const bool have_mask = logo && logo->width > 0 && logo->height > 0 &&
                           logo->rgba8.size() >=
                               static_cast<std::size_t>(logo->width) * logo->height * 4;
    std::uint32_t seed = 0xF2B1052u;
    const auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed & 0x00ffffffu) / 16777215.0f;
    };
    for (std::size_t i = 0; i < kCount; ++i) {
        TitleSparkle s;
        // TARGET: a point on the "Fable II" silhouette (logo alpha mask) — the sparkles
        // converge here to resolve the letters (measured §10: converge-to-silhouette).
        float tx = rnd();
        float ty = rnd();
        if (have_mask) {
            for (int attempt = 0; attempt < 48; ++attempt) {
                tx = rnd();
                ty = rnd();
                const auto px = (static_cast<std::size_t>(ty * logo->height) * logo->width +
                                 static_cast<std::size_t>(tx * logo->width)) * 4;
                if (px + 3 < logo->rgba8.size() && logo->rgba8[px + 3] > 24) break;
            }
        }
        s.target_x = tx;
        s.target_y = ty;
        // SPAWN: diffuse in a WIDE FLAT horizontal band over the wordmark (measured §10: X span
        // ~5-10x wider than Y — a tight horizontal band, ~1.15x wide x ~1.1x tall vs the text).
        s.spawn_x = (rnd() - 0.5f) * 1.15f + 0.5f;
        s.spawn_y = (rnd() - 0.5f) * 1.1f + 0.5f;
        s.delay = rnd();
        s.size = 12.0f + rnd() * 18.0f;
        s.phase = rnd() * 6.2831853f;
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
    // The title reveal (ignite→settle, first ~4s) plays over a FLAT GREY backdrop (measured
    // RGB ~72,72,72 in title_capture/reveal_frames/*) — the app clears the Title frame grey.
    // The scenic panorama then FADES IN over that grey from ~5.9s (retail: the title shows the
    // panorama once you wait past the reveal), scrolling slowly.
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

    // The SOLID wordmark resolves LATE — the letters emerge from the converging star cloud and
    // only become solid near the end of the reveal (retail t021882 ≈ native 2.65s), not during
    // the burst. So fade the logo in from ~1.9s to ~2.65s (the stars carry the letter shape until).
    const float logo_fade = std::clamp((title_time - 1.9f) / 0.75f, 0.0f, 1.0f);
    const NativeTexture* logo = assets_.texture(NativeUiAsset::Logo);
    // Enter at ignite (0.86s) even though logo_fade is still 0 — the ignition BLOOM + sparkle
    // band flash BEFORE the solid wordmark fades in (the letters emerge from the burst).
    if (logo && access_.id(NativeUiAsset::Logo) && title_time >= 0.86f) {
        const float logo_scale = width / (logo->width * 1.8f);
        const float logo_width = logo->width * logo_scale;
        const float logo_height = logo->height * logo_scale;
        const float logo_x = (width - logo_width) * 0.5f;
        // Wordmark vertical center measured at ~0.43·H in the retail reveal frames (was 0.47).
        const float logo_y = height * 0.43f - logo_height * 0.5f;
        add(NativeUiAsset::Logo, logo_x, logo_y, logo_x + logo_width, logo_y + logo_height, 0.0f,
            0.0f, 1.0f, 1.0f, rgba(255, 255, 255, alpha_byte(logo_fade)));

        // Title BURST + SPARKLES — measured from the retail reveal (ghidra_out/title_sparkle_
        // burst_re.txt §8/§10 + the reveal_frames/*.png captures): a BLUE-white additive GPU
        // point-sprite system over BLACK. (A) a big soft radial BLOOM (fe_logo_ambient starburst
        // blown up ~screen-tall) that flashes at ignite; (B) a DENSE horizontal SPARKLE BAND of
        // fe_logo_ambient blobs packed across the wordmark that CONVERGES onto the "Fable II"
        // silhouette to resolve the letters; (C) a settled blue GLOW rim. Timing anchored to
        // ignite = logo-fade start (0.86 s): burst peaks +1.1 s, letters resolve by +3.0 s.
        const float kIgnite = 0.86f;
        const float kBurstPeak = kIgnite + 1.1f;
        const float kConvergeEnd = kIgnite + 3.0f;
        const auto add_flare = [&](float px, float py, float half, std::uint32_t color) {
            const auto tid = access_.id(NativeUiAsset::LogoFlare);
            if (!tid) return;
            // Inset the UVs to skip fe_logo_ambient.png's 1px OPAQUE BLACK border (alpha=255 at
            // the very edge), which otherwise renders as a faint square frame around each sparkle.
            scene.add_sprite(tid, px - half, py - half, px + half, py + half, 0.05f, 0.05f, 0.95f,
                             0.95f, color);
            scene.set_last_blend(f2::render::BlendMode::Additive);
        };
        const float cx = logo_x + logo_width * 0.5f;
        const float cy = logo_y + logo_height * 0.5f;

        // (A) IGNITION BLOOM: a FULL-SCREEN additive WASH that flashes the whole screen bright at
        // ignite then fades to black — MEASURED to match the retail reveal frames' background
        // corners: 225 -> 180 -> 127 -> 72 -> 0 (title_capture/reveal_frames/*, an additive bloom
        // over black, NOT a static grey backdrop). The uniform wash reproduces the corner fade
        // (native corners were black before — the bloom must reach the corners); a brighter blue
        // central flare adds the wordmark-band pop. wash≈0 by the ~5.9s panorama fade-in.
        float bloom = 0.0f;
        if (title_time >= kIgnite) {
            const float tp = std::clamp((title_time - kIgnite) / 4.7f, 0.0f, 1.0f);
            bloom = std::pow(1.0f - tp, 2.2f);  // (1-t)^2.2 tracks the measured 225→72→0 corner fade
        }
        if (bloom > 0.004f) {
            add_rect(scene, 0.0f, 0.0f, width, height, rgba(248, 249, 252, alpha_byte(bloom * 0.88f)));
            scene.set_last_blend(f2::render::BlendMode::Additive);
        }
        (void)cx;
        (void)cy;

        // (B) SPARKLE CLOUD: spawn in a band, converge to the wordmark silhouette, then twinkle.
        ensure_title_sparkles();
        const float unit = width / 1280.0f;
        const auto ease_out = [](float t) { return 1.0f - (1.0f - t) * (1.0f - t); };
        for (const auto& s : title_sparkles_) {
            const float own_start = kIgnite + s.delay * 0.8f;
            if (title_time < own_start) continue;
            const float conv = ease_out(std::clamp(
                (title_time - own_start) / (kConvergeEnd - own_start), 0.0f, 1.0f));
            // spawn/target in logo-rect normalized coords -> screen.
            const float sxn = s.spawn_x + (s.target_x - s.spawn_x) * conv;
            const float syn = s.spawn_y + (s.target_y - s.spawn_y) * conv;
            const float jit = (1.0f - conv) * 4.0f * unit;
            const float px = logo_x + sxn * logo_width + std::sin(s.phase + title_time * 2.0f) * jit;
            const float py = logo_y + syn * logo_height + std::cos(s.phase + title_time * 2.3f) * jit;
            // opacity: fade in, hold bright through the burst, drop to a low idle twinkle.
            const float fade_in = std::clamp((title_time - own_start) / 0.3f, 0.0f, 1.0f);
            float a;
            if (title_time < kConvergeEnd) {
                a = fade_in * (0.55f + 0.45f * (title_time < kBurstPeak ? 1.0f : 0.8f));
            } else {
                a = 0.18f + 0.20f * (0.5f + 0.5f * std::sin(title_time * 3.0f + s.phase));  // twinkle
            }
            if (a <= 0.02f) continue;
            const float half = s.size * unit * (1.5f - 0.8f * conv) * 0.5f;
            add_flare(px, py, half, rgba(210, 228, 255, alpha_byte(a)));
        }

        // (C) SETTLED GLOW: a soft blue rim hugging the letters (logo silhouette, slightly enlarged,
        // low additive alpha, gentle pulse) once the letters have resolved.
        const float glow_in = std::clamp((title_time - (kBurstPeak)) / 0.8f, 0.0f, 1.0f);
        if (glow_in > 0.0f) {
            const float pulse = 0.5f + 0.5f * std::sin(title_time * (6.2831853f / 2.6f));
            const float glow_a = glow_in * (0.10f + 0.06f * pulse);
            const float gx = logo_width * 0.03f;
            const float gy = logo_height * 0.06f;
            scene.add_sprite(access_.id(NativeUiAsset::Logo), logo_x - gx, logo_y - gy,
                             logo_x + logo_width + gx, logo_y + logo_height + gy, 0.0f, 0.0f, 1.0f,
                             1.0f, rgba(120, 165, 255, alpha_byte(glow_a)));
            scene.set_last_blend(f2::render::BlendMode::Additive);
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

void FrontendSceneBuilder::emit_centered(f2::render::UiDrawList& scene, std::string_view text,
                                         float cx, float y, float size, std::uint32_t color) const {
    emit_text(scene, text, cx - text_width(text, size) * 0.5f, y, size, color);
}

void FrontendSceneBuilder::add_rect(f2::render::UiDrawList& scene, float x0, float y0, float x1,
                                    float y1, std::uint32_t color) const {
    if (!access_.font_ready || !access_.font_ready()) return;
    const auto fid = access_.font_id ? access_.font_id() : f2::render::kInvalidTexture;
    if (!fid) return;
    const float su = font_.solid_u();
    const float sv = font_.solid_v();
    scene.add_sprite(fid, x0, y0, x1, y1, su, sv, su, sv, color);
}

void FrontendSceneBuilder::add_menu_capsule(f2::render::UiDrawList& scene, float x0, float y0,
                                            float x1, float y1, float width) const {
    const float slice = 52.0f * (width / 1280.0f);
    const std::uint32_t white = 0xffffffffu;
    if (const auto id = access_.id(NativeUiAsset::MenuSurface)) {
        scene.add_sprite(id, x0, y0, x0 + slice, y1, 0.0f, 0.0f, 0.125f, 1.0f, white);
        scene.add_sprite(id, x0 + slice, y0, x1 - slice, y1, 0.125f, 0.0f, 0.875f, 1.0f, white);
        scene.add_sprite(id, x1 - slice, y0, x1, y1, 0.875f, 0.0f, 1.0f, 1.0f, white);
    }
    if (const auto id = access_.id(NativeUiAsset::FrameElements)) {
        const float v0 = 4.0f / 512.0f, v1 = 84.0f / 512.0f;
        const float xs[4] = {x0, x0 + slice, x1 - slice, x1};
        const float us[4] = {0.0f, 0.125f, 0.820f, 0.945f};
        for (int i = 0; i < 3; ++i) {
            scene.add_sprite(id, xs[i], y0, xs[i + 1], y1, us[i], v0,
                             (i == 0 ? 0.125f : i == 1 ? 0.813f : 0.945f), v1, white);
            scene.set_last_key_black(true);
        }
    }
}

void FrontendSceneBuilder::build_video(f2::render::UiDrawList& scene, float width, float height,
                                       f2::render::TextureId video) const {
    if (!video) return;
    scene.add_sprite(video, 0.0f, 0.0f, width, height, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffffu);
}

void FrontendSceneBuilder::build_choose_card(f2::render::UiDrawList& scene, float width,
                                             float height) const {
    const auto add_card = [&](NativeUiAsset asset, float center_x, float angle) {
        const auto id = access_.id(asset);
        if (!id) return;
        const float scale = width / 1280.0f;
        const float card_width = 256.0f * scale;
        const float card_height = 384.0f * scale;
        const float center_y = height * 0.502f;
        scene.add_sprite(id, center_x * scale - card_width * 0.5f, center_y - card_height * 0.5f,
                         center_x * scale + card_width * 0.5f, center_y + card_height * 0.5f, 0.0f,
                         0.0f, 1.0f, 1.0f, 0xffffffffu);
        scene.set_last_rotation(angle);
    };
    // Measured retail card centers/tilts from the captured choosecard screen at 1280x720.
    add_card(NativeUiAsset::CardBoy, 470.0f, -0.14f);
    add_card(NativeUiAsset::CardGirl, 781.0f, 0.105f);
}

void FrontendSceneBuilder::build_fps_overlay(f2::render::UiDrawList& scene, float width, float height,
                                             double fps) const {
    (void)height;
    const float unit = width / 1280.0f;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f FPS", fps);
    const float size = 20.0f * unit;
    const float x = width - text_width(buffer, size) - 12.0f * unit;
    const float y = 8.0f * unit;
    emit_text(scene, buffer, x + 1.0f * unit, y + 1.0f * unit, size, rgba(0, 0, 0, 190));
    emit_text(scene, buffer, x, y, size, rgba(80, 255, 80, 255));
}

void FrontendSceneBuilder::build_options_chrome(f2::render::UiDrawList& scene, float width,
                                                float height) const {
    (void)height;
    const float scale = width / 1280.0f;
    // Title pill.
    const float tx = 220.0f * scale, ty = 53.0f * scale, tw = 390.0f * scale, th = 47.0f * scale;
    add_menu_capsule(scene, tx, ty, tx + tw, ty + th, width);
    emit_centered(scene, "Options", tx + tw * 0.5f, ty + 8.0f * scale, 27.0f * scale,
                  rgba(238, 238, 238, 255));
    // Footer pill: [B] Back .... (coin) 5400
    const float fx = 220.0f * scale, fy = 594.0f * scale, fw = 390.0f * scale, fh = 47.0f * scale;
    add_menu_capsule(scene, fx, fy, fx + fw, fy + fh, width);
    if (access_.shader_ready(NativeUiAsset::Accept)) {
        scene.add_sprite(access_.id(NativeUiAsset::Accept), fx + 7.0f * scale, fy + 0.5f * scale,
                         fx + 53.0f * scale, fy + 46.5f * scale, 0.25f, 0.0f, 0.50f, 0.25f,
                         rgba(255, 255, 255, 255));
    }
    emit_text(scene, "Back", fx + 58.0f * scale, fy + 10.0f * scale, 22.0f * scale,
              rgba(238, 238, 238, 255));
    if (access_.shader_ready(NativeUiAsset::GoldCoin)) {
        scene.add_sprite(access_.id(NativeUiAsset::GoldCoin), fx + 247.0f * scale, fy + 7.0f * scale,
                         fx + 279.0f * scale, fy + 39.0f * scale, 0.0f, 0.0f, 1.0f, 1.0f,
                         rgba(255, 255, 255, 255));
    }
    emit_text(scene, "5400", fx + 288.0f * scale, fy + 10.0f * scale, 22.0f * scale,
              rgba(245, 222, 65, 255));
}

void FrontendSceneBuilder::build_options_page(f2::render::UiDrawList& scene, float width,
                                              float height, const NativeGame& game) const {
    const float unit = width / 1280.0f;
    const auto& options = game.frontend.options_items();
    const std::size_t selected =
        options.empty() ? std::size_t{0}
                        : std::min(game.frontend.selected_item(), options.size() - 1);
    const std::string_view page_id =
        options.empty() ? std::string_view{} : std::string_view(options[selected].id);
    const float page_x = width * 0.495f;
    const float page_w = width * 0.325f;
    const float center_x = page_x + page_w * 0.50f;
    add_rect(scene, page_x - 10.0f * unit, 0.0f, page_x + page_w + 10.0f * unit, height,
             rgba(0, 0, 0, 110));
    if (const auto id = access_.id(NativeUiAsset::FramesPageTexture)) {
        scene.add_sprite(id, page_x, 0.0f, page_x + page_w, height, 0.0f, 0.0f, 1.0f, 1.0f,
                         rgba(255, 255, 255, 245));
    }
    emit_centered(scene, options.empty() ? "Options" : options[selected].label, center_x,
                  height * 0.105f, 27.0f * unit, rgba(105, 55, 27, 255));
    if (const auto id = access_.id(NativeUiAsset::Motifs)) {
        scene.add_sprite(id, page_x + page_w * 0.09f, height * 0.165f, page_x + page_w * 0.91f,
                         height * 0.195f, 0.043f, 0.020f, 0.72f, 0.070f, rgba(133, 78, 28, 255));
    }
    // The row the cursor is on gets the highlight color + the </> adjust arrows.
    const std::uint32_t sel = rgba(145, 72, 30, 255);
    const std::uint32_t nrm = rgba(105, 55, 27, 255);
    const int focus = game.frontend.option_row();
    const auto arrows = [&](float y) {
        const float aw = 20.0f * unit;
        emit_text(scene, "<", page_x + page_w * 0.12f, y - 24.0f * unit, 28.0f * unit,
                  rgba(192, 107, 57, 235));
        emit_text(scene, ">", page_x + page_w * 0.88f - aw, y - 24.0f * unit, 28.0f * unit,
                  rgba(192, 107, 57, 235));
    };
    const auto value = [&](std::string_view label, std::string_view val, float y, int row) {
        emit_centered(scene, label, center_x, y, 21.0f * unit, focus == row ? sel : nrm);
        emit_centered(scene, val, center_x, y + 34.0f * unit, 21.0f * unit, rgba(24, 24, 24, 255));
        if (focus == row) arrows(y + 32.0f * unit);
    };
    const auto slider = [&](std::string_view label, int v, float y, int row) {
        if (!label.empty())
            emit_centered(scene, label, center_x, y, 21.0f * unit, focus == row ? sel : nrm);
        const float x0 = page_x + page_w * 0.22f;
        const float x1 = page_x + page_w * 0.78f;
        const float by = y + 34.0f * unit;
        add_rect(scene, x0, by, x1, by + 5.0f * unit, rgba(43, 40, 45, 255));
        add_rect(scene, x1 - 18.0f * unit * (static_cast<float>(v) / 100.0f), by, x1,
                 by + 5.0f * unit, rgba(190, 105, 66, 255));
    };
    if (page_id == "game") {
        value("Subtitles", game.frontend.subtitles_enabled() ? "On" : "Off", height * 0.235f, 0);
        value("Glowing Trail Brightness",
              game.frontend.breadcrumb_size() == 0   ? "Off"
              : game.frontend.breadcrumb_size() == 1 ? "Medium"
                                                     : "Bright",
              height * 0.355f, 1);
        value("Tutorials", game.frontend.tutorial_boxes_enabled() ? "On" : "Off", height * 0.475f, 2);
        value("Online Orbs", game.frontend.multiplayer_orbs_enabled() ? "Friends Only" : "Off",
              height * 0.595f, 3);
        value("Auto Joinable", game.frontend.auto_joinable_enabled() ? "On" : "Off", height * 0.715f,
              4);
    } else if (page_id == "controls") {
        value("Invert Aim", game.frontend.invert_aim_enabled() ? "On" : "Off", height * 0.235f, 0);
    } else if (page_id == "audio") {
        slider("Sounds", game.frontend.sounds_volume(), height * 0.255f, 0);
        slider("Music", game.frontend.music_volume(), height * 0.405f, 1);
        slider("Voice", game.frontend.voice_volume(), height * 0.555f, 2);
        value("Speakers", game.frontend.speaker_mode() == 0 ? "5.1 Surround" : "Stereo",
              height * 0.695f, 3);
    } else if (page_id == "video") {
        if (const auto id = access_.id(NativeUiAsset::CalibrationImage)) {
            scene.add_sprite(id, page_x + page_w * 0.16f, height * 0.23f, page_x + page_w * 0.86f,
                             height * 0.55f, 0.0f, 0.0f, 1.0f, 0.75f, rgba(255, 255, 255, 255));
        }
        emit_centered(scene, "Gamma", center_x, height * 0.575f, 21.0f * unit,
                      focus == 0 ? sel : nrm);
        slider("", game.frontend.gamma_percent(), height * 0.615f, 0);
        emit_centered(scene, "Adjust the gamma so that you are just", center_x, height * 0.685f,
                      17.0f * unit, rgba(35, 35, 35, 255));
        emit_centered(scene, "able to see the text on the left side", center_x, height * 0.725f,
                      17.0f * unit, rgba(35, 35, 35, 255));
        emit_centered(scene, "of the circular image.", center_x, height * 0.765f, 17.0f * unit,
                      rgba(35, 35, 35, 255));
        emit_centered(scene, "Display", center_x, height * 0.805f, 17.0f * unit,
                      rgba(105, 55, 27, 255));
        const auto display_value = [&](std::string_view label, std::string_view val, float y,
                                       int row) {
            const std::uint32_t col = focus == row ? sel : nrm;
            emit_text(scene, label, page_x + page_w * 0.19f, y, 16.0f * unit, col);
            emit_text(scene, val, page_x + page_w * 0.81f - text_width(val, 16.0f * unit), y,
                      16.0f * unit, rgba(24, 24, 24, 255));
        };
        const char* resolutions[] = {"1280 x 720", "1920 x 1080", "2560 x 1440"};
        const char* anti_aliasing[] = {"Off", "2x", "4x", "8x"};
        display_value("Resolution", resolutions[game.frontend.resolution_index()], height * 0.828f, 1);
        display_value("Anti-Aliasing", anti_aliasing[game.frontend.anti_aliasing_index()],
                      height * 0.858f, 2);
        display_value("FPS Display", game.frontend.fps_display_enabled() ? "On" : "Off",
                      height * 0.888f, 3);
    }
    const bool has_controller = access_.shader_ready(NativeUiAsset::Accept);
    const auto page_prompt = [&](std::string_view label, float y, float u0) {
        const float icon = 27.0f * unit;
        const float gap = 6.0f * unit;
        const float ts = 17.0f * unit;
        const float mw = text_width(label, ts);
        const float right = page_x + page_w * 0.88f;
        const float left = right - (icon + gap + mw);
        emit_text(scene, label, left + 1.0f * unit, y + 1.0f * unit, ts, rgba(0, 0, 0, 70));
        emit_text(scene, label, left, y, ts, rgba(35, 35, 35, 255));
        if (has_controller) {
            const float icon_x = left + mw + gap;
            scene.add_sprite(access_.id(NativeUiAsset::Accept), icon_x, y - 3.0f * unit,
                             icon_x + icon, y - 3.0f * unit + icon, u0, 0.0f, u0 + 0.25f, 0.25f,
                             rgba(255, 255, 255, 255));
        }
    };
    page_prompt("Cancel", height * 0.900f, 0.25f);
    page_prompt("Accept", height * 0.950f, 0.0f);
}

void FrontendSceneBuilder::build_main_menu(f2::render::UiDrawList& scene, float width, float height,
                                           const NativeGame& game, bool using_controller_prompts) {
    const auto add = [&](NativeUiAsset asset, float x0, float y0, float x1, float y1, float u0,
                         float v0, float u1, float v1, std::uint32_t color) {
        const auto tid = access_.id(asset);
        if (!tid) return;
        scene.add_sprite(tid, x0, y0, x1, y1, u0, v0, u1, v1, color);
    };
    const auto alpha = [](float value) { return alpha_byte(value); };
    const auto add_native_text = [&](std::string_view text, float x, float y, float size,
                                     std::uint32_t color) { emit_text(scene, text, x, y, size, color); };

    const auto background_asset = access_.id(NativeUiAsset::MainBackground)
                                      ? NativeUiAsset::MainBackground
                                      : NativeUiAsset::TitleBackground;
    if (const NativeTexture* background = assets_.texture(background_asset);
        background && access_.id(background_asset)) {
        const float scale = height / static_cast<float>(background->height);
        const float image_width = background->width * scale;
        const float pan_scale = width / 1280.0f;
        const float offset = std::fmod(
            width * 0.78125f + static_cast<float>(game.frontend.state_time()) * 29.0f * pan_scale,
            image_width);
        const auto background_id = access_.id(background_asset);
        for (float x = -offset; x < width; x += image_width) {
            scene.add_sprite(background_id, x, 0.0f, x + image_width, height, 0.0f, 0.0f, 1.0f, 1.0f,
                             rgba(255, 255, 255, 255));
        }
    } else {
        add(NativeUiAsset::MenuSurface, 0.0f, 0.0f, width * 0.16f, height, 0.0f, 0.0f, 1.0f, 1.0f,
            rgba(255, 255, 255, 245));
        add(NativeUiAsset::MenuSurface, width * 0.84f, 0.0f, width, height, 0.0f, 0.0f, 1.0f, 1.0f,
            rgba(255, 255, 255, 245));
    }

    const bool have_frame_elements = access_.id(NativeUiAsset::FrameElements) != 0;
    const float menu_unit_scale = width / 1280.0f;
    // Projected from the retail sprite shader at the 1280x720 reference. Rows are three-sliced, 450x68.
    const float row_x = 177.0f * menu_unit_scale;
    const float row_y = 160.0f * menu_unit_scale;
    const float row_width = 450.0f * menu_unit_scale;
    const float row_height = 68.0f * menu_unit_scale;
    const float slice_width = 52.0f * menu_unit_scale;
    const auto add_three_slice = [&](NativeUiAsset asset, float x0, float y0, float x1, float y1,
                                     float v0, float v1, std::uint32_t color, bool key_black_matte) {
        const auto add_slice = [&](float sx0, float sy0, float sx1, float sy1, float su0, float sv0,
                                   float su1, float sv1) {
            const auto before = scene.size();
            add(asset, sx0, sy0, sx1, sy1, su0, sv0, su1, sv1, color);
            if (key_black_matte && scene.size() != before) scene.set_last_key_black(true);
        };
        const float center_x0 = x0 + slice_width;
        const float center_x1 = x1 - slice_width;
        add_slice(x0, y0, center_x0, y1, 0.0f, v0, 0.125f, v1);
        add_slice(center_x0, y0, center_x1, y1, 0.125f, v0, 0.813f, v1);
        add_slice(center_x1, y0, x1, y1, 0.820f, v0, 0.945f, v1);
    };
    const bool have_ability = access_.id(NativeUiAsset::AbilityElements) != 0;
    const bool have_menu_surface = access_.id(NativeUiAsset::MenuSurface) != 0;
    const auto add_body_three_slice = [&](float x0, float y0, float x1, float y1,
                                          std::uint32_t color) {
        if (!have_ability || !have_menu_surface) return;
        const float body_width = x1 - x0;
        const float body_slice_width = body_width * (52.0f / 452.0f);
        // The retail two-texture body: SHAPE/alpha from ability_elements' opaque fill-mask core
        // (v104..143) and leather GRAIN from menu_surface as the detail -> fully opaque leather.
        const auto add_body_slice = [&](float sx0, float sx1, float su0, float su1) {
            const auto before = scene.size();
            add(NativeUiAsset::AbilityElements, sx0, y0, sx1, y1, su0, 104.0f / 512.0f, su1,
                143.0f / 512.0f, color);
            if (scene.size() == before) return;
            scene.set_last_detail(access_.id(NativeUiAsset::MenuSurface), (sx0 - x0) / body_width,
                                  0.0f, (sx1 - x0) / body_width, 1.0f);
        };
        const float center_x0 = x0 + body_slice_width;
        const float center_x1 = x1 - body_slice_width;
        add_body_slice(x0, center_x0, 12.0f / 512.0f, 64.0f / 512.0f);
        add_body_slice(center_x0, center_x1, 64.0f / 512.0f, 412.0f / 512.0f);
        add_body_slice(center_x1, x1, 412.0f / 512.0f, 464.0f / 512.0f);
    };
    const std::size_t selected_index = game.frontend.selected_item();
    const std::size_t previous_selected_index = game.frontend.previous_selected_item();
    const float selection_t =
        std::clamp(static_cast<float>(game.frontend.selection_time() / 0.15), 0.0f, 1.0f);
    const bool selection_animating = game.frontend.selection_animating();
    bool selected_prompt_ready = false;
    float selected_prompt_x = 0.0f;
    float selected_prompt_y = 0.0f;
    std::uint32_t selected_prompt_color = 0;
    struct RetailDrawRow {
        std::string label;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float scale = 1.0f;
        std::uint32_t color = 0;
    };
    std::vector<RetailDrawRow> draw_rows;
    // For the Options state the rows are the submenu tabs (options_items), not the main menu.
    const auto& displayed_items = game.frontend.state() == f2::FrontendState::Options
                                      ? game.frontend.options_items()
                                      : game.frontend.menu_items();
    for (std::size_t index = 0; index < displayed_items.size(); ++index) {
        const RetailMenuSlot* target_slot = retail_menu_slot(index, selected_index);
        const RetailMenuSlot* source_slot = retail_menu_slot(index, previous_selected_index);
        if (!target_slot || (selection_animating && !source_slot)) continue;
        const bool selected = index == game.frontend.selected_item();
        const RetailMenuSlot& from = selection_animating ? *source_slot : *target_slot;
        const auto lerp = [selection_t](float a, float b) { return a + (b - a) * selection_t; };
        const float slot_x = lerp(from.x, target_slot->x);
        const float slot_y = lerp(from.y, target_slot->y);
        const float slot_opacity = lerp(from.opacity, target_slot->opacity);
        const float slot_scale = lerp(from.scale, target_slot->scale);
        const float draw_width = row_width * slot_scale;
        const float draw_height = row_height * slot_scale;
        const float x = row_x + (slot_x - kRetailCenterSlotX) * menu_unit_scale;
        const float y = row_y + (kRetailCenterSlotY - slot_y) * menu_unit_scale +
                        (row_height - draw_height) * 0.5f;
        const float slot_alpha = slot_opacity / 100.0f;
        const auto row_color = rgba(255, 255, 255, alpha(slot_alpha));
        const auto& item = displayed_items[index];
        draw_rows.push_back({item.label, x, y, draw_width, draw_height, slot_scale, row_color});
        if (selected && using_controller_prompts && have_frame_elements) {
            selected_prompt_ready = true;
            // MenuHighlight is a fixed component at the center slot; rows animate through it.
            selected_prompt_x = row_x + 20.0f * menu_unit_scale;
            selected_prompt_y = row_y + 15.0f * menu_unit_scale;
            selected_prompt_color = row_color;
        }
    }
    // Render the recovered passes in serialized order: inner brown layers, frame rims, then labels.
    for (const auto& row : draw_rows) {
        const float body_width = row.width * (452.0f / 484.0f);
        const float body_height = row.height * (56.0f / 80.0f);
        const float body_x0 = row.x + (row.width - body_width) * 0.5f;
        const float body_y0 = row.y + (row.height - body_height) * 0.5f;
        add_body_three_slice(body_x0, body_y0, body_x0 + body_width, body_y0 + body_height,
                             row.color);
    }
    for (const auto& row : draw_rows) {
        if (!have_frame_elements) continue;
        add_three_slice(NativeUiAsset::FrameElements, row.x, row.y, row.x + row.width,
                        row.y + row.height, 4.0f / 512.0f, 84.0f / 512.0f, row.color, true);
    }
    for (const auto& row : draw_rows) {
        add_native_text(row.label, row.x + 92.0f * row.scale, row.y + 18.0f * row.scale,
                        26.0f * menu_unit_scale * row.scale, rgba(255, 224, 128, row.color >> 24u));
    }
    // Side panels overlay the row ends: their curved inner edge must occlude the left button ends.
    const bool has_exact_menu_frame = access_.id(NativeUiAsset::MenuFrameLeftUpper) &&
                                      access_.id(NativeUiAsset::MenuFrameLeftLower) &&
                                      access_.id(NativeUiAsset::MenuFrameRightUpper) &&
                                      access_.id(NativeUiAsset::MenuFrameRightLower);
    if (has_exact_menu_frame) {
        add(NativeUiAsset::MenuFrameLeftUpper, -3.05f * menu_unit_scale, -3.95f * menu_unit_scale,
            271.93f * menu_unit_scale, 513.67f * menu_unit_scale, 0.0f, 0.0f, 0.531f, 1.0f,
            rgba(255, 255, 255, 255));
        add(NativeUiAsset::MenuFrameLeftLower, -3.05f * menu_unit_scale, 513.67f * menu_unit_scale,
            271.93f * menu_unit_scale, 723.95f * menu_unit_scale, 0.0f, 0.0f, 0.531f, 0.406f,
            rgba(255, 255, 255, 255));
        add(NativeUiAsset::MenuFrameRightUpper, 1007.06f * menu_unit_scale, -4.95f * menu_unit_scale,
            1283.56f * menu_unit_scale, 515.53f * menu_unit_scale, 0.0f, 0.0f, 0.531f, 1.0f,
            rgba(255, 255, 255, 255));
        add(NativeUiAsset::MenuFrameRightLower, 1007.06f * menu_unit_scale, 515.53f * menu_unit_scale,
            1283.56f * menu_unit_scale, 726.98f * menu_unit_scale, 0.0f, 0.0f, 0.531f, 0.406f,
            rgba(255, 255, 255, 255));
    } else if (access_.id(NativeUiAsset::MenuFrameOverlay)) {
        scene.add_sprite(access_.id(NativeUiAsset::MenuFrameOverlay), 0.0f, 0.0f, width, height, 0.0f,
                         0.0f, 1.0f, 1.0f, rgba(255, 255, 255, 255));
    }
    if (!has_exact_menu_frame && !access_.id(NativeUiAsset::MenuFrameOverlay) &&
        access_.id(NativeUiAsset::SideRailAtlas)) {
        add(NativeUiAsset::SideRailAtlas, width * 0.145f, 0.0f, width * 0.172f, height, 0.0f, 0.0f,
            74.0f / 256.0f, 720.0f / 1024.0f, rgba(205, 145, 88, 245));
        add(NativeUiAsset::SideRailAtlas, width * 0.828f, 0.0f, width * 0.855f, height,
            74.0f / 256.0f, 0.0f, 150.0f / 256.0f, 720.0f / 1024.0f, rgba(205, 145, 88, 245));
    }
    if (selected_prompt_ready) {
        // MenuHighlight.rim_and_red: the metallic bezel + red center covered by the green passes.
        const float source_prompt_width = 0.448f;
        const float source_rim_width = 1.024f;
        const float source_prompt_left_in_rim = 0.328f;
        const float source_prompt_top_in_rim = 0.844f;
        const float source_prompt_bottom_in_rim = 1.268f;
        const float prompt_pixel_scale = (46.0f * menu_unit_scale) / source_prompt_width;
        const float rim_x0 = selected_prompt_x - source_prompt_left_in_rim * prompt_pixel_scale;
        const float rim_y0 = selected_prompt_y - source_prompt_top_in_rim * prompt_pixel_scale;
        const float rim_x1 = rim_x0 + source_rim_width * prompt_pixel_scale;
        const float rim_y1 = selected_prompt_y + source_prompt_bottom_in_rim * prompt_pixel_scale;
        add(NativeUiAsset::FrameElements, rim_x0, rim_y0, rim_x1, rim_y1, 0.0f, 0.171f, 0.25f, 0.687f,
            selected_prompt_color);
        const float prompt_y = selected_prompt_y - 2.0f * menu_unit_scale;
        add(NativeUiAsset::FrameElements, selected_prompt_x, prompt_y,
            selected_prompt_x + 46.0f * menu_unit_scale, prompt_y + 46.0f * menu_unit_scale,
            8.0f / 512.0f, 369.0f / 512.0f, 64.0f / 512.0f, 425.0f / 512.0f, selected_prompt_color);
        add(NativeUiAsset::FrameElements, selected_prompt_x, prompt_y - 0.4f * menu_unit_scale,
            selected_prompt_x + 46.0f * menu_unit_scale, prompt_y + 45.6f * menu_unit_scale,
            8.0f / 512.0f, 369.0f / 512.0f, 64.0f / 512.0f, 425.0f / 512.0f, selected_prompt_color);
    }
    if (game.frontend.state() == f2::FrontendState::ChooseCard) {
        // choosecard is a modal layer: menu + panorama stay visible behind a translucent black veil.
        const float card_fade = std::clamp(
            (static_cast<float>(game.frontend.state_time()) - 0.04f) / 0.34f, 0.0f, 1.0f);
        const float card_smooth = card_fade * card_fade * (3.0f - 2.0f * card_fade);
        add(NativeUiAsset::MenuSurface, 0.0f, 0.0f, width, height, 0.0f, 0.0f, 1.0f, 1.0f,
            rgba(0, 0, 0, 76));
        const auto add_card = [&](NativeUiAsset asset, float x0, float draw_width, float y_offset,
                                  float angle) {
            const auto id = access_.id(asset);
            if (!id) return;
            const float card_height = 398.0f * menu_unit_scale;
            const float center_y = height * 0.502f + y_offset * menu_unit_scale;
            const float scaled_width = draw_width * menu_unit_scale;
            scene.add_sprite(id, x0 * menu_unit_scale, center_y - card_height * 0.5f,
                             x0 * menu_unit_scale + scaled_width, center_y + card_height * 0.5f, 0.0f,
                             0.0f, 1.0f, 1.0f,
                             rgba(255, 255, 255, static_cast<std::uint32_t>(card_smooth * 255.0f)));
            scene.set_last_rotation(angle);
        };
        add_card(NativeUiAsset::CardBoy, 349.0f, 410.0f, -12.0f, -0.14f);
        add_card(NativeUiAsset::CardGirl, 654.0f, 250.0f, -3.0f, 0.105f);
    }
    // Native Options screen: title/footer chrome always, the settings panel when a submenu is open.
    if (game.frontend.state() == f2::FrontendState::Options) {
        build_options_chrome(scene, width, height);
        if (game.frontend.options_page_open()) build_options_page(scene, width, height, game);
    }
}

}  // namespace f2
