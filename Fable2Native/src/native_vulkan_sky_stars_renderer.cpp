#include "f2/native_vulkan_sky_stars_renderer.h"

#include <array>
#include <cstring>
#include <fstream>
#include <vector>

// -----------------------------------------------------------------------------
// Fable II procedural night stars (native Vulkan port) — mirror of native_sky_stars_renderer.cpp
// (D3D12). Port source: Fable2AssetBrowser SkyDomeXex kStars{Vertex,Pixel}ShaderHlsl.
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

constexpr int kStarCount = 512;
constexpr float kStarPointSize = 1.5f;

struct StarConstants {
    float camera_right[4]{};
    float camera_up[4]{};
    float camera_forward[4]{};
    float star_params[4]{};
};

bool read_spirv(const std::filesystem::path& path, std::vector<std::uint32_t>& words,
                std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "Vulkan stars shader not found: " + path.string();
        return false;
    }
    const auto size = file.tellg();
    if (size <= 0 || (size_t(size) % sizeof(std::uint32_t)) != 0) {
        error = "Vulkan stars shader has an invalid SPIR-V size.";
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
        error = "Vulkan could not create a stars shader module.";
        return VK_NULL_HANDLE;
    }
    return module;
}

uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t filter,
                          VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return UINT32_MAX;
}

}  // namespace

bool NativeVulkanSkyStarsRenderer::initialise(VkPhysicalDevice physical_device, VkDevice device,
                                              VkRenderPass render_pass, VkSampleCountFlagBits samples,
                                              const std::filesystem::path& shader_directory,
                                              std::string& error) {
    device_ = device;
    const auto fail = [&](const char* message) {
        error = message;
        destroy();
        return false;
    };
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        return fail("Vulkan could not create the stars descriptor layout.");
    }
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        return fail("Vulkan could not create the stars descriptor pool.");
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_set_layout_;
    if (vkAllocateDescriptorSets(device_, &allocate, &descriptor_set_) != VK_SUCCESS) {
        return fail("Vulkan could not allocate the stars descriptor set.");
    }

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = sizeof(StarConstants);
    buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (vkCreateBuffer(device_, &buffer_info, nullptr, &constants_buffer_) != VK_SUCCESS) {
        return fail("Vulkan could not create the stars constant buffer.");
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, constants_buffer_, &requirements);
    const uint32_t memory_type = find_memory_type(
        physical_device, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) return fail("Vulkan has no host-visible memory for the stars.");
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = requirements.size;
    memory.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device_, &memory, nullptr, &constants_memory_) != VK_SUCCESS ||
        vkBindBufferMemory(device_, constants_buffer_, constants_memory_, 0) != VK_SUCCESS ||
        vkMapMemory(device_, constants_memory_, 0, sizeof(StarConstants), 0, &mapped_constants_) !=
            VK_SUCCESS) {
        return fail("Vulkan could not map the stars constant buffer.");
    }
    VkDescriptorBufferInfo buffer{constants_buffer_, 0, sizeof(StarConstants)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    const VkShaderModule vs = make_shader(device_, shader_directory / "native_stars.vert.spv", error);
    const VkShaderModule fs = make_shader(device_, shader_directory / "native_stars.frag.spv", error);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        destroy();
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &descriptor_set_layout_;
    if (vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        return fail("Vulkan could not create the stars pipeline layout.");
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;  // additive ONE/ONE
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex_input;
    pipeline.pInputAssemblyState = &input_assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &rasterizer;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &color_blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = pipeline_layout_;
    pipeline.renderPass = render_pass;
    pipeline.subpass = 0;
    const VkResult result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if (result != VK_SUCCESS) return fail("Vulkan could not create the stars pipeline.");
    return true;
}

void NativeVulkanSkyStarsRenderer::render(VkCommandBuffer command_buffer, std::uint32_t width,
                                          std::uint32_t height, const NativeScene& scene,
                                          const SkyCamera& camera, double elapsed_seconds) {
    if (!ready() || !mapped_constants_ || width == 0 || height == 0 ||
        scene.star_brightness <= 0.0f) {
        return;
    }
    StarConstants c{};
    for (int i = 0; i < 3; ++i) {
        c.camera_right[i] = camera.right[i];
        c.camera_up[i] = camera.up[i];
        c.camera_forward[i] = camera.forward[i];
    }
    c.camera_right[3] = camera.tan_half_fov_x;
    c.camera_up[3] = camera.tan_half_fov_y;
    c.star_params[0] = static_cast<float>(elapsed_seconds);
    c.star_params[1] = scene.star_brightness;
    c.star_params[2] = kStarPointSize / static_cast<float>(width);
    c.star_params[3] = kStarPointSize / static_cast<float>(height);
    std::memcpy(mapped_constants_, &c, sizeof(c));

    VkViewport viewport{0.0f, static_cast<float>(height), static_cast<float>(width),
                        -static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &descriptor_set_, 0, nullptr);
    vkCmdDraw(command_buffer, static_cast<std::uint32_t>(kStarCount) * 6u, 1, 0, 0);
}

void NativeVulkanSkyStarsRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    if (mapped_constants_) vkUnmapMemory(device_, constants_memory_);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (constants_buffer_) vkDestroyBuffer(device_, constants_buffer_, nullptr);
    if (constants_memory_) vkFreeMemory(device_, constants_memory_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    device_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    constants_buffer_ = VK_NULL_HANDLE;
    constants_memory_ = VK_NULL_HANDLE;
    mapped_constants_ = nullptr;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    descriptor_set_ = VK_NULL_HANDLE;
}

}  // namespace f2
