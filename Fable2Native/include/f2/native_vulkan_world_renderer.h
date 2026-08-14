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
    void destroy();

    // Camera basis for the sky pass (same orbit/free-fly logic the world pass uses), so the
    // sky's fullscreen-triangle rays line up with world geometry. Mirrors D3D12's compute_camera.
    [[nodiscard]] SkyCamera compute_camera(std::uint32_t width, std::uint32_t height,
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
    [[nodiscard]] const std::array<float, 3>& scene_center() const noexcept { return scene_center_; }
    [[nodiscard]] float scene_radius() const noexcept { return scene_radius_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
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
    };
    std::vector<DrawRange> draw_ranges_;

    void render_pass(VkCommandBuffer command_buffer,
                     std::uint32_t width,
                     std::uint32_t height,
                     double elapsed_seconds,
                     bool water);
};

}  // namespace f2
