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

    // Sun shadow map (retail "Render ShadowBuffers"): the frontend owns a depth target + its DSV
    // and a shader-readable SRV; the renderer replays the opaque geometry into it from the sun's
    // ortho POV (render_shadow) and samples it in the world PS. size = square resolution.
    void set_shadow_map(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_GPU_DESCRIPTOR_HANDLE srv,
                        std::uint32_t size) {
        shadow_dsv_ = dsv;
        shadow_srv_gpu_ = srv;
        shadow_size_ = size;
    }
    // Depth-only shadow pass: render opaque geometry from the sun POV into the shadow DSV. Call
    // before render(); the frontend transitions the shadow target DEPTH_WRITE -> PIXEL_SHADER between.
    void render_shadow(ID3D12GraphicsCommandList* command_list, const NativeScene& scene,
                       double elapsed_seconds);

    // The frontend supplies a shader-readable copy of the World depth buffer. It is copied
    // after opaque geometry and sampled by the water pass for the shoreline edge factor.
    void set_scene_depth_copy(ID3D12Resource* source, ID3D12Resource* copy,
                              D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
        scene_depth_source_ = source;
        scene_depth_copy_ = copy;
        scene_depth_gpu_handle_ = gpu_handle;
    }

    // The orbit camera render() flies each frame, exposed so a companion pass (the sky)
    // can build rays from the EXACT same basis. Uses the same fit-to-scene framing.
    SkyCamera compute_camera(std::uint32_t width, std::uint32_t height,
                             double elapsed_seconds) const;

    // The exact row-major reversed-Z view_projection render() builds this frame, flattened
    // [row*4+col], so a companion pass (clouds) can project real geometry to line up
    // pixel-for-pixel with the world. Shares compute_camera() + the same near/far framing.
    std::array<float, 16> compute_view_projection(std::uint32_t width, std::uint32_t height,
                                                  double elapsed_seconds) const;

    // Sun ortho view-projection used for the shadow map (fits a box around the scene along the sun
    // travel dir, standard Z). Public so the shadow pass and PS share the exact same transform.
    std::array<float, 16> compute_light_view_projection(const std::array<float, 3>& sun) const;

    // Free-fly camera override (for level inspection). When set, compute_camera and
    // render() use this eye+yaw+pitch basis instead of the auto-orbit; the sky follows
    // since it shares compute_camera. Cleared back to orbit with clear_free_camera().
    void set_free_camera(const std::array<float, 3>& eye, float yaw, float pitch) {
        free_camera_ = true;
        free_eye_ = eye;
        free_yaw_ = yaw;
        free_pitch_ = pitch;
    }
    void clear_free_camera() noexcept { free_camera_ = false; }
    void set_character_offset(const std::array<float, 3>& offset) noexcept {
        character_offset_ = offset;
    }
    void set_character_motion(float phase, float strength) noexcept {
        character_motion_phase_ = phase;
        character_motion_strength_ = strength;
    }
    [[nodiscard]] const std::array<float, 3>& scene_center() const noexcept { return scene_center_; }
    [[nodiscard]] float scene_radius() const noexcept { return scene_radius_; }

private:
    // Point-light array cap (b1 Lights cbuffer). 64 covers a town square; the cooker
    // caps to the brightest 64 (level_lights_effects_re.txt §3.1).
    static constexpr std::uint32_t kMaxPointLights = 64;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> light_buffer_;  // b1 point-light array
    Microsoft::WRL::ComPtr<ID3D12Resource> water_buffer_;  // b2 authored WaterFile params
    D3D12_GPU_VIRTUAL_ADDRESS light_address_ = 0;
    D3D12_GPU_VIRTUAL_ADDRESS water_address_ = 0;
    ID3D12Resource* scene_depth_source_ = nullptr;
    ID3D12Resource* scene_depth_copy_ = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE scene_depth_gpu_handle_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> water_pipeline_;  // translucent animated water
    Microsoft::WRL::ComPtr<ID3D12RootSignature> shadow_root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadow_pipeline_;  // depth-only sun shadow pass
    D3D12_CPU_DESCRIPTOR_HANDLE shadow_dsv_{};       // frontend-owned shadow depth DSV
    D3D12_GPU_DESCRIPTOR_HANDLE shadow_srv_gpu_{};   // frontend-owned shadow depth SRV (t4)
    std::uint32_t shadow_size_ = 0;                  // square shadow-map resolution (0 = disabled)
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textures_;
    D3D12_VERTEX_BUFFER_VIEW vertex_view_{};
    D3D12_INDEX_BUFFER_VIEW index_view_{};
    D3D12_GPU_VIRTUAL_ADDRESS constant_address_ = 0;
    std::uint32_t index_count_ = 0;
    // World-space bounds of the baked geometry, so the camera frames a real cooked
    // level (spanning hundreds of units) instead of the origin-orbit test default.
    std::array<float, 3> scene_center_{0.0f, 0.7f, 0.0f};
    float scene_radius_ = 4.0f;
    // Free-fly camera override state (see set_free_camera).
    bool free_camera_ = false;
    std::array<float, 3> free_eye_{0.0f, 0.0f, 0.0f};
    float free_yaw_ = 0.0f;
    float free_pitch_ = 0.0f;
    std::array<float, 3> character_offset_{0.0f, 0.0f, 0.0f};
    float character_motion_phase_ = 0.0f;
    float character_motion_strength_ = 0.0f;
    struct DrawRange {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
        bool is_water = false;  // drawn in the translucent water pass with the water shader
        bool is_character = false;
        // World-space bounding sphere + per-instance max draw distance (0 = never cull) for the
        // distance-culling LOD gate (retail draw-distance). fable2-npc-popin-drawdistance-fix.
        std::array<float, 3> center{};
        float radius = 0.0f;
        float max_draw_distance = 0.0f;
    };
    std::vector<DrawRange> draw_ranges_;
    std::array<float, 3> camera_eye_{0.0f, 0.0f, 0.0f};  // last frame's eye, for shadow-pass culling
    void* mapped_constants_ = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE texture_gpu_handle_{};
    std::uint32_t texture_descriptor_stride_ = 0;
};

}  // namespace f2
