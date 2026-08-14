#include "f2/native_vulkan_sky_billboard_renderer.h"

#include "f2/native_texture.h"
#include "f2/sky_billboard.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

// -----------------------------------------------------------------------------
// Fable II celestial billboards (native Vulkan port) — mirror of native_sky_billboard_renderer.cpp
// (D3D12). Port source: Fable2AssetBrowser SkyboxRenderer.cpp draw_billboard + the moon block.
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

struct BillboardConstants {
    float colour[4];
};

// SkyXexDecomp.h billboard constants.
constexpr float kMoonBillboardDistance = 4000.0f;
constexpr float kMoonSizeScale = 300.0f;
constexpr float kMoonGlareDistance = 1000.0f;
constexpr float kMoonGlareSizeScale = 300.0f;
constexpr float kMoonPhaseUStep = 0.125f;
constexpr float kGate = 0.000099999997f;

bool read_spirv(const std::filesystem::path& path, std::vector<std::uint32_t>& words,
                std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "Vulkan billboard shader not found: " + path.string();
        return false;
    }
    const auto size = file.tellg();
    if (size <= 0 || (size_t(size) % sizeof(std::uint32_t)) != 0) {
        error = "Vulkan billboard shader has an invalid SPIR-V size.";
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
        error = "Vulkan could not create a billboard shader module.";
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

// Device-local image from RGBA8 + a CLAMP sampler (billboards do not tile).
bool create_element_texture(VkPhysicalDevice physical_device, VkDevice device,
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
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) return false;
    return true;
}

void tonemap(float c[3]) {
    for (int i = 0; i < 3; ++i) c[i] = c[i] / (1.0f + c[i]);
}

VkPipeline make_pipeline(VkDevice device, VkPipelineLayout layout, VkRenderPass render_pass,
                         VkSampleCountFlagBits samples, VkShaderModule vs, VkShaderModule fs,
                         bool additive) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, sizeof(BillboardVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]{{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                               {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attrs;
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
    blend.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
    pipeline.layout = layout;
    pipeline.renderPass = render_pass;
    pipeline.subpass = 0;
    VkPipeline result = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &result) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return result;
}

}  // namespace

bool NativeVulkanSkyBillboardRenderer::initialise(VkPhysicalDevice physical_device, VkDevice device,
                                                  VkCommandPool command_pool, VkQueue queue,
                                                  VkRenderPass render_pass,
                                                  VkSampleCountFlagBits samples,
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

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 2;
    layout.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        return fail("Vulkan could not create the billboard descriptor layout.");
    }
    VkDescriptorPoolSize pool_sizes[2]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 2;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        return fail("Vulkan could not create the billboard descriptor pool.");
    }

    const VkShaderModule vs = make_shader(device_, shader_directory / "native_billboard.vert.spv", error);
    const VkShaderModule fs = make_shader(device_, shader_directory / "native_billboard.frag.spv", error);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        destroy();
        return false;
    }
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &descriptor_set_layout_;
    if (vkCreatePipelineLayout(device_, &pipeline_layout, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        return fail("Vulkan could not create the billboard pipeline layout.");
    }
    alpha_pipeline_ = make_pipeline(device_, pipeline_layout_, render_pass, samples, vs, fs, false);
    additive_pipeline_ = make_pipeline(device_, pipeline_layout_, render_pass, samples, vs, fs, true);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if (!alpha_pipeline_ || !additive_pipeline_) {
        return fail("Vulkan could not create the billboard pipelines.");
    }
    return true;
}

void NativeVulkanSkyBillboardRenderer::release_elements() {
    for (Element* e : {&moon_, &glare_}) {
        if (e->mapped_vertices) vkUnmapMemory(device_, e->vertex_memory);
        if (e->mapped_uniform) vkUnmapMemory(device_, e->uniform_memory);
        if (e->vertex_buffer) vkDestroyBuffer(device_, e->vertex_buffer, nullptr);
        if (e->vertex_memory) vkFreeMemory(device_, e->vertex_memory, nullptr);
        if (e->uniform_buffer) vkDestroyBuffer(device_, e->uniform_buffer, nullptr);
        if (e->uniform_memory) vkFreeMemory(device_, e->uniform_memory, nullptr);
        if (e->sampler) vkDestroySampler(device_, e->sampler, nullptr);
        if (e->view) vkDestroyImageView(device_, e->view, nullptr);
        if (e->image) vkDestroyImage(device_, e->image, nullptr);
        if (e->image_memory) vkFreeMemory(device_, e->image_memory, nullptr);
        *e = Element{};
    }
    if (descriptor_pool_) vkResetDescriptorPool(device_, descriptor_pool_, 0);
}

void NativeVulkanSkyBillboardRenderer::ensure_scene(const NativeScene& scene) {
    if (bound_scene_ == &scene) return;
    bound_scene_ = &scene;
    release_elements();
    if (!scene.has_moon) return;

    auto build = [&](const std::string& path, Element& e) {
        if (path.empty()) return;
        NativeTexture tex;
        std::string err;
        if (!load_dds_rgba8(path, tex, err)) return;
        if (!create_element_texture(physical_device_, device_, command_pool_, queue_, tex, e.image,
                                    e.image_memory, e.view, e.sampler)) {
            e = Element{};
            return;
        }
        if (!create_host_buffer(physical_device_, device_, sizeof(BillboardVertex) * 6,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, e.vertex_buffer,
                                e.vertex_memory) ||
            vkMapMemory(device_, e.vertex_memory, 0, sizeof(BillboardVertex) * 6, 0,
                        &e.mapped_vertices) != VK_SUCCESS ||
            !create_host_buffer(physical_device_, device_, sizeof(BillboardConstants),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, e.uniform_buffer,
                                e.uniform_memory) ||
            vkMapMemory(device_, e.uniform_memory, 0, sizeof(BillboardConstants), 0,
                        &e.mapped_uniform) != VK_SUCCESS) {
            e = Element{};
            return;
        }
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptor_pool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &descriptor_set_layout_;
        if (vkAllocateDescriptorSets(device_, &allocate, &e.descriptor_set) != VK_SUCCESS) {
            e = Element{};
            return;
        }
        VkDescriptorBufferInfo buffer_info{e.uniform_buffer, 0, sizeof(BillboardConstants)};
        VkDescriptorImageInfo image_info{e.sampler, e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = e.descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &buffer_info;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = e.descriptor_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &image_info;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        e.ready = true;
    };
    build(scene.moon.texture, moon_);
    build(scene.moon.glare_texture, glare_);
}

void NativeVulkanSkyBillboardRenderer::render(VkCommandBuffer command_buffer, std::uint32_t width,
                                              std::uint32_t height, const NativeScene& scene,
                                              const SkyCamera& camera) {
    if (!ready() || width == 0 || height == 0 || !scene.has_moon) return;
    ensure_scene(scene);
    if (!moon_.ready) return;
    const NativeMoon& moon = scene.moon;
    if (moon.intensity < kGate) return;

    // Match the world renderer's flipped Vulkan viewport so the +Y-up billboard NDC aligns.
    VkViewport viewport{0.0f, static_cast<float>(height), static_cast<float>(width),
                        -static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);

    auto emit = [&](Element& e, VkPipeline pipeline, const std::array<BillboardVertex, 6>& verts,
                    const float colour[4]) {
        std::memcpy(e.mapped_vertices, verts.data(), sizeof(BillboardVertex) * 6);
        BillboardConstants c{};
        std::memcpy(c.colour, colour, sizeof(c.colour));
        std::memcpy(e.mapped_uniform, &c, sizeof(c));
        VkDeviceSize offset = 0;
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindVertexBuffers(command_buffer, 0, 1, &e.vertex_buffer, &offset);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0,
                                1, &e.descriptor_set, 0, nullptr);
        vkCmdDraw(command_buffer, 6, 1, 0, 0);
    };

    const float exposure = moon.exposure;
    std::array<BillboardVertex, 6> verts{};
    const float u0 = static_cast<float>(std::clamp(moon.phase, 0, 7)) * kMoonPhaseUStep;
    const float u1 = u0 + kMoonPhaseUStep;
    if (build_billboard(camera, moon.direction, kMoonBillboardDistance, moon.size * kMoonSizeScale,
                        moon.size * kMoonSizeScale, u0, u1, verts)) {
        float colour[4] = {0.5f * moon.intensity * exposure, 0.5f * moon.intensity * exposure,
                           moon.transparency * exposure, moon.transparency};
        tonemap(colour);
        emit(moon_, alpha_pipeline_, verts, colour);
    }
    if (glare_.ready && moon.glare_intensity >= kGate &&
        build_billboard(camera, moon.direction, kMoonGlareDistance, moon.glare_size * kMoonGlareSizeScale,
                        moon.glare_size * kMoonGlareSizeScale, 0.0f, 1.0f, verts)) {
        const float scale = moon.glare_intensity * exposure;
        float colour[4] = {scale, scale, scale, 1.0f};
        tonemap(colour);
        emit(glare_, additive_pipeline_, verts, colour);
    }
}

void NativeVulkanSkyBillboardRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    release_elements();
    if (alpha_pipeline_) vkDestroyPipeline(device_, alpha_pipeline_, nullptr);
    if (additive_pipeline_) vkDestroyPipeline(device_, additive_pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    alpha_pipeline_ = VK_NULL_HANDLE;
    additive_pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    bound_scene_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

}  // namespace f2
