#pragma once

#include "f2/native_font.h"
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

    // ---- Shared text helpers (used by every screen as it migrates) ----
    void emit_text(f2::render::UiDrawList& scene, std::string_view text, float x, float y, float size,
                   std::uint32_t color) const;
    [[nodiscard]] float text_width(std::string_view text, float size) const {
        return font_.measure(text, size);
    }

private:
    // Title reveal sparkle field: normalized (0..1) positions within the wordmark rect, seeded once
    // from the logo alpha mask so sparkles cluster on the FABLE II glyphs; emitted continuously.
    struct TitleSparkle {
        float x = 0.5f;
        float y = 0.5f;
        float delay = 0.0f;
        float life = 1.0f;
        float size = 12.0f;
        std::uint8_t texture = 0;
    };

    void ensure_title_sparkles();
    static NativeUiAsset sparkle_asset(std::uint8_t index);

    const NativeFont& font_;
    const NativeUiAssets& assets_;
    TextureAccess access_;
    std::vector<TitleSparkle> title_sparkles_;
};

}  // namespace f2
