#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace f2 {

// Full HDR -> LDR COMPOSITOR (the retail final pass, ghidra_out/rendering_pipeline.txt §D.3:
// "result = mul_sat(exposure * sceneColor) + bloom"). Given the RGBA16F scene target that every
// World pass renders into (see native_scene_color.h), this produces the LDR back buffer via:
//   1. bright-pass  : threshold the HDR scene into a half-res bloom target
//   2. blur (H, V)  : separable Gaussian on the bright target (ping-pong)
//   3. composite    : saturate(exposure * scene) + bloom_intensity * bloom  -> back buffer
//
// Owns its own bloom targets + descriptor/RTV heaps (self-contained like the sky/cloud renderers);
// it only needs the app's HDR scene resource (set via ensure_targets) and the back-buffer RTV.
// Default exposure 1.0 + bloom_intensity 0.0 reproduces the old direct-to-UNORM clamp bit-for-bit.
class NativeTonemapRenderer {
public:
    bool initialise(ID3D12Device* device, DXGI_FORMAT output_format, std::string& error);
    bool ready() const { return composite_pso_ != nullptr; }

    // (Re)create the bloom targets + SRVs for the current size, viewing the app's HDR scene target.
    // Call after the HDR scene target is (re)created. Safe to call every resize.
    void ensure_targets(ID3D12Device* device, ID3D12Resource* scene_target, std::uint32_t width,
                        std::uint32_t height);

    // Resolve the HDR scene to the LDR back buffer (back_buffer_rtv). The caller must have
    // transitioned the scene target to PIXEL_SHADER_RESOURCE and the back buffer to RENDER_TARGET.
    // Binds this renderer's own descriptor heap; the caller re-binds its heap afterwards.
    void render(ID3D12GraphicsCommandList* command_list, D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv,
                std::uint32_t width, std::uint32_t height, float exposure, float bloom_threshold,
                float bloom_intensity);

private:
    // One image with a render-target view and a shader-resource view (a bloom-chain target).
    struct Target {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        D3D12_GPU_DESCRIPTOR_HANDLE srv{};
        std::uint32_t srv_index = 0;
    };

    void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                 D3D12_RESOURCE_STATES after);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> bright_pso_;     // HDR scene -> bright target
    Microsoft::WRL::ComPtr<ID3D12PipelineState> blur_pso_;       // separable Gaussian
    Microsoft::WRL::ComPtr<ID3D12PipelineState> composite_pso_;  // scene + bloom -> back buffer

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;  // shader-visible: scene + 3 bloom SRVs
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;  // 3 bloom RTVs
    UINT srv_stride_ = 0;
    UINT rtv_stride_ = 0;

    D3D12_GPU_DESCRIPTOR_HANDLE scene_srv_{};  // over the app's HDR scene target
    Target bright_;
    Target blur_h_;
    Target blur_v_;
    std::uint32_t bloom_width_ = 0;
    std::uint32_t bloom_height_ = 0;
};

}  // namespace f2
