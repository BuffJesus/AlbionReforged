#pragma once

#include "native_scene.h"
#include "sky_camera.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace f2 {

// Vulkan mirror of NativeSkyBillboardRenderer (D3D12): the night moon + its glare halo drawn as
// camera-facing screen-space quads after the clouds, behind the world. Same port source
// (SkyboxRenderer.cpp draw_billboard) and the shared build_billboard() geometry. A no-op unless
// the scene authors a moon (has_moon → night).
class NativeVulkanSkyBillboardRenderer {
public:
    bool initialise(VkPhysicalDevice physical_device,
                    VkDevice device,
                    VkCommandPool command_pool,
                    VkQueue queue,
                    VkRenderPass render_pass,
                    VkSampleCountFlagBits samples,
                    const std::filesystem::path& shader_directory,
                    std::string& error);
    void render(VkCommandBuffer command_buffer,
                std::uint32_t width,
                std::uint32_t height,
                const NativeScene& scene,
                const SkyCamera& camera);
    void destroy();
    [[nodiscard]] bool ready() const noexcept { return alpha_pipeline_ != VK_NULL_HANDLE; }

private:
    void ensure_scene(const NativeScene& scene);
    void release_elements();

    struct Element {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
        void* mapped_vertices = nullptr;
        VkBuffer uniform_buffer = VK_NULL_HANDLE;
        VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
        void* mapped_uniform = nullptr;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        bool ready = false;
    };

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline alpha_pipeline_ = VK_NULL_HANDLE;     // moon disc (SRC_ALPHA/INV)
    VkPipeline additive_pipeline_ = VK_NULL_HANDLE;  // glare (SRC_ALPHA/ONE)
    Element moon_{};   // MoonPhases billboard
    Element glare_{};  // moon glare halo
    const NativeScene* bound_scene_ = nullptr;
};

}  // namespace f2
