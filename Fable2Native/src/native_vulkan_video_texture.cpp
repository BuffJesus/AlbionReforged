#include "f2/native_vulkan_video_texture.h"

#include <cstring>

namespace f2 {
namespace {

bool find_memory_type(VkPhysicalDevice physical_device, std::uint32_t type_filter,
                      VkMemoryPropertyFlags properties, std::uint32_t& result) {
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
        if ((type_filter & (1u << index)) != 0 &&
            (memory_properties.memoryTypes[index].propertyFlags & properties) == properties) {
            result = index;
            return true;
        }
    }
    return false;
}

bool create_staging_buffer(VkPhysicalDevice physical_device, VkDevice device,
                           VkDeviceSize size, VkBuffer& buffer,
                           VkDeviceMemory& memory, std::string& error) {
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
        error = "Vulkan could not create the video staging buffer.";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    std::uint32_t memory_type = 0;
    if (!find_memory_type(physical_device, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          memory_type)) {
        error = "Vulkan has no host-visible memory type for video upload.";
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        error = "Vulkan could not allocate video staging memory.";
        if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool create_image(VkPhysicalDevice physical_device, VkDevice device,
                  std::uint32_t width, std::uint32_t height,
                  VkImage& image, VkDeviceMemory& memory, std::string& error) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) {
        error = "Vulkan could not create the video image.";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    std::uint32_t memory_type = 0;
    if (!find_memory_type(physical_device, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory_type)) {
        error = "Vulkan has no device-local memory type for video.";
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        error = "Vulkan could not allocate video image memory.";
        if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

}  // namespace

NativeVulkanVideoTexture::~NativeVulkanVideoTexture() {
    destroy();
}

bool NativeVulkanVideoTexture::initialise(VkPhysicalDevice physical_device, VkDevice device,
                                          VkCommandPool command_pool, VkQueue queue,
                                          const NativeVideoFrame& frame, std::string& error) {
    destroy();
    if (frame.width == 0 || frame.height == 0 || frame.rgba8.size() !=
            static_cast<std::size_t>(frame.width) * frame.height * 4) {
        error = "Vulkan received an invalid decoded video frame.";
        return false;
    }
    physical_device_ = physical_device;
    device_ = device;
    command_pool_ = command_pool;
    queue_ = queue;
    width_ = frame.width;
    height_ = frame.height;
    const auto size = static_cast<VkDeviceSize>(frame.rgba8.size());
    if (!create_image(physical_device_, device_, width_, height_, image_, image_memory_, error) ||
        !create_staging_buffer(physical_device_, device_, size, staging_buffer_, staging_memory_, error)) {
        destroy();
        return false;
    }

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &view_) != VK_SUCCESS) {
        error = "Vulkan could not create the video image view.";
        destroy();
        return false;
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 1.0f;
    if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        error = "Vulkan could not create the video sampler.";
        destroy();
        return false;
    }
    if (!upload(frame, error)) {
        destroy();
        return false;
    }
    return true;
}

bool NativeVulkanVideoTexture::update(const NativeVideoFrame& frame, std::string& error) {
    if (!is_ready() || !matches(frame)) {
        error = "Vulkan video texture dimensions changed.";
        return false;
    }
    return upload(frame, error);
}

bool NativeVulkanVideoTexture::upload(const NativeVideoFrame& frame, std::string& error) {
    void* mapped = nullptr;
    if (vkMapMemory(device_, staging_memory_, 0, frame.rgba8.size(), 0, &mapped) != VK_SUCCESS) {
        error = "Vulkan could not map the video staging buffer.";
        return false;
    }
    std::memcpy(mapped, frame.rgba8.data(), frame.rgba8.size());
    vkUnmapMemory(device_, staging_memory_);

    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = command_pool_;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &allocation, &command_buffer) != VK_SUCCESS) {
        error = "Vulkan could not allocate a video upload command buffer.";
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command_buffer, &begin) != VK_SUCCESS) {
        error = "Vulkan could not begin a video upload command buffer.";
        vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
        return false;
    }
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.oldLayout = shader_read_layout_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                : VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcAccessMask = shader_read_layout_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.image = image_;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command_buffer,
                         shader_read_layout_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                              : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_transfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {frame.width, frame.height, 1};
    vkCmdCopyBufferToImage(command_buffer, staging_buffer_, image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier to_shader = to_transfer;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_shader);
    if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
        error = "Vulkan could not finish a video upload command buffer.";
        vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
        return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer;
    const auto submit_result = vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    if (submit_result != VK_SUCCESS || vkQueueWaitIdle(queue_) != VK_SUCCESS) {
        error = "Vulkan could not submit the video upload.";
        vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
        return false;
    }
    vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
    shader_read_layout_ = true;
    return true;
}

void NativeVulkanVideoTexture::destroy() {
    if (!device_) return;
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    if (view_) vkDestroyImageView(device_, view_, nullptr);
    if (staging_buffer_) vkDestroyBuffer(device_, staging_buffer_, nullptr);
    if (staging_memory_) vkFreeMemory(device_, staging_memory_, nullptr);
    if (image_) vkDestroyImage(device_, image_, nullptr);
    if (image_memory_) vkFreeMemory(device_, image_memory_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    staging_buffer_ = VK_NULL_HANDLE;
    staging_memory_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    image_memory_ = VK_NULL_HANDLE;
    width_ = 0;
    height_ = 0;
    shader_read_layout_ = false;
    physical_device_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    command_pool_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
}

}  // namespace f2
