#include "f2/native_vulkan_world_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace f2 {
namespace {

struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 4> color{};
};

struct Constants {
    std::array<float, 16> view_projection{};
};

struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

std::array<float, 3> subtract(const std::array<float, 3>& a,
                              const std::array<float, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> cross(const std::array<float, 3>& a,
                           const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

std::array<float, 3> normalise(std::array<float, 3> value) {
    const float length = std::sqrt(dot(value, value));
    if (length <= 0.0001f) return {0.0f, 0.0f, 1.0f};
    for (auto& component : value) component /= length;
    return value;
}

std::array<float, 16> multiply(const std::array<float, 16>& a,
                               const std::array<float, 16>& b) {
    std::array<float, 16> result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t index = 0; index < 4; ++index) {
                result[column * 4 + row] +=
                    a[index * 4 + row] * b[column * 4 + index];
            }
        }
    }
    return result;
}

std::array<float, 4> material_color(const NativeScene& scene, std::uint32_t index) {
    if (index < scene.materials.size()) return scene.materials[index].base_color;
    return {0.25f, 0.65f, 0.95f, 1.0f};
}

Geometry make_geometry(const NativeScene& scene) {
    Geometry geometry;
    for (const auto& instance : scene.instances) {
        if (instance.mesh >= scene.meshes.size()) continue;
        const auto& mesh = scene.meshes[instance.mesh];
        const auto color = material_color(scene, mesh.material);
        const auto base = static_cast<std::uint32_t>(geometry.vertices.size());
        for (const auto& source : mesh.vertices) {
            geometry.vertices.push_back({
                {source.position[0] * instance.scale + instance.position[0],
                 source.position[1] * instance.scale + instance.position[1],
                 source.position[2] * instance.scale + instance.position[2]},
                color});
        }
        for (const auto index : mesh.indices) geometry.indices.push_back(base + index);
    }

    if (!geometry.vertices.empty()) return geometry;

    geometry.vertices = {
        {{-2.0f, 0.0f, -2.0f}, {0.18f, 0.30f, 0.42f, 1.0f}},
        {{2.0f, 0.0f, -2.0f}, {0.18f, 0.30f, 0.42f, 1.0f}},
        {{2.0f, 0.0f, 2.0f}, {0.18f, 0.30f, 0.42f, 1.0f}},
        {{-2.0f, 0.0f, 2.0f}, {0.18f, 0.30f, 0.42f, 1.0f}},
        {{0.0f, 2.0f, 0.0f}, {0.95f, 0.62f, 0.18f, 1.0f}},
    };
    geometry.indices = {0, 1, 2, 0, 2, 3, 0, 4, 1, 1, 4, 2,
                        2, 4, 3, 3, 4, 0};
    return geometry;
}

bool find_memory_type(VkPhysicalDevice physical_device,
                      std::uint32_t type_filter,
                      VkMemoryPropertyFlags properties,
                      std::uint32_t& result) {
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

bool create_buffer(VkPhysicalDevice physical_device,
                   VkDevice device,
                   VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkBuffer& buffer,
                   VkDeviceMemory& memory,
                   std::string& error) {
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
        error = "Vulkan could not create a world buffer.";
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    std::uint32_t memory_type = 0;
    if (!find_memory_type(physical_device, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          memory_type)) {
        error = "Vulkan has no host-visible memory type for the world buffer.";
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        error = "Vulkan could not allocate world buffer memory.";
        if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool upload_buffer(VkPhysicalDevice physical_device,
                   VkDevice device,
                   const void* data,
                   VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkBuffer& buffer,
                   VkDeviceMemory& memory,
                   std::string& error) {
    if (!create_buffer(physical_device, device, size, usage, buffer, memory, error)) return false;
    void* mapped = nullptr;
    if (vkMapMemory(device, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
        error = "Vulkan could not map a world buffer.";
        return false;
    }
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(device, memory);
    return true;
}

bool read_spirv(const std::filesystem::path& path,
                std::vector<std::uint32_t>& code,
                std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Vulkan shader is missing: " + path.string();
        return false;
    }
    const auto size = input.tellg();
    if (size <= 0 || size % 4 != 0) {
        error = "Vulkan shader has an invalid SPIR-V size: " + path.string();
        return false;
    }
    code.resize(static_cast<std::size_t>(size) / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code.data()), size);
    return static_cast<bool>(input);
}

bool create_shader_module(VkDevice device,
                          const std::vector<std::uint32_t>& code,
                          VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * sizeof(std::uint32_t);
    info.pCode = code.data();
    return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS;
}

}  // namespace

bool NativeVulkanWorldRenderer::initialise(VkPhysicalDevice physical_device,
                                           VkDevice device,
                                           VkRenderPass render_pass,
                                           VkFormat color_format,
                                           const std::filesystem::path& shader_directory,
                                           const NativeScene& scene,
                                           std::string& error) {
    device_ = device;
    const auto geometry = make_geometry(scene);
    if (geometry.vertices.empty() || geometry.indices.empty()) {
        error = "The native Vulkan world has no renderable geometry.";
        return false;
    }
    if (!upload_buffer(physical_device, device, geometry.vertices.data(),
                       geometry.vertices.size() * sizeof(Vertex),
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer_, vertex_memory_, error) ||
        !upload_buffer(physical_device, device, geometry.indices.data(),
                       geometry.indices.size() * sizeof(std::uint32_t),
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer_, index_memory_, error) ||
        !create_buffer(physical_device, device, sizeof(Constants),
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, constant_buffer_, constant_memory_, error)) {
        return false;
    }
    if (vkMapMemory(device_, constant_memory_, 0, sizeof(Constants), 0, &mapped_constants_) != VK_SUCCESS) {
        error = "Vulkan could not map the world camera buffer.";
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        error = "Vulkan could not create the world descriptor layout.";
        return false;
    }

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        error = "Vulkan could not create the world descriptor pool.";
        return false;
    }
    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_info.descriptorPool = descriptor_pool_;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &descriptor_set_layout_;
    if (vkAllocateDescriptorSets(device_, &set_info, &descriptor_set_) != VK_SUCCESS) {
        error = "Vulkan could not allocate the world descriptor set.";
        return false;
    }
    VkDescriptorBufferInfo buffer_info{constant_buffer_, 0, sizeof(Constants)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    std::vector<std::uint32_t> vertex_code;
    std::vector<std::uint32_t> fragment_code;
    if (!read_spirv(shader_directory / "native_world.vert.spv", vertex_code, error) ||
        !read_spirv(shader_directory / "native_world.frag.spv", fragment_code, error)) return false;
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    if (!create_shader_module(device_, vertex_code, vertex_module) ||
        !create_shader_module(device_, fragment_code, fragment_module)) {
        error = "Vulkan could not create the native world shader modules.";
        if (vertex_module) vkDestroyShaderModule(device_, vertex_module, nullptr);
        if (fragment_module) vkDestroyShaderModule(device_, fragment_module, nullptr);
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
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPushConstantRange no_push_constants{};
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &descriptor_set_layout_;
    pipeline_layout.pushConstantRangeCount = 0;
    pipeline_layout.pPushConstantRanges = &no_push_constants;
    if (vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        error = "Vulkan could not create the native world pipeline layout.";
        vkDestroyShaderModule(device_, vertex_module, nullptr);
        vkDestroyShaderModule(device_, fragment_module, nullptr);
        return false;
    }
    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_) != VK_SUCCESS) {
        error = "Vulkan could not create the native world pipeline.";
        vkDestroyShaderModule(device_, vertex_module, nullptr);
        vkDestroyShaderModule(device_, fragment_module, nullptr);
        return false;
    }
    vkDestroyShaderModule(device_, vertex_module, nullptr);
    vkDestroyShaderModule(device_, fragment_module, nullptr);
    (void)color_format;
    index_count_ = static_cast<std::uint32_t>(geometry.indices.size());
    return true;
}

void NativeVulkanWorldRenderer::render(VkCommandBuffer command_buffer,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       double elapsed_seconds) {
    if (!pipeline_ || !mapped_constants_ || width == 0 || height == 0) return;

    const float angle = static_cast<float>(elapsed_seconds * 0.25);
    const std::array<float, 3> eye{std::sin(angle) * 7.0f, 4.0f, std::cos(angle) * 7.0f};
    const std::array<float, 3> target{0.0f, 0.7f, 0.0f};
    const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    const auto forward = normalise(subtract(target, eye));
    const auto right = normalise(cross(forward, up));
    const auto camera_up = cross(right, forward);
    std::array<float, 16> view{};
    view[0] = right[0]; view[1] = camera_up[0]; view[2] = -forward[0]; view[3] = 0.0f;
    view[4] = right[1]; view[5] = camera_up[1]; view[6] = -forward[1]; view[7] = 0.0f;
    view[8] = right[2]; view[9] = camera_up[2]; view[10] = -forward[2]; view[11] = 0.0f;
    view[12] = -dot(right, eye); view[13] = -dot(camera_up, eye);
    view[14] = dot(forward, eye); view[15] = 1.0f;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float scale = 1.0f / std::tan(0.5f);
    const float near_plane = 0.1f;
    const float far_plane = 100.0f;
    std::array<float, 16> projection{};
    projection[0] = scale / aspect;
    projection[5] = scale;
    projection[10] = far_plane / (near_plane - far_plane);
    projection[11] = -1.0f;
    projection[14] = (near_plane * far_plane) / (near_plane - far_plane);
    Constants constants{multiply(projection, view)};
    std::memcpy(mapped_constants_, &constants, sizeof(constants));

    VkViewport viewport{0.0f, static_cast<float>(height), static_cast<float>(width),
                        -static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    VkDeviceSize offset = 0;
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &descriptor_set_, 0, nullptr);
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer_, &offset);
    vkCmdBindIndexBuffer(command_buffer, index_buffer_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(command_buffer, index_count_, 1, 0, 0, 0);
}

void NativeVulkanWorldRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    if (mapped_constants_) vkUnmapMemory(device_, constant_memory_);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    if (constant_buffer_) vkDestroyBuffer(device_, constant_buffer_, nullptr);
    if (constant_memory_) vkFreeMemory(device_, constant_memory_, nullptr);
    if (index_buffer_) vkDestroyBuffer(device_, index_buffer_, nullptr);
    if (index_memory_) vkFreeMemory(device_, index_memory_, nullptr);
    if (vertex_buffer_) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
    if (vertex_memory_) vkFreeMemory(device_, vertex_memory_, nullptr);
    device_ = VK_NULL_HANDLE;
    vertex_buffer_ = VK_NULL_HANDLE;
    vertex_memory_ = VK_NULL_HANDLE;
    index_buffer_ = VK_NULL_HANDLE;
    index_memory_ = VK_NULL_HANDLE;
    constant_buffer_ = VK_NULL_HANDLE;
    constant_memory_ = VK_NULL_HANDLE;
    mapped_constants_ = nullptr;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
}

}  // namespace f2
