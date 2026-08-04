#pragma once

#include "f2/native_texture.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <vector>

namespace f2 {

// Temporary native approximation for the title ambient layer. The exact
// FXGUI_Logomain_Ambient sprite material is still gated on PM4 geometry and
// constants; keeping this isolated makes the eventual replacement shared by
// D3D12 and Vulkan.
class NativeLogoSparkles {
public:
    // The selected user's logo mask is used only as a temporary visual stand-in
    // until the captured FXGUI_Logomain_Ambient material is decoded.
    void draw_ambient(ImDrawList* draw_list,
                      ImTextureID logo_texture,
                      float x,
                      float y,
                      float width,
                      float height,
                      float time,
                      float opacity);

    void draw(ImDrawList* draw_list,
              const NativeTexture& logo,
              const std::array<ImTextureID, 8>& textures,
              float x,
              float y,
              float width,
              float height,
              float time,
              float opacity);

private:
    struct Particle {
        float x = 0.0f;
        float y = 0.0f;
        float delay = 0.0f;
        float life = 0.5f;
        float fall = 0.0f;
        std::uint8_t texture = 0;
        bool falls = false;
    };

    void rebuild(const NativeTexture& logo);

    std::vector<Particle> particles_;
    std::uint32_t seed_ = 0;
    std::uint32_t logo_width_ = 0;
    std::uint32_t logo_height_ = 0;
};

}  // namespace f2
