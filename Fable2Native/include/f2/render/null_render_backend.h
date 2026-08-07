#pragma once

#include "f2/render/render_backend.h"

#include <cstdint>
#include <vector>

namespace f2::render {

// A headless IRenderBackend that renders nothing but records what it was asked to do. It lets the
// frontend logic + scene layer be exercised in tests/CI without a GPU, and is the reference that
// proves the backend contract is usable independent of D3D12/Vulkan.
class NullRenderBackend : public IRenderBackend {
public:
    [[nodiscard]] BackendCaps caps() const noexcept override {
        return BackendCaps{"null", false, 1};
    }

    bool initialize(void* /*window*/, std::uint32_t width, std::uint32_t height,
                    std::string& /*error*/) override {
        width_ = width;
        height_ = height;
        initialized_ = true;
        return true;
    }
    void resize(std::uint32_t width, std::uint32_t height) override {
        width_ = width;
        height_ = height;
    }
    void set_msaa(int samples) override { requested_msaa_ = samples; }

    TextureId create_texture(const std::uint8_t* /*rgba8*/, std::uint32_t width,
                             std::uint32_t height) override {
        const TextureId id = next_texture_id_++;
        texture_sizes_.emplace_back(width, height);
        return id;
    }
    void update_texture(TextureId /*id*/, const std::uint8_t* /*rgba8*/, std::uint32_t /*w*/,
                        std::uint32_t /*h*/) override {
        ++texture_updates_;
    }

    void begin_frame() override { ++frames_begun_; }
    void draw_ui(std::span<const UiQuad> quads, std::uint32_t /*vw*/, std::uint32_t /*vh*/) override {
        last_quad_count_ = quads.size();
        total_quads_ += quads.size();
    }
    void present() override { ++frames_presented_; }

    // Introspection for tests.
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] TextureId texture_count() const noexcept { return next_texture_id_ - 1; }
    [[nodiscard]] std::size_t last_quad_count() const noexcept { return last_quad_count_; }
    [[nodiscard]] std::size_t total_quads() const noexcept { return total_quads_; }
    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return frames_presented_; }
    [[nodiscard]] int requested_msaa() const noexcept { return requested_msaa_; }

private:
    bool initialized_ = false;
    std::uint32_t width_ = 0, height_ = 0;
    int requested_msaa_ = 1;
    TextureId next_texture_id_ = kInvalidTexture + 1;  // ids start at 1; 0 stays "no texture"
    std::vector<std::pair<std::uint32_t, std::uint32_t>> texture_sizes_;
    std::size_t last_quad_count_ = 0, total_quads_ = 0, texture_updates_ = 0;
    std::uint64_t frames_begun_ = 0, frames_presented_ = 0;
};

}  // namespace f2::render
