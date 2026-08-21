#include "f2/native_vulkan_tonemap_renderer.h"

#include <array>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------------
// Fable II HDR -> LDR compositor with bloom (native Vulkan) — mirror of native_tonemap_renderer.cpp
// (D3D12). Port source: ghidra_out/rendering_pipeline.txt §D.3 — the retail final COMPOSITOR samples
// the resolved HDR scene (RGBA16F) + a thresholded/blurred bloom buffer + an exposure value:
// "result = mul_sat(exposure * sceneColor) + bloom". Default exposure 1.0 + bloom_intensity 0.0 is
// the direct saturate(scene) clamp. All passes are full-screen triangles from gl_VertexIndex.
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

struct Push {
    float params[4]{};
};

bool read_spirv(const std::filesystem::path& path, std::vector<std::uint32_t>& words,
                std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "Vulkan tonemap shader not found: " + path.string();
        return false;
    }
    const auto size = file.tellg();
    if (size <= 0 || (static_cast<std::size_t>(size) % sizeof(std::uint32_t)) != 0) {
        error = "Vulkan tonemap shader has an invalid SPIR-V size.";
        return false;
    }
    words.resize(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), size);
    return static_cast<bool>(file);
}

VkShaderModule make_shader(VkDevice device, const std::filesystem::path& path, std::string& error) {
    std::vector<std::uint32_t> words;
    if (!read_spirv(path, words, error)) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size() * sizeof(std::uint32_t);
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        error = "Vulkan could not create a tonemap shader module.";
        return VK_NULL_HANDLE;
    }
    return module;
}

std::uint32_t find_memory_type(VkPhysicalDevice physical_device, std::uint32_t filter,
                               VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return UINT32_MAX;
}

// A minimal fullscreen pipeline for one fragment shader, baked against render_pass with samples.
VkPipeline make_fullscreen_pipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule vs,
                                    VkShaderModule fs, VkRenderPass render_pass,
                                    VkSampleCountFlagBits samples) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterizer;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = render_pass;
    info.subpass = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}

}  // namespace

bool NativeVulkanTonemapRenderer::initialise(VkPhysicalDevice physical_device, VkDevice device,
                                             VkFormat hdr_format, VkRenderPass output_render_pass,
                                             VkSampleCountFlagBits output_samples,
                                             const std::filesystem::path& shader_directory,
                                             std::string& error) {
    physical_device_ = physical_device;
    device_ = device;
    hdr_format_ = hdr_format;
    const auto fail = [&](const char* message) {
        if (error.empty()) error = message;
        destroy();
        return false;
    };

    // Verify the HDR format supports a colour attachment + a sampled image (universally supported,
    // but check per the design). Also a sane fallback if a device rejects it.
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, hdr_format_, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
        !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        return fail("Vulkan HDR scene format lacks colour-attachment + sampled support.");
    }

    // Linear-clamp sampler for every source (matches the D3D12 static sampler).
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    if (vkCreateSampler(device_, &sampler, nullptr, &sampler_) != VK_SUCCESS)
        return fail("Vulkan could not create the tonemap sampler.");

    // Single combined-image-sampler binding; the composite pipeline uses two sets of this layout.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &set_layout_) != VK_SUCCESS)
        return fail("Vulkan could not create the tonemap descriptor layout.");

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(Push);
    // Two set slots (set0 = scene/source, set1 = bloom); bright/blur bind set0 only (the layout is a
    // superset, which is legal). This mirrors the D3D12 two-table root signature exactly.
    const VkDescriptorSetLayout layouts[2] = {set_layout_, set_layout_};
    VkPipelineLayoutCreateInfo pl_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl_info.setLayoutCount = 2;
    pl_info.pSetLayouts = layouts;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pl_info, nullptr, &pipeline_layout_) != VK_SUCCESS)
        return fail("Vulkan could not create the tonemap pipeline layout.");

    // Descriptor pool for the four source sets (scene + bright + blur_h + blur_v).
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS)
        return fail("Vulkan could not create the tonemap descriptor pool.");
    VkDescriptorSet* sets[4] = {&scene_set_, &bright_set_, &blur_h_set_, &blur_v_set_};
    for (auto* out : sets) {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptor_pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &set_layout_;
        if (vkAllocateDescriptorSets(device_, &alloc, out) != VK_SUCCESS)
            return fail("Vulkan could not allocate a tonemap descriptor set.");
    }

    // Bloom render pass: a single-sample HDR colour target, load DONT_CARE, final SHADER_READ_ONLY.
    VkAttachmentDescription color{};
    color.format = hdr_format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    // Dependencies so the previous pass' shader read finishes before we write, and our write is
    // visible to the subsequent shader read (composite / next blur tap).
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &color;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 2;
    rp_info.pDependencies = deps;
    if (vkCreateRenderPass(device_, &rp_info, nullptr, &bloom_render_pass_) != VK_SUCCESS)
        return fail("Vulkan could not create the tonemap bloom render pass.");

    // Pipelines.
    const VkShaderModule vs = make_shader(device_, shader_directory / "native_tonemap.vert.spv", error);
    const VkShaderModule bright =
        make_shader(device_, shader_directory / "native_tonemap_bright.frag.spv", error);
    const VkShaderModule blur =
        make_shader(device_, shader_directory / "native_tonemap_blur.frag.spv", error);
    const VkShaderModule composite =
        make_shader(device_, shader_directory / "native_tonemap_composite.frag.spv", error);
    const auto drop = [&]() {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (bright) vkDestroyShaderModule(device_, bright, nullptr);
        if (blur) vkDestroyShaderModule(device_, blur, nullptr);
        if (composite) vkDestroyShaderModule(device_, composite, nullptr);
    };
    if (!vs || !bright || !blur || !composite) {
        drop();
        destroy();
        return false;
    }
    bright_pipeline_ = make_fullscreen_pipeline(device_, pipeline_layout_, vs, bright,
                                                bloom_render_pass_, VK_SAMPLE_COUNT_1_BIT);
    blur_pipeline_ = make_fullscreen_pipeline(device_, pipeline_layout_, vs, blur,
                                              bloom_render_pass_, VK_SAMPLE_COUNT_1_BIT);
    composite_pipeline_ = make_fullscreen_pipeline(device_, pipeline_layout_, vs, composite,
                                                   output_render_pass, output_samples);
    drop();
    if (!bright_pipeline_ || !blur_pipeline_ || !composite_pipeline_)
        return fail("Vulkan could not create the tonemap pipelines.");
    return true;
}

bool NativeVulkanTonemapRenderer::create_target(Target& target, std::uint32_t width,
                                                std::uint32_t height, std::string& error) {
    destroy_target(target);
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = hdr_format_;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device_, &image_info, nullptr, &target.image) != VK_SUCCESS) {
        error = "Vulkan could not create a tonemap bloom image.";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, target.image, &requirements);
    const std::uint32_t memory_type = find_memory_type(physical_device_, requirements.memoryTypeBits,
                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        error = "Vulkan has no device-local memory for a tonemap bloom image.";
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device_, &alloc, nullptr, &target.memory) != VK_SUCCESS ||
        vkBindImageMemory(device_, target.image, target.memory, 0) != VK_SUCCESS) {
        error = "Vulkan could not bind a tonemap bloom image.";
        return false;
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = target.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = hdr_format_;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &target.view) != VK_SUCCESS) {
        error = "Vulkan could not create a tonemap bloom image view.";
        return false;
    }
    VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb_info.renderPass = bloom_render_pass_;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &target.view;
    fb_info.width = width;
    fb_info.height = height;
    fb_info.layers = 1;
    if (vkCreateFramebuffer(device_, &fb_info, nullptr, &target.framebuffer) != VK_SUCCESS) {
        error = "Vulkan could not create a tonemap bloom framebuffer.";
        return false;
    }
    return true;
}

void NativeVulkanTonemapRenderer::ensure_targets(VkImageView scene_view, std::uint32_t width,
                                                 std::uint32_t height) {
    targets_ready_ = false;
    if (!ready() || scene_view == VK_NULL_HANDLE || width == 0 || height == 0) return;
    const std::uint32_t bw = width > 1 ? width / 2 : 1;
    const std::uint32_t bh = height > 1 ? height / 2 : 1;

    std::string error;
    if (!create_target(bright_, bw, bh, error) || !create_target(blur_h_, bw, bh, error) ||
        !create_target(blur_v_, bw, bh, error)) {
        destroy_targets();
        return;
    }
    bloom_width_ = bw;
    bloom_height_ = bh;

    // (Re)point the descriptor sets at their sources.
    const std::pair<VkDescriptorSet, VkImageView> writes[4] = {
        {scene_set_, scene_view}, {bright_set_, bright_.view},
        {blur_h_set_, blur_h_.view}, {blur_v_set_, blur_v_.view}};
    for (const auto& [set, view] : writes) {
        VkDescriptorImageInfo image{sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    targets_ready_ = true;
}

void NativeVulkanTonemapRenderer::render_bloom(VkCommandBuffer cmd, float bloom_threshold,
                                               float bloom_intensity) {
    if (!ready() || !targets_ready_ || bloom_intensity <= 0.0f) return;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(bloom_width_),
                              static_cast<float>(bloom_height_), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {bloom_width_, bloom_height_}};

    const auto run_pass = [&](VkFramebuffer framebuffer, VkPipeline pipeline, VkDescriptorSet source,
                              const Push& push) {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = bloom_render_pass_;
        begin.framebuffer = framebuffer;
        begin.renderArea = scissor;
        begin.clearValueCount = 0;
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &source,
                                0, nullptr);
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                           &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    };

    // Bright-pass: HDR scene -> bright_ (threshold).
    Push bright{};
    bright.params[0] = bloom_threshold;
    run_pass(bright_.framebuffer, bright_pipeline_, scene_set_, bright);

    // Blur horizontal: bright_ -> blur_h_.
    Push blur_h{};
    blur_h.params[0] = 1.0f / static_cast<float>(bloom_width_);
    run_pass(blur_h_.framebuffer, blur_pipeline_, bright_set_, blur_h);

    // Blur vertical: blur_h_ -> blur_v_.
    Push blur_v{};
    blur_v.params[1] = 1.0f / static_cast<float>(bloom_height_);
    run_pass(blur_v_.framebuffer, blur_pipeline_, blur_h_set_, blur_v);
}

void NativeVulkanTonemapRenderer::composite(VkCommandBuffer cmd, std::uint32_t width,
                                            std::uint32_t height, float exposure,
                                            float bloom_intensity) {
    if (!ready() || !targets_ready_) return;
    const bool bloom_on = bloom_intensity > 0.0f;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline_);
    const VkDescriptorSet sets[2] = {scene_set_, bloom_on ? blur_v_set_ : scene_set_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 2, sets, 0,
                            nullptr);
    Push push{};
    push.params[0] = exposure;
    push.params[1] = bloom_on ? bloom_intensity : 0.0f;
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void NativeVulkanTonemapRenderer::destroy_target(Target& target) {
    if (device_ == VK_NULL_HANDLE) return;
    if (target.framebuffer) vkDestroyFramebuffer(device_, target.framebuffer, nullptr);
    if (target.view) vkDestroyImageView(device_, target.view, nullptr);
    if (target.image) vkDestroyImage(device_, target.image, nullptr);
    if (target.memory) vkFreeMemory(device_, target.memory, nullptr);
    target = Target{};
}

void NativeVulkanTonemapRenderer::destroy_targets() {
    destroy_target(bright_);
    destroy_target(blur_h_);
    destroy_target(blur_v_);
    bloom_width_ = 0;
    bloom_height_ = 0;
    targets_ready_ = false;
}

void NativeVulkanTonemapRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    destroy_targets();
    if (bright_pipeline_) vkDestroyPipeline(device_, bright_pipeline_, nullptr);
    if (blur_pipeline_) vkDestroyPipeline(device_, blur_pipeline_, nullptr);
    if (composite_pipeline_) vkDestroyPipeline(device_, composite_pipeline_, nullptr);
    if (bloom_render_pass_) vkDestroyRenderPass(device_, bloom_render_pass_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    bright_pipeline_ = VK_NULL_HANDLE;
    blur_pipeline_ = VK_NULL_HANDLE;
    composite_pipeline_ = VK_NULL_HANDLE;
    bloom_render_pass_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    scene_set_ = bright_set_ = blur_h_set_ = blur_v_set_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

}  // namespace f2
