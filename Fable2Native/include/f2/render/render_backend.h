#pragma once

#include <cstdint>
#include <span>
#include <string>

// f2::render — the backend-neutral presentation seam (the f2render layer, docs/NATIVE_PORT_PLAN.md).
// The frontend logic (FrontendController) and the UI *scene* (a list of UiQuads) know nothing about
// D3D12 or Vulkan; a concrete IRenderBackend consumes the scene. This is what lets one executable
// host both backends behind a restart-applied toggle, and what keeps the UI moddable: the scene is
// data (asset ids + rects + uvs + color), not backend calls.
namespace f2::render {

// Opaque, backend-neutral texture handle. Backends map it to their own resource (D3D12 descriptor,
// Vulkan descriptor set, …). 0 is the reserved "no texture" value.
using TextureId = std::uint32_t;
inline constexpr TextureId kInvalidTexture = 0;

// One backend-neutral UI draw command. Coordinates are in pixels of the current viewport; UVs are
// 0..1. `detail_texture` + `combine_detail` express the retail title/menu two-layer material
// (out.rgb = detail.rgb*k + detail.a*main.rgb); `key_black_matte` keys pure black to transparent.
struct UiQuad {
    TextureId texture = kInvalidTexture;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;   // destination rect (pixels)
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;   // primary UV
    std::uint32_t color = 0xffffffffu;                  // RGBA8 modulate
    float rotation_radians = 0.0f;
    TextureId detail_texture = kInvalidTexture;
    float detail_u0 = 0.0f, detail_v0 = 0.0f, detail_u1 = 1.0f, detail_v1 = 1.0f;
    bool combine_detail = false;
    bool key_black_matte = false;
};

// What a backend can do. The Options AA setting consults msaa/max_msaa_samples; an unsupported
// setting is disabled in the menu rather than faked.
struct BackendCaps {
    const char* name = "none";
    bool supports_msaa = false;
    int max_msaa_samples = 1;
};

// The presentation backend contract. One instance owns the device/swapchain for the process
// lifetime (the D3D12/Vulkan choice is restart-applied, so no live teardown/recreate is required).
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    [[nodiscard]] virtual BackendCaps caps() const noexcept = 0;

    // window_handle is the native window (HWND on Windows), passed as void* to keep this header
    // free of platform headers. Returns false + fills `error` on failure.
    virtual bool initialize(void* window_handle, std::uint32_t width, std::uint32_t height,
                            std::string& error) = 0;
    virtual void resize(std::uint32_t width, std::uint32_t height) = 0;

    // Request an MSAA sample count for UI/scene rendering. Backends clamp to caps().max_msaa_samples
    // and no-op if unsupported. Restart-applied backends may defer application to the next frame.
    virtual void set_msaa(int samples) = 0;

    // Upload an RGBA8 image (width*height*4 bytes) and return a stable TextureId; update replaces the
    // pixels of an existing id (used for the streaming video frame).
    virtual TextureId create_texture(const std::uint8_t* rgba8, std::uint32_t width,
                                     std::uint32_t height) = 0;
    virtual void update_texture(TextureId id, const std::uint8_t* rgba8, std::uint32_t width,
                                std::uint32_t height) = 0;

    // Per-frame: begin (acquire + clear), draw the UI scene in a [0,viewport_w]×[0,viewport_h] pixel
    // space, then present. Video is just quads referencing a create/update_texture id.
    virtual void begin_frame() = 0;
    virtual void draw_ui(std::span<const UiQuad> quads, std::uint32_t viewport_width,
                         std::uint32_t viewport_height) = 0;
    virtual void present() = 0;
};

}  // namespace f2::render
