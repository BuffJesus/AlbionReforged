#pragma once

#include "native_scene.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f2 {

class NativeVulkanWorldRenderer {
public:
    // See native_world_renderer.h: the full chapter2slums cook emits ~661 materials
    // (2 textures each); a low cap clamps later (prop) materials to a wrong texture →
    // near-black props. 4096 = 2048 materials (ghidra_out/dark_props_diagnosis.txt).
    static constexpr std::uint32_t kMaxMaterialTextures = 4096;
    bool initialise(VkPhysicalDevice physical_device,
                    VkDevice device,
                    VkCommandPool command_pool,
                    VkQueue queue,
                    VkRenderPass render_pass,
                    VkFormat color_format,
                    VkSampleCountFlagBits samples,
                    const std::filesystem::path& texture_root,
                    const std::filesystem::path& shader_directory,
                    const NativeScene& scene,
                    std::string& error);
    void render(VkCommandBuffer command_buffer,
                std::uint32_t width,
                std::uint32_t height,
                double elapsed_seconds);
    void destroy();

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
    std::array<float, 3> sun_direction_{0.0f, -1.0f, 0.0f};  // from the cooked scene
    std::array<float, 3> sun_color_{1.0f, 1.0f, 1.0f};
    std::array<float, 3> scene_center_{0.0f, 0.0f, 0.0f};    // geometry bounds -> auto-frame camera
    float scene_radius_ = 1.0f;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptor_sets_;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<VkImage> texture_images_;
    std::vector<VkDeviceMemory> texture_memories_;
    std::vector<VkImageView> texture_views_;
    std::vector<VkSampler> texture_samplers_;
    // Per-material tangent-space normal maps (t1), parallel to the albedo arrays above.
    std::vector<VkImage> normal_images_;
    std::vector<VkDeviceMemory> normal_memories_;
    std::vector<VkImageView> normal_views_;
    std::vector<VkSampler> normal_samplers_;
    std::uint32_t index_count_ = 0;
    struct DrawRange {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
        bool is_water = false;
    };
    std::vector<DrawRange> draw_ranges_;
};

}  // namespace f2
