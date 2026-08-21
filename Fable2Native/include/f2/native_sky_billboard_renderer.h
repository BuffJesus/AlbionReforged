#pragma once

#include "native_scene.h"
#include "sky_camera.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>

namespace f2 {

// Celestial billboard pass (theme Sky element pass; retail reference: Fable2AssetBrowser
// SkyboxRenderer.cpp draw_billboard @1558-1734 + SkyXex::k*Distance/SizeScale). Draws the moon
// (a phase billboard, alpha-blended) + its glare halo (additive) as camera-facing quads, after
// the clouds and behind the world. Screen-space quads (positions built on the CPU from the shared
// SkyCamera basis, exactly like the retail draw_billboard), so it needs no view-projection.
// A no-op unless the scene authors a moon (has_moon → night). Owns its own root sig / PSOs /
// cbuffer / dynamic vertex buffer / descriptor heap; binds and leaves its own heap like the sky.
class NativeSkyBillboardRenderer {
public:
    bool initialise(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& error);
    void render(ID3D12GraphicsCommandList* command_list,
                const NativeScene& scene,
                const SkyCamera& camera,
                std::uint32_t width,
                std::uint32_t height);
    bool ready() const { return alpha_pso_ != nullptr; }

private:
    void ensure_scene(ID3D12Device* device, ID3D12CommandQueue* queue, const NativeScene& scene);

    static constexpr std::uint32_t kMaxBillboards = 2;  // moon + moon glare

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> alpha_pso_;     // moon disc (SRC_ALPHA/INV)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> additive_pso_;  // glare (SRC_ALPHA/ONE)
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_;    // ring of kMaxBillboards cbuffers
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer_;      // dynamic, 6 verts * kMaxBillboards
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;     // moon + glare SRVs
    Microsoft::WRL::ComPtr<ID3D12Resource> moon_texture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> glare_texture_;
    bool moon_ready_ = false;
    bool glare_ready_ = false;
    std::string moon_name_;
    std::string glare_name_;
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
