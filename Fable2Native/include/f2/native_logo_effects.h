#pragma once

#include "f2/native_texture.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <vector>

namespace f2 {

// Native title ambient effects. The captured atlas path uses recovered PM4
// geometry, constants, atlas rectangles, and live timing when user data is
// available; the procedural path remains as a fallback.
class NativeLogoSparkles {
public:
    // The selected user's logo mask remains a temporary visual stand-in until
    // the recovered FXGUI_Logomain_Ambient material is sourced from user data.
    void draw_ambient(ImDrawList* draw_list,
                      ImTextureID logo_texture,
                      float x,
                      float y,
                      float width,
                      float height,
                      float time,
                      float opacity);

    // Draws the recovered main-atlas three-slice geometry when the user has
    // supplied ambient_atlas. The loader has already applied the recovered
    // detail combine to that user-owned atlas before this helper samples it.
    void draw_captured_atlas_slices(ImDrawList* draw_list,
                                    ImTextureID atlas_texture,
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
