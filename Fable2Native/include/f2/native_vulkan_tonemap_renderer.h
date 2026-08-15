#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace f2 {

// Vulkan mirror of NativeTonemapRenderer (D3D12): the full HDR -> LDR COMPOSITOR (the retail final
// pass, ghidra_out/rendering_pipeline.txt §D.3: "result = mul_sat(exposure * sceneColor) + bloom").
// Given the RGBA16F scene target that every World pass renders into (see native_scene_color.h /
// VK_FORMAT_R16G16B16A16_SFLOAT), it produces the LDR swap-chain image via:
//   1. bright-pass  : threshold the HDR scene into a half-res bloom target
//   2. blur (H, V)  : separable 9-tap Gaussian on the bright target (ping-pong)
//   3. composite    : saturate(exposure * scene) + bloom_intensity * bloom  -> swap-chain image
//
// The bright/blur passes each render into their own half-res HDR image through this renderer's own
// tiny single-subpass render passes; the composite draw is recorded by the app INSIDE the swap-chain
// render pass (the pipeline is baked against the swap-chain render pass passed to initialise).
// Default exposure 1.0 + bloom_intensity 0.0 reproduces the direct saturate(scene) clamp exactly.
class NativeVulkanTonemapRenderer {
public:
    // hdr_format = the World scene-colour format (VK_FORMAT_R16G16B16A16_SFLOAT). output_render_pass /
    // output_samples = the swap-chain render pass + its sample count (for the composite pipeline).
    bool initialise(VkPhysicalDevice physical_device, VkDevice device, VkFormat hdr_format,
                    VkRenderPass output_render_pass, VkSampleCountFlagBits output_samples,
                    const std::filesystem::path& shader_directory, std::string& error);

    [[nodiscard]] bool ready() const noexcept { return composite_pipeline_ != VK_NULL_HANDLE; }

    // (Re)create the half-res bloom targets + descriptor sets, viewing the app's HDR scene resolve
    // image view. Call after the HDR scene target is (re)created (safe every resize).
    void ensure_targets(VkImageView scene_view, std::uint32_t width, std::uint32_t height);

    // Bright-pass + separable blur into this renderer's own half-res HDR passes. MUST be called
    // OUTSIDE any render pass (it begins/ends its own). No-op when bloom_intensity <= 0.
    void render_bloom(VkCommandBuffer command_buffer, float bloom_threshold, float bloom_intensity);

    // Composite draw: saturate(exposure * scene) + intensity * bloom. MUST be recorded INSIDE the
    // swap-chain render pass. Uses the blurred bloom target from the last render_bloom() when
    // bloom_intensity > 0, else samples the scene for both taps (intensity forced to 0).
    void composite(VkCommandBuffer command_buffer, std::uint32_t width, std::uint32_t height,
                   float exposure, float bloom_intensity);

    void destroy();

private:
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };

    bool create_target(Target& target, std::uint32_t width, std::uint32_t height, std::string& error);
    void destroy_target(Target& target);
    void destroy_targets();

    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat hdr_format_ = VK_FORMAT_R16G16B16A16_SFLOAT;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;  // binding0 = source (combined image sampler)
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;  // + 16B push constant (vec4 params)
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

    VkRenderPass bloom_render_pass_ = VK_NULL_HANDLE;  // half-res HDR target, load DONT_CARE
    VkPipeline bright_pipeline_ = VK_NULL_HANDLE;      // HDR scene -> bright (threshold)
    VkPipeline blur_pipeline_ = VK_NULL_HANDLE;        // separable Gaussian
    VkPipeline composite_pipeline_ = VK_NULL_HANDLE;   // scene + bloom -> swap-chain (output pass)

    // Descriptor sets over each sampled source; re-written in ensure_targets().
    VkDescriptorSet scene_set_ = VK_NULL_HANDLE;    // over the app's HDR scene resolve view
    VkDescriptorSet bright_set_ = VK_NULL_HANDLE;   // over bright_
    VkDescriptorSet blur_h_set_ = VK_NULL_HANDLE;   // over blur_h_
    VkDescriptorSet blur_v_set_ = VK_NULL_HANDLE;   // over blur_v_

    Target bright_;
    Target blur_h_;
    Target blur_v_;
    std::uint32_t bloom_width_ = 0;
    std::uint32_t bloom_height_ = 0;
    bool targets_ready_ = false;
};

}  // namespace f2
