#pragma once

#include "f2/render/render_backend.h"

#include <cstdint>
#include <vector>

namespace f2::render {

// A backend-neutral UI scene: the ordered list of quads a shared scene builder produces from the
// FrontendController state (and, later, from moddable layout data). Any IRenderBackend consumes it
// via draw_ui(list.quads(), w, h). Nothing here knows about D3D12/Vulkan — this is the seam that lets
// one scene render identically on both backends and lets mods relayout/reskin at the data level.
class UiDrawList {
public:
    void clear() noexcept { quads_.clear(); }
    [[nodiscard]] bool empty() const noexcept { return quads_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return quads_.size(); }
    [[nodiscard]] std::span<const UiQuad> quads() const noexcept { return quads_; }

    // Append a raw quad.
    void add(const UiQuad& quad) { quads_.push_back(quad); }

    // A textured sprite: destination rect (pixels) + UV rect (0..1) + RGBA8 modulate.
    void add_sprite(TextureId texture, float x0, float y0, float x1, float y1, float u0 = 0.0f,
                    float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f,
                    std::uint32_t color = 0xffffffffu) {
        UiQuad q;
        q.texture = texture;
        q.x0 = x0; q.y0 = y0; q.x1 = x1; q.y1 = y1;
        q.u0 = u0; q.v0 = v0; q.u1 = u1; q.v1 = v1;
        q.color = color;
        quads_.push_back(q);
    }

    // A solid-color rect (no texture): a 1x1 white texture id modulated by `color`.
    void add_rect(TextureId white_texture, float x0, float y0, float x1, float y1,
                  std::uint32_t color) {
        add_sprite(white_texture, x0, y0, x1, y1, 0.0f, 0.0f, 1.0f, 1.0f, color);
    }

    // The retail two-layer/detail material (title reveal, menu rows): main sprite combined with a
    // detail sprite (out.rgb = detail.rgb*k + detail.a*main.rgb). `key_black` mattes pure black out.
    void add_detail_sprite(TextureId main_texture, TextureId detail_texture, float x0, float y0,
                           float x1, float y1, float u0, float v0, float u1, float v1, float du0,
                           float dv0, float du1, float dv1, std::uint32_t color = 0xffffffffu,
                           bool key_black = false) {
        UiQuad q;
        q.texture = main_texture;
        q.x0 = x0; q.y0 = y0; q.x1 = x1; q.y1 = y1;
        q.u0 = u0; q.v0 = v0; q.u1 = u1; q.v1 = v1;
        q.color = color;
        q.detail_texture = detail_texture;
        q.detail_u0 = du0; q.detail_v0 = dv0; q.detail_u1 = du1; q.detail_v1 = dv1;
        q.combine_detail = true;
        q.key_black_matte = key_black;
        quads_.push_back(q);
    }

    // Set the rotation (radians) of the most recently appended quad (sparkle/star effects).
    void set_last_rotation(float radians) {
        if (!quads_.empty()) quads_.back().rotation_radians = radians;
    }

private:
    std::vector<UiQuad> quads_;
};

}  // namespace f2::render
