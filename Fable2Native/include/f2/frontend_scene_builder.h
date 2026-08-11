#pragma once

#include "f2/native_font.h"
#include "f2/native_game.h"
#include "f2/native_ui.h"
#include "f2/render/render_backend.h"
#include "f2/render/ui_draw_list.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace f2 {

// Backend-neutral builder for the frontend's UI scenes. It turns the FrontendController state (passed
// per-screen) into an f2::render::UiDrawList that ANY IRenderBackend can draw, so the D3D12 and Vulkan
// frontends render the SAME scene and neither needs ImGui (docs/FRONTEND_ARCHITECTURE.md). Texture
// identity is injected: the app maps each logical texture to a stable TextureId + reports readiness,
// while owning the actual GPU resources. Screens are migrated onto this incrementally.
class FrontendSceneBuilder {
public:
    // How the owning app resolves logical textures to backend-neutral ids. `id` returns kInvalidTexture
    // when the asset's GPU texture is not yet created (mirrors the app's `.texture` guard); `shader_ready`
    // is true only once it is uploaded and sampleable (the stricter `.texture && .shader_read` guard).
    struct TextureAccess {
        std::function<f2::render::TextureId(NativeUiAsset)> id;
        std::function<bool(NativeUiAsset)> shader_ready;
        std::function<f2::render::TextureId()> font_id;
        std::function<bool()> font_ready;
    };

    FrontendSceneBuilder(const NativeFont& font, const NativeUiAssets& assets, TextureAccess access);

    // ---- Screens (migrated from the D3D12 app's render_native_* one at a time) ----
    void build_loading(f2::render::UiDrawList& scene, float width, float height) const;
    void build_title(f2::render::UiDrawList& scene, float width, float height, double state_time,
                     std::string_view accept_prompt, bool using_controller_prompts);
    // Full-screen quad of the decoded intro/attract frame. `video` is the app's video TextureId (the
    // app still owns the streaming texture + gates readiness before calling).
    void build_video(f2::render::UiDrawList& scene, float width, float height,
                     f2::render::TextureId video) const;
    void build_choose_card(f2::render::UiDrawList& scene, float width, float height) const;
    // On-screen FPS counter overlay (top-right), drawn by both backends when the Video option is on.
    void build_fps_overlay(f2::render::UiDrawList& scene, float width, float height, double fps) const;
    // Main menu + (when state==Options) the options chrome/page, and the ChooseCard modal veil+cards.
    void build_main_menu(f2::render::UiDrawList& scene, float width, float height,
                         const NativeGame& game, bool using_controller_prompts);

    // ---- Shared text helpers (used by every screen as it migrates) ----
    void emit_text(f2::render::UiDrawList& scene, std::string_view text, float x, float y, float size,
                   std::uint32_t color) const;
    void emit_centered(f2::render::UiDrawList& scene, std::string_view text, float cx, float y,
                       float size, std::uint32_t color) const;
    [[nodiscard]] float text_width(std::string_view text, float size) const {
        return font_.measure(text, size);
    }

private:
    // Solid-color rect (samples the font atlas' opaque white texel as a point).
    void add_rect(f2::render::UiDrawList& scene, float x0, float y0, float x1, float y1,
                  std::uint32_t color) const;
    // A retail menu capsule (leather three-slice + frame-elements rim): Options title/footer pills.
    void add_menu_capsule(f2::render::UiDrawList& scene, float x0, float y0, float x1, float y1,
                          float width) const;
    void build_options_chrome(f2::render::UiDrawList& scene, float width, float height) const;
    void build_options_page(f2::render::UiDrawList& scene, float width, float height,
                            const NativeGame& game) const;

    // Title reveal sparkle cloud (measured: ghidra_out/title_sparkle_burst_re.txt §8/§10).
    // The retail effect is a blue-white additive point-sprite burst whose sparkles SPAWN in a
    // wide band over the wordmark and CONVERGE onto the "Fable II" silhouette, then settle to a
    // low idle twinkle. Each sprite has a spawn point (band) + a target point (on the logo alpha
    // mask), a stagger delay, size and twinkle phase; positions lerp spawn->target with ease-out.
    struct TitleSparkle {
        float spawn_x = 0.5f;   // normalized in the wordmark rect (can spill outside 0..1)
        float spawn_y = 0.5f;
        float target_x = 0.5f;  // on the logo silhouette (alpha > threshold)
        float target_y = 0.5f;
        float delay = 0.0f;     // 0..1 stagger
        float size = 10.0f;     // px at 1280 wide
        float phase = 0.0f;     // idle-twinkle phase offset
    };
    void ensure_title_sparkles();

    const NativeFont& font_;
    const NativeUiAssets& assets_;
    TextureAccess access_;
    std::vector<TitleSparkle> title_sparkles_;
};

}  // namespace f2
