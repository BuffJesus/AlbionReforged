#include "f2/native_font.h"

#include <cstdio>
#include <fstream>

// Vendored public-domain stb_truetype (v1.26) — the single implementation unit lives here.
// STBTT_STATIC gives every stb function internal linkage; the ones we don't call then trip
// MSVC C4505 (unreferenced local function), which is expected for a vendored single-header lib.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#endif
#include "stb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace f2 {

bool NativeFont::load(const std::filesystem::path& ttf_path, float pixel_height,
                      std::string& error) {
    std::ifstream file(ttf_path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "The font file could not be opened: " + ttf_path.string();
        return false;
    }
    const auto size = static_cast<std::streamsize>(file.tellg());
    file.seekg(0);
    std::vector<std::uint8_t> blob(static_cast<std::size_t>(size));
    if (size <= 0 || !file.read(reinterpret_cast<char*>(blob.data()), size)) {
        error = "The font file could not be read: " + ttf_path.string();
        return false;
    }
    return load_from_memory(blob.data(), blob.size(), pixel_height, error);
}

bool NativeFont::load_from_memory(const std::uint8_t* ttf, std::size_t size, float pixel_height,
                                  std::string& error) {
    if (!ttf || size == 0 || pixel_height <= 1.0f) {
        error = "The font blob was empty or the pixel height was invalid.";
        return false;
    }
    pixel_height_ = pixel_height;

    // A 1024x1024 shelf-packed atlas holds the 95 printable ASCII glyphs at typical UI sizes with
    // room to spare. The base size is deliberately larger than the smallest use (menu labels) so
    // scaling up to the title/logo sizes stays reasonably crisp (the caller scales by size/height).
    constexpr std::uint32_t kAtlasW = 1024;
    constexpr std::uint32_t kAtlasH = 1024;
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(kAtlasW) * kAtlasH, 0);

    stbtt_pack_context pack{};
    if (!stbtt_PackBegin(&pack, coverage.data(), static_cast<int>(kAtlasW),
                         static_cast<int>(kAtlasH), 0, 1, nullptr)) {
        error = "stbtt_PackBegin failed (atlas allocation).";
        return false;
    }
    std::array<stbtt_packedchar, kCount> packed{};
    const int ok = stbtt_PackFontRange(&pack, ttf, 0, pixel_height,
                                       static_cast<int>(kFirst), static_cast<int>(kCount),
                                       packed.data());
    stbtt_PackEnd(&pack);
    if (!ok) {
        error = "stbtt_PackFontRange failed (font did not fit / invalid TTF).";
        return false;
    }

    // The pack API yields glyph y-offsets relative to the BASELINE (negative = above it). ImGui's
    // atlas builder adds the font ascent so its ImFontGlyph.Y0/Y1 are relative to the TOP of the
    // line; we do the same here so emit_text can position text by its top-left (a drop-in for
    // ImGui::AddText). Without this, every migrated label would sit ~one ascent too high.
    stbtt_fontinfo info{};
    float ascent_px = 0.0f;
    if (stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        int ascent = 0, descent = 0, line_gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
        ascent_px = static_cast<float>(ascent) * stbtt_ScaleForPixelHeight(&info, pixel_height);
    }
    ascent_ = ascent_px;

    // Expand the single-channel coverage into an RGBA8 white sprite with alpha = coverage, so the
    // UI shader's `color * sampled` modulate tints the glyph and mattes it against the background.
    atlas_rgba8_.assign(static_cast<std::size_t>(kAtlasW) * kAtlasH * 4, 0);
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        atlas_rgba8_[i * 4 + 0] = 255;
        atlas_rgba8_[i * 4 + 1] = 255;
        atlas_rgba8_[i * 4 + 2] = 255;
        atlas_rgba8_[i * 4 + 3] = coverage[i];
    }
    atlas_width_ = kAtlasW;
    atlas_height_ = kAtlasH;

    // Paint a small fully-opaque white block in the bottom-right corner (well clear of the packed
    // glyphs) so callers can draw solid-color rects by sampling its center UV — no separate 1x1
    // white texture / descriptor needed for UI fills.
    for (std::uint32_t y = kAtlasH - 4; y < kAtlasH; ++y) {
        for (std::uint32_t x = kAtlasW - 4; x < kAtlasW; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kAtlasW + x) * 4;
            atlas_rgba8_[i + 0] = 255;
            atlas_rgba8_[i + 1] = 255;
            atlas_rgba8_[i + 2] = 255;
            atlas_rgba8_[i + 3] = 255;
        }
    }
    solid_u_ = (static_cast<float>(kAtlasW) - 2.0f) / static_cast<float>(kAtlasW);
    solid_v_ = (static_cast<float>(kAtlasH) - 2.0f) / static_cast<float>(kAtlasH);

    const float inv_w = 1.0f / static_cast<float>(kAtlasW);
    const float inv_h = 1.0f / static_cast<float>(kAtlasH);
    for (std::size_t i = 0; i < kCount; ++i) {
        const stbtt_packedchar& p = packed[i];
        Glyph g;
        g.valid = true;
        g.u0 = static_cast<float>(p.x0) * inv_w;
        g.v0 = static_cast<float>(p.y0) * inv_h;
        g.u1 = static_cast<float>(p.x1) * inv_w;
        g.v1 = static_cast<float>(p.y1) * inv_h;
        g.x0 = p.xoff;
        g.y0 = p.yoff + ascent_px;   // convert baseline-relative -> top-of-line relative (ImGui-style)
        g.x1 = p.xoff2;
        g.y1 = p.yoff2 + ascent_px;
        g.advance = p.xadvance;
        glyphs_[i] = g;
    }
    return true;
}

const NativeFont::Glyph& NativeFont::glyph(std::uint32_t codepoint) const noexcept {
    if (codepoint < kFirst || codepoint > kLast) return invalid_;
    return glyphs_[codepoint - kFirst];
}

float NativeFont::measure(std::string_view text, float size) const noexcept {
    if (pixel_height_ <= 0.0f) return 0.0f;
    const float scale = size / pixel_height_;
    float width = 0.0f;
    for (const char ch : text) {
        const Glyph& g = glyph(static_cast<std::uint8_t>(ch));
        if (g.valid) width += g.advance * scale;
    }
    return width;
}

}  // namespace f2
