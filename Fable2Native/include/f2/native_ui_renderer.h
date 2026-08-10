#pragma once

#include "f2/render/render_backend.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace f2 {

struct NativeUiQuad {
    D3D12_GPU_DESCRIPTOR_HANDLE texture{};
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    std::uint32_t color = 0xffffffffu;
    float rotation_radians = 0.0f;
    D3D12_GPU_DESCRIPTOR_HANDLE detail_texture{};
    float detail_u0 = 0.0f;
    float detail_v0 = 0.0f;
    float detail_u1 = 1.0f;
    float detail_v1 = 1.0f;
    bool combine_detail = false;
    bool key_black_matte = false;
    bool alpha_mask = false;
    f2::render::BlendMode blend_mode = f2::render::BlendMode::Alpha;
};

class NativeUiRenderer {
public:
    // sample_count is the MSAA sample count the target render target uses (1 = no MSAA); the
    // pipelines are baked for it, so re-initialise when the Anti-Aliasing option changes.
    bool initialise(ID3D12Device* device, std::uint32_t sample_count, std::string& error);
    bool ready() const { return pipeline_state_ != nullptr; }
    [[nodiscard]] std::uint32_t sample_count() const noexcept { return sample_count_; }
    void render(ID3D12GraphicsCommandList* command_list,
                std::uint32_t width, std::uint32_t height,
                const std::vector<NativeUiQuad>& quads);

    // Backend-neutral overload: draw a shared UiScene (f2::render::UiQuad list) by resolving each
    // opaque TextureId to a D3D12 descriptor. This is the seam the render-backend migration uses so
    // the D3D12 backend can draw the same scene the Vulkan backend does (docs/FRONTEND_ARCHITECTURE.md).
    using TextureResolver = std::function<D3D12_GPU_DESCRIPTOR_HANDLE(f2::render::TextureId)>;
    void render(ID3D12GraphicsCommandList* command_list,
                std::uint32_t width, std::uint32_t height,
                std::span<const f2::render::UiQuad> quads,
                const TextureResolver& resolve);

private:
    struct Vertex {
        float x = 0.0f;
        float y = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
        float u = 0.0f;
        float v = 0.0f;
        float detail_u = 0.0f;
        float detail_v = 0.0f;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> additive_pipeline_state_;
    D3D12_VERTEX_BUFFER_VIEW vertex_view_{};
    Vertex* mapped_vertices_ = nullptr;
    std::size_t vertex_capacity_ = 0;
    std::uint32_t sample_count_ = 1;
};

}  // namespace f2
