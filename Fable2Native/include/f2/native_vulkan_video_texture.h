#pragma once

#include "f2/native_video_decoder.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace f2 {

class NativeVulkanVideoTexture {
public:
    ~NativeVulkanVideoTexture();

    bool initialise(VkPhysicalDevice physical_device, VkDevice device,
                    VkCommandPool command_pool, VkQueue queue,
                    const NativeVideoFrame& frame, std::string& error);
    bool update(const NativeVideoFrame& frame, std::string& error);
    void destroy();

    [[nodiscard]] bool is_ready() const noexcept { return image_ != VK_NULL_HANDLE; }
    [[nodiscard]] bool matches(const NativeVideoFrame& frame) const noexcept {
        return width_ == frame.width && height_ == frame.height;
    }
    [[nodiscard]] VkImageView view() const noexcept { return view_; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    bool upload(const NativeVideoFrame& frame, std::string& error);

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkBuffer staging_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool shader_read_layout_ = false;
};

}  // namespace f2
