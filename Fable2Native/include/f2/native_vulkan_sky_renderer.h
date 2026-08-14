#pragma once

#include "native_scene.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace f2 {

class NativeVulkanSkyRenderer {
public:
    bool initialise(VkPhysicalDevice physical_device,
                    VkDevice device,
                    VkRenderPass render_pass,
                    VkFormat color_format,
                    VkSampleCountFlagBits samples,
                    const std::filesystem::path& shader_directory,
                    std::string& error);
    void render(VkCommandBuffer command_buffer,
                std::uint32_t width,
                std::uint32_t height,
                const NativeScene& scene);
    void destroy();
    [[nodiscard]] bool ready() const noexcept { return pipeline_ != VK_NULL_HANDLE; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkBuffer constants_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory constants_memory_ = VK_NULL_HANDLE;
    void* mapped_constants_ = nullptr;
};

}  // namespace f2
