#include "f2/native_vulkan_ui_renderer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace f2 {
namespace {

// Matches native_ui_renderer.cpp: headroom for the UI scene plus the title sparkle field.
constexpr std::size_t kVertexCapacity = 8192;

// Push-constant block shared by both stages (mirrors the D3D12 root 32-bit constants).
struct PushConstants {
    float viewport[2] = {0.0f, 0.0f};
    std::uint32_t combine_detail = 0;
    std::uint32_t key_black_matte = 0;
    std::uint32_t alpha_mask = 0;
};

float channel(std::uint32_t color, unsigned shift) {
    return static_cast<float>((color >> shift) & 0xffu) / 255.0f;
}

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

bool read_spirv(const std::filesystem::path& path, std::vector<std::uint32_t>& code,
                std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Vulkan could not open the UI shader: " + path.string();
        return false;
    }
    const auto size = input.tellg();
    if (size <= 0 || size % 4 != 0) {
        error = "Vulkan UI shader has an invalid SPIR-V size: " + path.string();
        return false;
    }
    code.resize(static_cast<std::size_t>(size) / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code.data()), size);
    return static_cast<bool>(input);
}

bool create_shader_module(VkDevice device, const std::vector<std::uint32_t>& code,
                          VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(std::uint32_t);
    info.pCode = code.data();
    return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS;
}

}  // namespace

NativeVulkanUiRenderer::~NativeVulkanUiRenderer() { destroy(); }

bool NativeVulkanUiRenderer::initialise(VkPhysicalDevice physical_device, VkDevice device,
                                        VkRenderPass render_pass, std::uint32_t frame_count,
                                        VkSampleCountFlagBits samples,
                                        const std::filesystem::path& shader_directory,
                                        std::string& error) {
    destroy();
    device_ = device;
    vertex_capacity_ = kVertexCapacity;
    frame_count = std::max<std::uint32_t>(1, frame_count);

    // Per-frame host-visible vertex buffers so frames in flight never stomp each other.
    frame_buffers_.resize(frame_count);
    const VkDeviceSize buffer_size = sizeof(Vertex) * vertex_capacity_;
    for (auto& frame : frame_buffers_) {
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = buffer_size;
        buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &buffer_info, nullptr, &frame.buffer) != VK_SUCCESS) {
            error = "Vulkan could not create a UI vertex buffer.";
            destroy();
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, frame.buffer, &requirements);
        std::uint32_t memory_type = 0;
        if (!find_memory_type(physical_device, requirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              memory_type)) {
            error = "Vulkan has no host-visible memory type for the UI vertex buffer.";
            destroy();
            return false;
        }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        if (vkAllocateMemory(device_, &allocation, nullptr, &frame.memory) != VK_SUCCESS ||
            vkBindBufferMemory(device_, frame.buffer, frame.memory, 0) != VK_SUCCESS ||
            vkMapMemory(device_, frame.memory, 0, buffer_size, 0,
                        reinterpret_cast<void**>(&frame.mapped)) != VK_SUCCESS) {
            error = "Vulkan could not allocate the UI vertex memory.";
            destroy();
            return false;
        }
    }

    // Descriptor layout: binding 0 = ui sampler, binding 1 = detail sampler (both fragment).
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (std::uint32_t index = 0; index < 2; ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 2;
    layout_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_) !=
        VK_SUCCESS) {
        error = "Vulkan could not create the UI descriptor layout.";
        destroy();
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descriptor_set_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr, &pipeline_layout_) !=
        VK_SUCCESS) {
        error = "Vulkan could not create the UI pipeline layout.";
        destroy();
        return false;
    }

    std::vector<std::uint32_t> vertex_code;
    std::vector<std::uint32_t> fragment_code;
    if (!read_spirv(shader_directory / "native_ui.vert.spv", vertex_code, error) ||
        !read_spirv(shader_directory / "native_ui.frag.spv", fragment_code, error)) {
        destroy();
        return false;
    }
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    if (!create_shader_module(device_, vertex_code, vertex_module) ||
        !create_shader_module(device_, fragment_code, fragment_module)) {
        error = "Vulkan could not create the UI shader modules.";
        if (vertex_module) vkDestroyShaderModule(device_, vertex_module, nullptr);
        if (fragment_module) vkDestroyShaderModule(device_, fragment_module, nullptr);
        destroy();
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertex_binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, detail_u)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions = attributes;

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
    multisample.rasterizationSamples = samples == 0 ? VK_SAMPLE_COUNT_1_BIT : samples;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;

    // Alpha blend, mirroring the D3D12 pipeline: color = src.a*src + (1-src.a)*dst,
    // alpha = 1*src.a + (1-src.a)*dst.a.
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    bool ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                        &pipeline_) == VK_SUCCESS;

    // Additive variant for the title sparkle burst + glow (src-alpha / one).
    if (ok) {
        VkPipelineColorBlendAttachmentState additive_attachment = blend_attachment;
        additive_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        additive_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        VkPipelineColorBlendStateCreateInfo additive_blend = blend;
        additive_blend.pAttachments = &additive_attachment;
        pipeline_info.pColorBlendState = &additive_blend;
        ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &additive_pipeline_) == VK_SUCCESS;
    }

    vkDestroyShaderModule(device_, vertex_module, nullptr);
    vkDestroyShaderModule(device_, fragment_module, nullptr);
    if (!ok) {
        error = "Vulkan could not create the UI pipelines.";
        destroy();
        return false;
    }
    return true;
}

void NativeVulkanUiRenderer::render(VkCommandBuffer command_buffer, std::uint32_t frame_index,
                                    std::uint32_t width, std::uint32_t height,
                                    std::span<const f2::render::UiQuad> quads,
                                    const TextureResolver& resolve) {
    if (!ready() || command_buffer == VK_NULL_HANDLE || width == 0 || height == 0 ||
        quads.empty() || !resolve || frame_buffers_.empty()) {
        return;
    }
    if (quads.size() * 6 > vertex_capacity_) return;
    auto& frame = frame_buffers_[frame_index % frame_buffers_.size()];
    if (!frame.mapped) return;

    std::size_t vertex_index = 0;
    for (const auto& quad : quads) {
        const float r = channel(quad.color, 0);
        const float g = channel(quad.color, 8);
        const float b = channel(quad.color, 16);
        const float a = channel(quad.color, 24);
        const float center_x = (quad.x0 + quad.x1) * 0.5f;
        const float center_y = (quad.y0 + quad.y1) * 0.5f;
        const float cosine = std::cos(quad.rotation_radians);
        const float sine = std::sin(quad.rotation_radians);
        const auto position = [&](float x, float y) {
            const float dx = x - center_x;
            const float dy = y - center_y;
            return std::array<float, 2>{center_x + dx * cosine - dy * sine,
                                        center_y + dx * sine + dy * cosine};
        };
        const auto p00 = position(quad.x0, quad.y0);
        const auto p10 = position(quad.x1, quad.y0);
        const auto p11 = position(quad.x1, quad.y1);
        const auto p01 = position(quad.x0, quad.y1);
        const Vertex vertices[] = {
            {p00[0], p00[1], r, g, b, a, quad.u0, quad.v0, quad.detail_u0, quad.detail_v0},
            {p10[0], p10[1], r, g, b, a, quad.u1, quad.v0, quad.detail_u1, quad.detail_v0},
            {p11[0], p11[1], r, g, b, a, quad.u1, quad.v1, quad.detail_u1, quad.detail_v1},
            {p00[0], p00[1], r, g, b, a, quad.u0, quad.v0, quad.detail_u0, quad.detail_v0},
            {p11[0], p11[1], r, g, b, a, quad.u1, quad.v1, quad.detail_u1, quad.detail_v1},
            {p01[0], p01[1], r, g, b, a, quad.u0, quad.v1, quad.detail_u0, quad.detail_v1},
        };
        std::memcpy(frame.mapped + vertex_index, vertices, sizeof(vertices));
        vertex_index += std::size(vertices);
    }

    const VkViewport vk_viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                                 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(command_buffer, 0, 1, &vk_viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    const VkDeviceSize vertex_offset = 0;
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &frame.buffer, &vertex_offset);

    // Bind the alpha pipeline up front; only flip to additive when a quad needs it (common scene is
    // all-alpha -> a single bind), mirroring the D3D12 renderer.
    auto current_blend = f2::render::BlendMode::Alpha;
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    std::uint32_t first_vertex = 0;
    for (const auto& quad : quads) {
        if (quad.blend_mode != current_blend) {
            current_blend = quad.blend_mode;
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              current_blend == f2::render::BlendMode::Additive ? additive_pipeline_
                                                                               : pipeline_);
        }
        PushConstants push{};
        push.viewport[0] = static_cast<float>(width);
        push.viewport[1] = static_cast<float>(height);
        push.combine_detail = quad.combine_detail ? 1u : 0u;
        push.key_black_matte = quad.key_black_matte ? 1u : 0u;
        push.alpha_mask = quad.alpha_mask ? 1u : 0u;
        vkCmdPushConstants(command_buffer, pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        const auto detail = quad.combine_detail ? quad.detail_texture : quad.texture;
        VkDescriptorSet set = resolve(quad.texture, detail);
        if (set != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline_layout_, 0, 1, &set, 0, nullptr);
            vkCmdDraw(command_buffer, 6, 1, first_vertex, 0);
        }
        first_vertex += 6;
    }
}

void NativeVulkanUiRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        frame_buffers_.clear();
        return;
    }
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (additive_pipeline_) vkDestroyPipeline(device_, additive_pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (descriptor_set_layout_)
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    for (auto& frame : frame_buffers_) {
        if (frame.memory) vkUnmapMemory(device_, frame.memory);
        if (frame.buffer) vkDestroyBuffer(device_, frame.buffer, nullptr);
        if (frame.memory) vkFreeMemory(device_, frame.memory, nullptr);
    }
    frame_buffers_.clear();
    pipeline_ = VK_NULL_HANDLE;
    additive_pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    vertex_capacity_ = 0;
}

}  // namespace f2
