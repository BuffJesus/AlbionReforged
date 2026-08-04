#pragma once

#include "native_scene.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace f2 {

class NativeVulkanWorldRenderer {
public:
    bool initialise(VkPhysicalDevice physical_device,
                    VkDevice device,
                    VkCommandPool command_pool,
                    VkQueue queue,
                    VkRenderPass render_pass,
                    VkFormat color_format,
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
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkImage texture_image_ = VK_NULL_HANDLE;
    VkDeviceMemory texture_memory_ = VK_NULL_HANDLE;
    VkImageView texture_view_ = VK_NULL_HANDLE;
    VkSampler texture_sampler_ = VK_NULL_HANDLE;
    std::uint32_t index_count_ = 0;
};

}  // namespace f2
