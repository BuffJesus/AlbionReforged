#include "f2/native_vulkan_cloud_renderer.h"

#include "f2/native_texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

// -----------------------------------------------------------------------------
// Fable II scrolling cloud layers (native Vulkan port) — mirror of native_cloud_renderer.cpp
// (D3D12). Port source: Fable2AssetBrowser SkyboxRenderer.cpp kCloud*Shader + CloudRuntime.
// Drawn after the sky and before the opaque world, alpha-blended, no depth, high->low order.
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

struct CloudVertex {
    float x, y, z;
    float u, v;
};
static_assert(sizeof(CloudVertex) == 20);

struct CloudConstants {
    float view_projection[16];   // column-major (GLSL)
    float viewer_position[4];
    float viewer_direction[4];
    float light_position[4];
    float light_colour[4];
    float layer_params[4];       // x=transparency y=ambient z=brightness w=ShaderNormalStrength
    float uv_scale_offset[4];    // xy = scale, zw = scroll offset
    float cloud_globals[4];      // x = global brightness, z = alpha-test reference
};
static_assert(sizeof(CloudConstants) == 11 * 16);

bool read_spirv(const std::filesystem::path& path, std::vector<std::uint32_t>& words,
                std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "Vulkan cloud shader not found: " + path.string();
        return false;
    }
    const auto size = file.tellg();
    if (size <= 0 || (size_t(size) % sizeof(std::uint32_t)) != 0) {
        error = "Vulkan cloud shader has an invalid SPIR-V size.";
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
        error = "Vulkan could not create a cloud shader module.";
        return VK_NULL_HANDLE;
    }
    return module;
}

bool find_memory_type(VkPhysicalDevice physical_device, uint32_t filter,
                      VkMemoryPropertyFlags properties, uint32_t& out_type) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            out_type = i;
            return true;
        }
    }
    return false;
}

bool create_host_buffer(VkPhysicalDevice physical_device, VkDevice device, VkDeviceSize size,
                        VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    uint32_t type = 0;
    if (!find_memory_type(physical_device, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          type)) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        if (memory) vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// Create a device-local density image from RGBA8 pixels + a WRAP sampler (clouds scroll).
bool create_density_texture(VkPhysicalDevice physical_device, VkDevice device,
                            VkCommandPool command_pool, VkQueue queue, const NativeTexture& src,
                            VkImage& image, VkDeviceMemory& memory, VkImageView& view,
                            VkSampler& sampler) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {src.width, src.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, image, &req);
    uint32_t type = 0;
    if (!find_memory_type(physical_device, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          type) &&
        !find_memory_type(physical_device, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          type)) {
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!create_host_buffer(physical_device, device, src.rgba8.size(),
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, staging_memory)) {
        return false;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device, staging_memory, 0, src.rgba8.size(), 0, &mapped) != VK_SUCCESS) return false;
    std::memcpy(mapped, src.rgba8.data(), src.rgba8.size());
    vkUnmapMemory(device, staging_memory);

    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &command_info, &cmd) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.image = image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_transfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {src.width, src.height, 1};
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier to_shader = to_transfer;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_shader);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const bool ok = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                    vkQueueWaitIdle(queue) == VK_SUCCESS;
    vkFreeCommandBuffers(device, command_pool, 1, &cmd);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_memory, nullptr);
    if (!ok) return false;

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) return false;
    return true;
}

std::array<float, 3> normalise3(std::array<float, 3> v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l <= 1e-6f) return {0.0f, 1.0f, 0.0f};
    return {v[0] / l, v[1] / l, v[2] / l};
}

float shader_normal_strength(float authored) {
    float t = 1.0f - authored;
    if (!(t >= 0.1f)) t = 0.1f;
    if (t > 2.0f) t = 2.0f;
    return t;
}

}  // namespace

bool NativeVulkanCloudRenderer::initialise(VkPhysicalDevice physical_device, VkDevice device,
                                           VkCommandPool command_pool, VkQueue queue,
                                           VkRenderPass render_pass, VkSampleCountFlagBits samples,
                                           const std::filesystem::path& shader_directory,
                                           std::string& error) {
    physical_device_ = physical_device;
    device_ = device;
    command_pool_ = command_pool;
    queue_ = queue;
    const auto fail = [&](const char* message) {
        error = message;
        destroy();
        return false;
    };

    // Descriptor layout: binding 0 = UBO (VS+FS), binding 1 = density sampler (FS).
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 2;
    layout.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        return fail("Vulkan could not create the cloud descriptor layout.");
    }
    VkDescriptorPoolSize pool_sizes[2]{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxLayers},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxLayers},
    };
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kMaxLayers;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        return fail("Vulkan could not create the cloud descriptor pool.");
    }

    // Static index buffer {0,1,2,1,3,2}.
    {
        const std::uint16_t indices[6] = {0, 1, 2, 1, 3, 2};
        if (!create_host_buffer(physical_device_, device_, sizeof(indices),
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer_, index_memory_)) {
            return fail("Vulkan could not create the cloud index buffer.");
        }
        void* mapped = nullptr;
        if (vkMapMemory(device_, index_memory_, 0, sizeof(indices), 0, &mapped) != VK_SUCCESS) {
            return fail("Vulkan could not map the cloud index buffer.");
        }
        std::memcpy(mapped, indices, sizeof(indices));
        vkUnmapMemory(device_, index_memory_);
    }

    const VkShaderModule vertex = make_shader(device_, shader_directory / "native_cloud.vert.spv", error);
    const VkShaderModule fragment = make_shader(device_, shader_directory / "native_cloud.frag.spv", error);
    if (!vertex || !fragment) {
        if (vertex) vkDestroyShaderModule(device_, vertex, nullptr);
        if (fragment) vkDestroyShaderModule(device_, fragment, nullptr);
        destroy();
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &descriptor_set_layout_;
    if (vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vertex, nullptr);
        vkDestroyShaderModule(device_, fragment, nullptr);
        return fail("Vulkan could not create the cloud pipeline layout.");
    }

    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.stride = sizeof(CloudVertex);
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vertex_attributes[2]{};
    vertex_attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    vertex_attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 12};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = vertex_attributes;
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
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
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
    const VkResult pipeline_result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vertex, nullptr);
    vkDestroyShaderModule(device_, fragment, nullptr);
    if (pipeline_result != VK_SUCCESS) {
        return fail("Vulkan could not create the cloud pipeline.");
    }
    return true;
}

void NativeVulkanCloudRenderer::release_layers() {
    for (Layer& layer : layers_) {
        if (layer.mapped_uniform) vkUnmapMemory(device_, layer.uniform_memory);
        if (layer.uniform_buffer) vkDestroyBuffer(device_, layer.uniform_buffer, nullptr);
        if (layer.uniform_memory) vkFreeMemory(device_, layer.uniform_memory, nullptr);
        if (layer.vertex_buffer) vkDestroyBuffer(device_, layer.vertex_buffer, nullptr);
        if (layer.vertex_memory) vkFreeMemory(device_, layer.vertex_memory, nullptr);
        if (layer.sampler) vkDestroySampler(device_, layer.sampler, nullptr);
        if (layer.view) vkDestroyImageView(device_, layer.view, nullptr);
        if (layer.image) vkDestroyImage(device_, layer.image, nullptr);
        if (layer.image_memory) vkFreeMemory(device_, layer.image_memory, nullptr);
        layer = Layer{};
    }
    if (descriptor_pool_) vkResetDescriptorPool(device_, descriptor_pool_, 0);
    layer_count_ = 0;
}

void NativeVulkanCloudRenderer::ensure_scene(const NativeScene& scene) {
    if (bound_scene_ == &scene) return;
    bound_scene_ = &scene;
    release_layers();

    const std::size_t count = std::min<std::size_t>(scene.clouds.size(), kMaxLayers);
    for (std::size_t i = 0; i < count; ++i) {
        const NativeCloudLayer& src = scene.clouds[i];
        if (src.density_map.empty()) continue;
        NativeTexture tex;
        std::string err;
        if (!load_dds_rgba8(src.density_map, tex, err)) continue;

        Layer& layer = layers_[layer_count_];
        if (!create_density_texture(physical_device_, device_, command_pool_, queue_, tex,
                                    layer.image, layer.image_memory, layer.view, layer.sampler)) {
            layer = Layer{};
            continue;
        }
        // Per-layer static quad (render space): centred at world origin, at Y = height.
        const CloudVertex quad[4] = {
            {-src.size_x, src.height, -src.size_y, 0.0f, 0.0f},
            { src.size_x, src.height, -src.size_y, 1.0f, 0.0f},
            {-src.size_x, src.height,  src.size_y, 0.0f, 1.0f},
            { src.size_x, src.height,  src.size_y, 1.0f, 1.0f},
        };
        if (!create_host_buffer(physical_device_, device_, sizeof(quad),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, layer.vertex_buffer,
                                layer.vertex_memory)) {
            layer = Layer{};
            continue;
        }
        void* vmap = nullptr;
        if (vkMapMemory(device_, layer.vertex_memory, 0, sizeof(quad), 0, &vmap) == VK_SUCCESS) {
            std::memcpy(vmap, quad, sizeof(quad));
            vkUnmapMemory(device_, layer.vertex_memory);
        }
        if (!create_host_buffer(physical_device_, device_, sizeof(CloudConstants),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, layer.uniform_buffer,
                                layer.uniform_memory) ||
            vkMapMemory(device_, layer.uniform_memory, 0, sizeof(CloudConstants), 0,
                        &layer.mapped_uniform) != VK_SUCCESS) {
            layer = Layer{};
            continue;
        }
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptor_pool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &descriptor_set_layout_;
        if (vkAllocateDescriptorSets(device_, &allocate, &layer.descriptor_set) != VK_SUCCESS) {
            layer = Layer{};
            continue;
        }
        VkDescriptorBufferInfo buffer_info{layer.uniform_buffer, 0, sizeof(CloudConstants)};
        VkDescriptorImageInfo image_info{layer.sampler, layer.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = layer.descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &buffer_info;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = layer.descriptor_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &image_info;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        layer.scene_index = static_cast<int>(i);
        layer.ready = true;
        ++layer_count_;
    }
}

void NativeVulkanCloudRenderer::render(VkCommandBuffer command_buffer, std::uint32_t width,
                                       std::uint32_t height, const NativeScene& scene,
                                       const std::array<float, 16>& view_projection,
                                       const std::array<float, 3>& eye,
                                       const std::array<float, 3>& forward, double elapsed_seconds) {
    if (!ready() || width == 0 || height == 0 || scene.clouds.empty()) return;
    ensure_scene(scene);
    if (layer_count_ == 0) return;

    // Active layers with transparency>0, sorted high->low (back-to-front).
    std::vector<int> order;
    for (std::uint32_t i = 0; i < layer_count_; ++i) {
        const Layer& layer = layers_[i];
        if (layer.ready && scene.clouds[layer.scene_index].transparency > 0.0f) {
            order.push_back(static_cast<int>(i));
        }
    }
    if (order.empty()) return;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return scene.clouds[layers_[a].scene_index].height >
               scene.clouds[layers_[b].scene_index].height;
    });

    const auto sun_toward = normalise3({-scene.sun_direction[0], -scene.sun_direction[1],
                                        -scene.sun_direction[2]});
    // Match the world renderer's flipped Vulkan viewport so cloud geometry lines up.
    VkViewport viewport{0.0f, static_cast<float>(height), static_cast<float>(width),
                        -static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindIndexBuffer(command_buffer, index_buffer_, 0, VK_INDEX_TYPE_UINT16);

    for (int li : order) {
        Layer& layer = layers_[li];
        const NativeCloudLayer& src = scene.clouds[layer.scene_index];
        CloudConstants c{};
        std::memcpy(c.view_projection, view_projection.data(), sizeof(c.view_projection));
        c.viewer_position[0] = eye[0];
        c.viewer_position[1] = eye[1];
        c.viewer_position[2] = eye[2];
        c.viewer_position[3] = 1.0f;
        c.viewer_direction[0] = forward[0];
        c.viewer_direction[1] = forward[1];
        c.viewer_direction[2] = forward[2];
        c.light_position[0] = eye[0] - sun_toward[0] * 2000.0f;
        c.light_position[1] = eye[1] - sun_toward[1] * 2000.0f;
        c.light_position[2] = eye[2] - sun_toward[2] * 2000.0f;
        c.light_position[3] = 1.0f;
        c.light_colour[0] = scene.sun_color[0];
        c.light_colour[1] = scene.sun_color[1];
        c.light_colour[2] = scene.sun_color[2];
        c.light_colour[3] = 1.0f;
        c.layer_params[0] = src.transparency;
        c.layer_params[1] = src.ambient;
        c.layer_params[2] = src.brightness;
        c.layer_params[3] = shader_normal_strength(src.normal_strength);
        const float scroll_x = src.velocity_x * 0.001f * static_cast<float>(elapsed_seconds);
        const float scroll_y = src.velocity_y * 0.001f * static_cast<float>(elapsed_seconds);
        c.uv_scale_offset[0] = src.texture_scale_x;
        c.uv_scale_offset[1] = src.texture_scale_y;
        c.uv_scale_offset[2] = scroll_x - std::floor(scroll_x);
        c.uv_scale_offset[3] = scroll_y - std::floor(scroll_y);
        c.cloud_globals[0] = scene.cloud_global_brightness;
        c.cloud_globals[2] = scene.cloud_alpha_ref;
        std::memcpy(layer.mapped_uniform, &c, sizeof(c));

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &layer.vertex_buffer, &offset);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0,
                                1, &layer.descriptor_set, 0, nullptr);
        vkCmdDrawIndexed(command_buffer, 6, 1, 0, 0, 0);
    }
}

void NativeVulkanCloudRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    release_layers();
    if (index_buffer_) vkDestroyBuffer(device_, index_buffer_, nullptr);
    if (index_memory_) vkFreeMemory(device_, index_memory_, nullptr);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    index_buffer_ = VK_NULL_HANDLE;
    index_memory_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    bound_scene_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

}  // namespace f2
