#pragma once

#include "native_scene.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>

namespace f2 {

// Camera basis the sky pass needs, in the SAME convention the world renderer uses
// (Y-up render space). NativeWorldRenderer::compute_camera fills this so the sky's
// fullscreen-triangle rays line up pixel-for-pixel with the world geometry.
struct SkyCamera {
    std::array<float, 3> position{};   // eye, world/render space
    std::array<float, 3> right{};      // orthonormal camera basis
    std::array<float, 3> up{};
    std::array<float, 3> forward{};
    float tan_half_fov_x = 1.0f;       // right.w
    float tan_half_fov_y = 1.0f;       // up.w
};

// A self-contained procedural-sky pass: one fullscreen triangle drawn BEHIND the world
// (no depth test/write) that runs the ported SkyDomeXex analytic atmosphere shader.
// Ships Phase 1 (analytic atmosphere) with a Phase 0 gradient fallback compiled into the
// same PS. Owns its own root signature, PSO, cbuffer, in-scatter LUT texture, and a small
// shader-visible descriptor heap (so it does not perturb the app's shared SRV heap layout).
class NativeSkyRenderer {
public:
    bool initialise(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& error);

    // Draw the sky. Must be called with the render target (and optional depth) already
    // bound and the viewport/scissor set by the caller, BEFORE world geometry. Binds and
    // leaves its own descriptor heap set — the caller re-binds the app heap afterwards.
    void render(ID3D12GraphicsCommandList* command_list,
                const NativeScene& scene,
                const SkyCamera& camera,
                std::uint32_t width,
                std::uint32_t height,
                double elapsed_seconds);

    bool ready() const { return pipeline_state_ != nullptr; }

private:
    bool build_lut_texture(ID3D12Device* device, ID3D12CommandQueue* queue, std::string& error);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> lut_texture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> lut_upload_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
    D3D12_GPU_DESCRIPTOR_HANDLE lut_gpu_handle_{};
    D3D12_GPU_VIRTUAL_ADDRESS constant_address_ = 0;
    void* mapped_constants_ = nullptr;
};

}  // namespace f2
