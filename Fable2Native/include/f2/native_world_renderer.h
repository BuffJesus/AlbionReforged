#pragma once

#include "native_scene.h"
#include "native_sky_renderer.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f2 {

class NativeWorldRenderer {
public:
    // Descriptor slots reserved for world material textures. Each material uses THREE
    // (albedo t0 + normal t1 + spec/"material" t2), so the material cap is
    // kMaxMaterialTextures/3. The full chapter2slums cook (props + terrain + vista + hero +
    // NPCs) emits ~661 materials; props are emitted LAST, so a low cap clamped materials 128+
    // to a wrong texture → near-black props (ghidra_out/dark_props_diagnosis.txt). 6144 =
    // 2048 materials, with headroom. The SRV heap auto-sizes off this constant
    // (native_frontend_app.cpp:464).
    static constexpr std::uint32_t kMaxMaterialTextures = 6144;
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

    // The orbit camera render() flies each frame, exposed so a companion pass (the sky)
    // can build rays from the EXACT same basis. Uses the same fit-to-scene framing.
    SkyCamera compute_camera(std::uint32_t width, std::uint32_t height,
                             double elapsed_seconds) const;

private:
    // Point-light array cap (b1 Lights cbuffer). 64 covers a town square; the cooker
    // caps to the brightest 64 (level_lights_effects_re.txt §3.1).
    static constexpr std::uint32_t kMaxPointLights = 64;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> light_buffer_;  // b1 point-light array
    D3D12_GPU_VIRTUAL_ADDRESS light_address_ = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> water_pipeline_;  // translucent animated water
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
        bool is_water = false;  // drawn in the translucent water pass with the water shader
    };
    std::vector<DrawRange> draw_ranges_;
    void* mapped_constants_ = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE texture_gpu_handle_{};
    std::uint32_t texture_descriptor_stride_ = 0;
};

}  // namespace f2
