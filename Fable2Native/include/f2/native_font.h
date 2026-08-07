#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace f2 {

// A self-owned bitmap font atlas rasterized from a TTF via stb_truetype's pack API. This replaces the
// frontend's dependency on ImGui's font atlas for native UI text (the ImGui-removal step in
// docs/FRONTEND_ARCHITECTURE.md). Glyph metrics mirror ImGui's ImFontGlyph fields (x0/y0/x1/y1 = pen-
// relative quad offsets at `pixel_height`, u0..v1 = atlas UVs, advance) so the existing
// add_native_text layout math ports across unchanged. Backend-agnostic and headless-testable: it
// produces an RGBA8 atlas + a glyph table; uploading the atlas as a texture is the caller's job.
class NativeFont {
public:
    struct Glyph {
        bool valid = false;
        float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;  // atlas UVs (0..1)
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // pen-relative quad offsets at pixel_height
        float advance = 0.0f;                              // pen advance at pixel_height
    };

    // Rasterize printable ASCII (32..126) from a TTF at `pixel_height` into an RGBA8 atlas.
    bool load(const std::filesystem::path& ttf_path, float pixel_height, std::string& error);
    bool load_from_memory(const std::uint8_t* ttf, std::size_t size, float pixel_height,
                          std::string& error);

    [[nodiscard]] bool ready() const noexcept { return !atlas_rgba8_.empty(); }
    [[nodiscard]] float pixel_height() const noexcept { return pixel_height_; }
    // Ascent (top of line to baseline) in pixels at `size` (glyph y-offsets are top-of-line relative).
    [[nodiscard]] float ascent(float size) const noexcept {
        return pixel_height_ > 0.0f ? ascent_ * (size / pixel_height_) : 0.0f;
    }
    [[nodiscard]] std::uint32_t atlas_width() const noexcept { return atlas_width_; }
    [[nodiscard]] std::uint32_t atlas_height() const noexcept { return atlas_height_; }
    [[nodiscard]] const std::vector<std::uint8_t>& atlas_rgba8() const noexcept {
        return atlas_rgba8_;
    }

    // Glyph for a codepoint; returns an invalid glyph for out-of-range / unrasterized codepoints.
    [[nodiscard]] const Glyph& glyph(std::uint32_t codepoint) const noexcept;

    // Pixel width of `text` laid out at `size` px (glyph advances summed, scaled by size/pixel_height).
    [[nodiscard]] float measure(std::string_view text, float size) const noexcept;

    // UV of an opaque-white texel in the atlas (for solid-color UI rects sampled as a point).
    [[nodiscard]] float solid_u() const noexcept { return solid_u_; }
    [[nodiscard]] float solid_v() const noexcept { return solid_v_; }

    // Latin-1 printable range: ASCII + the symbols the frontend uses (© 0xA9, ® 0xAE in the legal
    // line) + accented letters. Codepoints 0x7F-0x9F are C1 controls with empty glyphs (skipped).
    static constexpr std::uint32_t kFirst = 32;
    static constexpr std::uint32_t kLast = 255;
    static constexpr std::size_t kCount = kLast - kFirst + 1;

private:
    float pixel_height_ = 0.0f;
    float ascent_ = 0.0f;
    float solid_u_ = 0.0f;
    float solid_v_ = 0.0f;
    std::uint32_t atlas_width_ = 0;
    std::uint32_t atlas_height_ = 0;
    std::vector<std::uint8_t> atlas_rgba8_;
    std::array<Glyph, kCount> glyphs_{};
    Glyph invalid_{};
};

}  // namespace f2
