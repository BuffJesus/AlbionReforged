#pragma once

#include "f2/native_texture.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <vector>

namespace f2 {

// The title logo's ambient sparkles are driven from the opaque pixels of the
// logo instead of from a second, guessed set of coordinates.  This keeps the
// effect attached to whichever logo the selected game data provides.
class NativeLogoSparkles {
public:
    // FXGUI_Logomain_Ambient uses the same authored Fable II mask as the
    // white logo, but renders it behind the logo with a blue additive-looking
    // falloff. The mask is supplied by the selected user's UI asset root.
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
