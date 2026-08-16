#pragma once

#include "native_scene.h"
#include "sky_camera.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f2 {

class NativeVulkanWorldRenderer {
public:
    // The full chapter2slums cook emits ~661 materials. Each material owns albedo, normal,
    // and specular resources; this Vulkan cap is expressed in material count (not descriptor
    // slots), unlike the D3D12 descriptor-heap cap.
    static constexpr std::uint32_t kMaxMaterialTextures = 4096;
    bool initialise(VkPhysicalDevice physical_device,
                    VkDevice device,
                    VkCommandPool command_pool,
                    VkQueue queue,
                    VkRenderPass render_pass,
                    VkFormat color_format,
                    VkSampleCountFlagBits samples,
                    VkImageView depth_resolve_view,
                    const std::filesystem::path& texture_root,
                    const std::filesystem::path& shader_directory,
                    const NativeScene& scene,
                    std::string& error);
    void render(VkCommandBuffer command_buffer,
                std::uint32_t width,
                std::uint32_t height,
                double elapsed_seconds);
    // MSAA frames use two render-pass subpasses. Keep the combined entry point for the
    // single-subpass path while allowing the frontend to place opaque and water draws in
    // their matching subpasses.
    void render_opaque(VkCommandBuffer command_buffer,
                       std::uint32_t width,
                       std::uint32_t height,
                       double elapsed_seconds);
    void render_water(VkCommandBuffer command_buffer,
                      std::uint32_t width,
                      std::uint32_t height,
                      double elapsed_seconds);
    // Depth-only sun shadow pass (retail "Render ShadowBuffers"): replay opaque geometry from the
    // sun's ortho POV into the renderer-owned shadow depth map. Runs its OWN render pass, so the
    // app must call this BEFORE beginning the world/HDR render pass. Gated on the sun being above
    // the horizon (sun.y < -0.05); a no-op otherwise. The light VP is written into the shared
    // camera UBO here so both this pass and the world frag term use the exact same transform.
    void render_shadow(VkCommandBuffer command_buffer,
                       std::uint32_t width,
                       std::uint32_t height,
                       double elapsed_seconds);
    // Planar reflection pass (retail g_ReflectionSampler, PSHADER_WATERPATCH): replay opaque
    // geometry MIRRORED about the water plane into a renderer-owned colour RT (own render pass, like
    // the shadow pass), then the water frag samples it. Call BEFORE the world render pass. No-op if
    // the scene has no water. D3D12 parity (there the target is frontend-owned; here renderer-owned,
    // matching the shadow-pass ownership asymmetry).
    void render_reflection(VkCommandBuffer command_buffer,
                           std::uint32_t width,
                           std::uint32_t height,
                           double elapsed_seconds);
    // Refraction tile (retail program 57 g_RefractionSampler c14): replay opaque geometry with the
    // NORMAL camera (no mirror/clip) into a renderer-owned colour tile = the scene BEHIND the water.
    // Vulkan can't copy a colour attachment mid-render-pass (D3D12's grab-pass approach), so it
    // re-renders opaque into an isolated pass instead (reuses the reflection render pass). Call BEFORE
    // the world render pass. No-op if the scene has no water.
    void render_refraction(VkCommandBuffer command_buffer,
                           std::uint32_t width,
                           std::uint32_t height,
                           double elapsed_seconds);
    [[nodiscard]] bool has_water() const noexcept { return has_water_; }
    [[nodiscard]] float water_plane_y() const noexcept { return water_plane_y_; }
    // Sun ortho view-projection used for the shadow map (fits a box around the scene along the sun
    // travel dir, standard Z in [0,1]). Public so the shadow pass and frag term share it. Mirrors
    // the D3D12 renderer's compute_light_view_projection.
    [[nodiscard]] std::array<float, 16> compute_light_view_projection(
        const std::array<float, 3>& sun) const;
    void destroy();

    // Camera basis for the sky pass (same orbit/free-fly logic the world pass uses), so the
    // sky's fullscreen-triangle rays line up with world geometry. Mirrors D3D12's compute_camera.
    [[nodiscard]] SkyCamera compute_camera(std::uint32_t width, std::uint32_t height,
                                           double elapsed_seconds) const;

    // The exact column-major (GLSL) view_projection render_pass() builds this frame, so a
    // companion pass (clouds) projects real geometry to line up with the world. Mirrors the
    // matrix render_pass() uses (projection * view, reversed-Z).
    [[nodiscard]] std::array<float, 16> compute_view_projection(std::uint32_t width,
                                                                std::uint32_t height,
                                                                double elapsed_seconds) const;

    // Free-fly camera override (level inspection) — mirrors the D3D12 renderer.
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
    // Dynamic-mesh (hero skinning) display path — rewrites character mesh `mesh_index`'s vertices in
    // the host-visible vertex buffer from AnimationPlayer::skin()'s MODEL-space positions (renderer
    // applies the hero instance transform). No-op if unused. D3D12 parity. docs/RENDERER_INTERFACE.md.
    void set_character_pose(std::size_t mesh_index,
                            const std::vector<std::array<float, 3>>& model_positions);
    [[nodiscard]] std::size_t character_mesh_count() const noexcept { return character_meshes_.size(); }
    [[nodiscard]] std::uint32_t character_mesh_vertex_count(std::size_t i) const noexcept {
        return i < character_meshes_.size() ? character_meshes_[i].vertex_count : 0u;
    }
    [[nodiscard]] const std::array<float, 3>& scene_center() const noexcept { return scene_center_; }
    [[nodiscard]] float scene_radius() const noexcept { return scene_radius_; }

    // Square sun shadow-map resolution (retail Render ShadowBuffers). Matches the D3D12 kShadowSize.
    static constexpr std::uint32_t kShadowSize = 2048;
    // Square planar reflection RT resolution. Matches the D3D12 kReflectionSize.
    static constexpr std::uint32_t kReflectionSize = 1024;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    // Sun shadow map: a renderer-owned D32 depth image rendered from the sun POV each frame in its
    // own render pass, then sampled by the world frag shader (sampler2DShadow, binding 7).
    VkImage shadow_image_ = VK_NULL_HANDLE;
    VkDeviceMemory shadow_memory_ = VK_NULL_HANDLE;
    VkImageView shadow_view_ = VK_NULL_HANDLE;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;  // comparison sampler (hardware PCF)
    VkRenderPass shadow_render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer shadow_framebuffer_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_ = VK_NULL_HANDLE;  // depth-only
    bool shadow_ready_ = false;
    // Planar reflection RT (renderer-owned, like the shadow map): a colour image + its own depth,
    // an own render pass/framebuffer, a linear sampler, and a pipeline (world shaders, single-sample).
    VkImage reflection_image_ = VK_NULL_HANDLE;
    VkDeviceMemory reflection_memory_ = VK_NULL_HANDLE;
    VkImageView reflection_view_ = VK_NULL_HANDLE;
    VkSampler reflection_sampler_ = VK_NULL_HANDLE;
    VkImage reflection_depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory reflection_depth_memory_ = VK_NULL_HANDLE;
    VkImageView reflection_depth_view_ = VK_NULL_HANDLE;
    VkRenderPass reflection_render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer reflection_framebuffer_ = VK_NULL_HANDLE;
    VkPipeline reflection_pipeline_ = VK_NULL_HANDLE;
    bool reflection_ready_ = false;
    // Refraction tile (scene behind water). Reuses reflection_render_pass_ + reflection_pipeline_
    // (same opaque world shaders); only a distinct colour+depth image + framebuffer are needed.
    VkImage refraction_image_ = VK_NULL_HANDLE;
    VkDeviceMemory refraction_memory_ = VK_NULL_HANDLE;
    VkImageView refraction_view_ = VK_NULL_HANDLE;
    VkImage refraction_depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory refraction_depth_memory_ = VK_NULL_HANDLE;
    VkImageView refraction_depth_view_ = VK_NULL_HANDLE;
    VkFramebuffer refraction_framebuffer_ = VK_NULL_HANDLE;
    bool refraction_ready_ = false;
    // Derived single water plane (render Y) = radius-weighted mean of the water draw ranges (phase 1;
    // multi-height canals collapse here). Second UBO slice carries the reflected VP (dynamic offset).
    bool has_water_ = false;
    float water_plane_y_ = 0.0f;
    VkDeviceSize reflection_ubo_offset_ = 0;  // dynamic offset of the reflection Camera UBO slice
    void* mapped_reflection_constants_ = nullptr;
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
    VkBuffer index_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
    VkBuffer constant_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory constant_memory_ = VK_NULL_HANDLE;
    void* mapped_constants_ = nullptr;
    VkBuffer lights_buffer_ = VK_NULL_HANDLE;   // b1-equivalent point-light UBO (static)
    VkDeviceMemory lights_memory_ = VK_NULL_HANDLE;
    // Per-material authored WaterFile params + WaterTheme opacity (binding 5).
    std::vector<VkBuffer> water_buffers_;
    std::vector<VkDeviceMemory> water_memories_;
    std::array<float, 3> sun_direction_{0.0f, -1.0f, 0.0f};  // from the cooked scene
    std::array<float, 3> sun_color_{1.0f, 1.0f, 1.0f};
    std::array<float, 3> scene_fog_color_{0.0f, 0.0f, 0.0f};  // distance fog (0 max = off)
    float scene_fog_start_ = 0.0f;
    float scene_fog_end_ = 1.0f;
    float scene_fog_max_ = 0.0f;
    // Theme sky endpoints -> the water reflection tracks the real rendered sky per time-of-day.
    std::array<float, 4> scene_sky_zenith_{0.6549f, 0.8157f, 1.0f, 1.0f};
    std::array<float, 3> scene_sky_horizon_{0.222f, 0.5789f, 1.11f};
    std::array<float, 3> scene_center_{0.0f, 0.0f, 0.0f};    // geometry bounds -> auto-frame camera
    float scene_radius_ = 1.0f;
    bool free_camera_ = false;
    std::array<float, 3> free_eye_{0.0f, 0.0f, 0.0f};
    float free_yaw_ = 0.0f;
    float free_pitch_ = 0.0f;
    std::array<float, 3> character_offset_{0.0f, 0.0f, 0.0f};
    float character_motion_phase_ = 0.0f;
    float character_motion_strength_ = 0.0f;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptor_sets_;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline water_pipeline_ = VK_NULL_HANDLE;
    VkImageView depth_resolve_view_ = VK_NULL_HANDLE;
    bool depth_resolve_enabled_ = false;
    std::vector<VkImage> texture_images_;
    std::vector<VkDeviceMemory> texture_memories_;
    std::vector<VkImageView> texture_views_;
    std::vector<VkSampler> texture_samplers_;
    // Per-material tangent-space normal maps (t1), parallel to the albedo arrays above.
    std::vector<VkImage> normal_images_;
    std::vector<VkDeviceMemory> normal_memories_;
    std::vector<VkImageView> normal_views_;
    std::vector<VkSampler> normal_samplers_;
    std::vector<VkImage> spec_images_;
    std::vector<VkDeviceMemory> spec_memories_;
    std::vector<VkImageView> spec_views_;
    std::vector<VkSampler> spec_samplers_;
    std::uint32_t index_count_ = 0;
    struct DrawRange {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
        bool is_water = false;
        bool is_character = false;
        std::array<float, 3> center{};
        float radius = 0.0f;
        float max_draw_distance = 0.0f;  // 0 = never cull (retail draw-distance LOD gate)
    };
    std::vector<DrawRange> draw_ranges_;
    struct CharacterMesh {
        std::uint32_t base_vertex = 0;
        std::uint32_t vertex_count = 0;
        std::array<float, 3> rotation{};
        float scale = 1.0f;
        std::array<float, 3> position{};
    };
    std::vector<CharacterMesh> character_meshes_;
    std::uint32_t vertex_count_ = 0;
    std::array<float, 3> camera_eye_{0.0f, 0.0f, 0.0f};  // last eye, for shadow-pass culling

    void render_pass(VkCommandBuffer command_buffer,
                     std::uint32_t width,
                     std::uint32_t height,
                     double elapsed_seconds,
                     bool water);
};

}  // namespace f2
