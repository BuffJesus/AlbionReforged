#pragma once

#include "f2/render/render_backend.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace f2 {

// Vulkan mirror of NativeUiRenderer: draws a backend-neutral f2::render::UiQuad scene with the same
// shader/blend semantics (combine_detail / key_black_matte / alpha_mask; alpha + additive blend;
// CPU quad rotation; screen-pixel -> NDC). This is the seam that lets the Vulkan frontend render the
// SAME shared scene the D3D12 backend does, so the front end no longer needs ImGui
// (docs/FRONTEND_ARCHITECTURE.md).
//
// The app owns texture lifetime and per-texture descriptor sets, allocated against
// descriptor_set_layout() (binding 0 = ui sampler, binding 1 = detail sampler). It supplies a
// resolver mapping a (primary, detail) TextureId pair to such a set; non-detail quads pass the same
// id for both bindings.
class NativeVulkanUiRenderer {
public:
    ~NativeVulkanUiRenderer();

    bool initialise(VkPhysicalDevice physical_device, VkDevice device, VkRenderPass render_pass,
                    std::uint32_t frame_count, const std::filesystem::path& shader_directory,
                    std::string& error);
    void destroy();

    [[nodiscard]] bool ready() const noexcept { return pipeline_ != VK_NULL_HANDLE; }

    // Layout the app allocates its per-texture descriptor sets against (binding 0 ui + binding 1 detail).
    [[nodiscard]] VkDescriptorSetLayout descriptor_set_layout() const noexcept {
        return descriptor_set_layout_;
    }

    using TextureResolver =
        std::function<VkDescriptorSet(f2::render::TextureId primary, f2::render::TextureId detail)>;

    // Record draw commands for `quads` into `command_buffer` (must be inside an active render pass
    // matching the render pass passed to initialise). `frame_index` (< frame_count) selects a
    // per-frame vertex buffer so frames in flight do not overwrite each other.
    void render(VkCommandBuffer command_buffer, std::uint32_t frame_index, std::uint32_t width,
                std::uint32_t height, std::span<const f2::render::UiQuad> quads,
                const TextureResolver& resolve);

private:
    struct Vertex {
        float x = 0.0f;
        float y = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
        float u = 0.0f;
        float v = 0.0f;
        float detail_u = 0.0f;
        float detail_v = 0.0f;
    };

    struct FrameBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        Vertex* mapped = nullptr;
    };

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;           // alpha blend
    VkPipeline additive_pipeline_ = VK_NULL_HANDLE;  // src-alpha / one
    std::vector<FrameBuffer> frame_buffers_;
    std::size_t vertex_capacity_ = 0;
};

}  // namespace f2
