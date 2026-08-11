#pragma once

#include "native_scene.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f2 {

class NativeWorldRenderer {
public:
    // Descriptor slots reserved for world material textures. Each material uses TWO
    // (albedo t0 + normal t1), so the material cap is kMaxMaterialTextures/2.
    static constexpr std::uint32_t kMaxMaterialTextures = 128;
    bool initialise(ID3D12Device* device,
                    ID3D12CommandQueue* queue,
                    const NativeScene& scene,
                    const std::filesystem::path& texture_root,
                    D3D12_CPU_DESCRIPTOR_HANDLE texture_cpu_handle,
                    D3D12_GPU_DESCRIPTOR_HANDLE texture_gpu_handle,
                    std::string& error);
    void render(ID3D12GraphicsCommandList* command_list,
                const NativeScene& scene,
                std::uint32_t width,
                std::uint32_t height,
                double elapsed_seconds);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textures_;
    D3D12_VERTEX_BUFFER_VIEW vertex_view_{};
    D3D12_INDEX_BUFFER_VIEW index_view_{};
    D3D12_GPU_VIRTUAL_ADDRESS constant_address_ = 0;
    std::uint32_t index_count_ = 0;
    // World-space bounds of the baked geometry, so the camera frames a real cooked
    // level (spanning hundreds of units) instead of the origin-orbit test default.
    std::array<float, 3> scene_center_{0.0f, 0.7f, 0.0f};
    float scene_radius_ = 4.0f;
    struct DrawRange {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
    };
    std::vector<DrawRange> draw_ranges_;
    void* mapped_constants_ = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE texture_gpu_handle_{};
    std::uint32_t texture_descriptor_stride_ = 0;
};

}  // namespace f2
