#pragma once

#include "native_scene.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace f2 {

// Scrolling cloud-layer pass (theme Clouds; retail reference: Fable2AssetBrowser
// SkyboxRenderer.cpp kCloudPixelShader + CloudRuntime). Drawn AFTER the sky atmosphere and
// BEFORE the opaque world: each layer is one flat alpha-blended quad at its authored height,
// centred at the world origin, no depth test/write, sorted high->low (back-to-front). Owns its
// root signature, PSO, cbuffer, a dynamic 4-vertex buffer, a static index buffer, and a
// descriptor heap of per-layer density SRVs. Rebuilds its per-layer textures when the scene's
// cloud set changes. A no-op when the scene has no cloud layers, so non-cloud scenes are
// unaffected.
class NativeCloudRenderer {
public:
    bool initialise(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& error);

    // Draw the scene's cloud layers over the already-drawn sky. Binds and leaves its OWN
    // descriptor heap set (the caller re-binds the app heap afterwards, like the sky pass).
    // `view_projection` is the world renderer's exact row-major matrix; `eye`/`forward` its
    // camera. No-op until initialise() succeeds and the scene has cloud layers.
    void render(ID3D12GraphicsCommandList* command_list,
                const NativeScene& scene,
                const std::array<float, 16>& view_projection,
                const std::array<float, 3>& eye,
                const std::array<float, 3>& forward,
                std::uint32_t width,
                std::uint32_t height,
                double elapsed_seconds);

    bool ready() const { return pipeline_state_ != nullptr; }

private:
    // Lazily (re)upload one density texture per cloud layer when the scene changes. Populates
    // layer_ready_ (a layer with a texture that failed to load is skipped at draw time).
    void ensure_scene(ID3D12Device* device, ID3D12CommandQueue* queue, const NativeScene& scene);

    static constexpr std::uint32_t kMaxLayers = 4;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_;   // ring of kMaxLayers cbuffers
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer_;     // dynamic, 4 verts * kMaxLayers
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer_;      // static {0,1,2,1,3,2}
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;    // kMaxLayers density SRVs
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kMaxLayers> density_textures_;
    std::array<bool, kMaxLayers> layer_ready_{};
    std::array<std::string, kMaxLayers> layer_texture_name_{};
    D3D12_INDEX_BUFFER_VIEW index_view_{};
    D3D12_GPU_VIRTUAL_ADDRESS constant_address_ = 0;
    void* mapped_constants_ = nullptr;
    void* mapped_vertices_ = nullptr;
    std::uint32_t srv_descriptor_size_ = 0;
    std::uint32_t constant_stride_ = 0;
    const NativeScene* bound_scene_ = nullptr;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
};

}  // namespace f2
