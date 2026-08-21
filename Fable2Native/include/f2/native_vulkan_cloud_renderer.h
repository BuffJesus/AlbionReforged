#pragma once

#include "native_scene.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace f2 {

// Vulkan mirror of NativeCloudRenderer (D3D12): scrolling theme cloud layers drawn AFTER the sky
// and BEFORE the opaque world, alpha-blended with no depth test/write, layers sorted high->low.
// Same port source (SkyboxRenderer.cpp kCloudPixelShader + CloudRuntime). Owns per-layer density
// images/samplers, UBOs and descriptor sets, a per-layer vertex buffer, and a shared index buffer.
// Rebuilds its per-layer resources when the scene's cloud set changes; a no-op with no cloud layers.
class NativeVulkanCloudRenderer {
public:
    bool initialise(VkPhysicalDevice physical_device,
                    VkDevice device,
                    VkCommandPool command_pool,
                    VkQueue queue,
                    VkRenderPass render_pass,
                    VkSampleCountFlagBits samples,
                    const std::filesystem::path& shader_directory,
                    std::string& error);
    // Draw the scene's cloud layers. `view_projection` is the Vulkan world renderer's exact
    // column-major matrix; `eye`/`forward` its camera (render space).
    void render(VkCommandBuffer command_buffer,
                std::uint32_t width,
                std::uint32_t height,
                const NativeScene& scene,
                const std::array<float, 16>& view_projection,
                const std::array<float, 3>& eye,
                const std::array<float, 3>& forward,
                double elapsed_seconds);
    void destroy();
    [[nodiscard]] bool ready() const noexcept { return pipeline_ != VK_NULL_HANDLE; }

private:
    static constexpr std::uint32_t kMaxLayers = 4;
    void ensure_scene(const NativeScene& scene);
    void release_layers();

    struct Layer {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
        VkBuffer uniform_buffer = VK_NULL_HANDLE;
        VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
        void* mapped_uniform = nullptr;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        int scene_index = -1;  // index into scene.clouds
        bool ready = false;
    };

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkBuffer index_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
    std::array<Layer, kMaxLayers> layers_{};
    std::uint32_t layer_count_ = 0;
    const NativeScene* bound_scene_ = nullptr;
};

}  // namespace f2
