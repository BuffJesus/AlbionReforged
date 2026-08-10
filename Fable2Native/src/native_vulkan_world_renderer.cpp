#include "f2/native_vulkan_world_renderer.h"
#include "f2/native_texture.h"

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
    std::array<float, 2> uv{};
};

struct Constants {
    std::array<float, 16> view_projection{};
};

struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    struct DrawRange {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
    };
    std::vector<DrawRange> draw_ranges;
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

// Place a model-local vertex: scale, then Euler-rotate (Rz*Ry*Rx), then translate.
// Cooked level instances carry yaw in rotation[1] (prop_instance_xform, LevelLoader.cpp:1328).
std::array<float, 3> place_vertex(const std::array<float, 3>& p,
                                  const std::array<float, 3>& rot, float scale,
                                  const std::array<float, 3>& translate) {
    const float x = p[0] * scale, y = p[1] * scale, z = p[2] * scale;
    const float cx = std::cos(rot[0]), sx = std::sin(rot[0]);
    const float y1 = y * cx - z * sx, z1 = y * sx + z * cx;
    const float cy = std::cos(rot[1]), sy = std::sin(rot[1]);
    const float x2 = x * cy + z1 * sy, z2 = -x * sy + z1 * cy;
    const float cz = std::cos(rot[2]), sz = std::sin(rot[2]);
    const float x3 = x2 * cz - y1 * sz, y3 = x2 * sz + y1 * cz;
    return {x3 + translate[0], y3 + translate[1], z2 + translate[2]};
}

Geometry make_geometry(const NativeScene& scene) {
    Geometry geometry;
    for (const auto& instance : scene.instances) {
        if (instance.mesh >= scene.meshes.size()) continue;
        const auto& mesh = scene.meshes[instance.mesh];
        const auto color = material_color(scene, mesh.material);
        const auto base = static_cast<std::uint32_t>(geometry.vertices.size());
        for (const auto& source : mesh.vertices) {
            const auto world = place_vertex(source.position, instance.rotation,
                                            instance.scale, instance.position);
            geometry.vertices.push_back({world, color, source.uv});
        }
        const auto first_index = static_cast<std::uint32_t>(geometry.indices.size());
        for (const auto index : mesh.indices) geometry.indices.push_back(base + index);
        geometry.draw_ranges.push_back({first_index,
                                        static_cast<std::uint32_t>(mesh.indices.size()),
                                        std::min(mesh.material,
                                                 NativeVulkanWorldRenderer::kMaxMaterialTextures - 1)});
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
    geometry.draw_ranges.push_back({0, static_cast<std::uint32_t>(geometry.indices.size()), 0});
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

std::filesystem::path resolve_texture(const NativeMaterial& material,
                                      const std::filesystem::path& texture_root) {
    if (material.albedo.empty()) return {};
    const std::filesystem::path requested(material.albedo);
    if (requested.is_absolute() && std::filesystem::is_regular_file(requested)) return requested;
    const auto direct = texture_root / requested;
    if (std::filesystem::is_regular_file(direct)) return direct;
    if (requested.has_parent_path() && requested.begin()->string() == "data") {
        const auto without_data = texture_root / requested.lexically_relative("data");
        if (std::filesystem::is_regular_file(without_data)) return without_data;
    }
    return {};
}

bool create_texture(VkPhysicalDevice physical_device,
                    VkDevice device,
                    VkCommandPool command_pool,
                    VkQueue queue,
                    const NativeTexture& source,
                    VkImage& image,
                    VkDeviceMemory& memory,
                    VkImageView& view,
                    VkSampler& sampler,
                    std::string& error) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {source.width, source.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) {
        error = "Vulkan could not create the native texture image.";
        return false;
    }
    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(device, image, &image_requirements);
    std::uint32_t memory_type = 0;
    if (!find_memory_type(physical_device, image_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory_type) &&
        !find_memory_type(physical_device, image_requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          memory_type)) {
        error = "Vulkan has no usable memory type for the native texture.";
        return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = image_requirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        error = "Vulkan could not allocate native texture memory.";
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!create_buffer(physical_device, device, source.rgba8.size(),
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, staging_memory, error)) {
        return false;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device, staging_memory, 0, source.rgba8.size(), 0, &mapped) != VK_SUCCESS) {
        error = "Vulkan could not map the native texture staging buffer.";
        return false;
    }
    std::memcpy(mapped, source.rgba8.data(), source.rgba8.size());
    vkUnmapMemory(device, staging_memory);

    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &command_info, &command_buffer) != VK_SUCCESS) {
        error = "Vulkan could not allocate native texture upload commands.";
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command_buffer, &begin);
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.image = image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_transfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {source.width, source.height, 1};
    vkCmdCopyBufferToImage(command_buffer, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier to_shader = to_transfer;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_shader);
    if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
        error = "Vulkan could not finish native texture upload commands.";
        return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer;
    if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
        vkQueueWaitIdle(queue) != VK_SUCCESS) {
        error = "Vulkan could not submit the native texture upload.";
        return false;
    }
    vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_memory, nullptr);

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
        error = "Vulkan could not create the native texture view.";
        return false;
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // UI atlases are sampled at authored sub-rectangles. Wrapping would pull
    // pixels from the opposite edge of the atlas into the curved borders and
    // create the visible seams seen on the menu rails.
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 1.0f;
    if (vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
        error = "Vulkan could not create the native texture sampler.";
        return false;
    }
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
                                           VkCommandPool command_pool,
                                           VkQueue queue,
                                           VkRenderPass render_pass,
                                           VkFormat color_format,
                                           VkSampleCountFlagBits samples,
                                           const std::filesystem::path& texture_root,
                                           const std::filesystem::path& shader_directory,
                                           const NativeScene& scene,
                                           std::string& error) {
    device_ = device;
    command_pool_ = command_pool;
    queue_ = queue;
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

    const auto material_count = std::max<std::size_t>(
        1, std::min<std::size_t>(scene.materials.size(), kMaxMaterialTextures));
    texture_images_.resize(material_count);
    texture_memories_.resize(material_count);
    texture_views_.resize(material_count);
    texture_samplers_.resize(material_count);
    for (std::size_t material_index = 0; material_index < material_count; ++material_index) {
        NativeTexture native_texture{1, 1, {255, 255, 255, 255}};
        if (material_index < scene.materials.size()) {
            const auto texture_path = resolve_texture(scene.materials[material_index], texture_root);
            if (!texture_path.empty()) {
                std::string texture_error;
                load_dds_rgba8(texture_path, native_texture, texture_error);
            }
        }
        if (!create_texture(physical_device, device_, command_pool_, queue_, native_texture,
                            texture_images_[material_index], texture_memories_[material_index],
                            texture_views_[material_index], texture_samplers_[material_index], error)) {
            return false;
        }
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 2;
    layout_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        error = "Vulkan could not create the world descriptor layout.";
        return false;
    }

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<std::uint32_t>(material_count)},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<std::uint32_t>(material_count)},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = static_cast<std::uint32_t>(material_count);
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        error = "Vulkan could not create the world descriptor pool.";
        return false;
    }
    std::vector<VkDescriptorSetLayout> set_layouts(material_count, descriptor_set_layout_);
    descriptor_sets_.resize(material_count);
    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_info.descriptorPool = descriptor_pool_;
    set_info.descriptorSetCount = static_cast<std::uint32_t>(material_count);
    set_info.pSetLayouts = set_layouts.data();
    if (vkAllocateDescriptorSets(device_, &set_info, descriptor_sets_.data()) != VK_SUCCESS) {
        error = "Vulkan could not allocate the world descriptor sets.";
        return false;
    }
    VkDescriptorBufferInfo buffer_info{constant_buffer_, 0, sizeof(Constants)};
    std::vector<VkDescriptorImageInfo> image_infos(material_count);
    std::vector<VkWriteDescriptorSet> writes(material_count * 2);
    for (std::size_t material_index = 0; material_index < material_count; ++material_index) {
        image_infos[material_index] = {texture_samplers_[material_index],
                                       texture_views_[material_index],
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        auto& uniform_write = writes[material_index * 2];
        uniform_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        uniform_write.dstSet = descriptor_sets_[material_index];
        uniform_write.dstBinding = 0;
        uniform_write.descriptorCount = 1;
        uniform_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniform_write.pBufferInfo = &buffer_info;
        auto& image_write = writes[material_index * 2 + 1];
        image_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        image_write.dstSet = descriptor_sets_[material_index];
        image_write.dstBinding = 1;
        image_write.descriptorCount = 1;
        image_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        image_write.pImageInfo = &image_infos[material_index];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    draw_ranges_.clear();
    for (const auto& range : geometry.draw_ranges) {
        draw_ranges_.push_back({range.first_index, range.index_count, range.material_index});
    }

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
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 28},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
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
    multisample.rasterizationSamples = samples == 0 ? VK_SAMPLE_COUNT_1_BIT : samples;
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
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer_, &offset);
    vkCmdBindIndexBuffer(command_buffer, index_buffer_, 0, VK_INDEX_TYPE_UINT32);
    for (const auto& range : draw_ranges_) {
        const auto material_index = std::min<std::size_t>(range.material_index,
                                                           descriptor_sets_.size() - 1);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1,
                                &descriptor_sets_[material_index], 0, nullptr);
        vkCmdDrawIndexed(command_buffer, range.index_count, 1, range.first_index, 0, 0);
    }
}

void NativeVulkanWorldRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    if (mapped_constants_) vkUnmapMemory(device_, constant_memory_);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    for (const auto sampler : texture_samplers_) {
        if (sampler) vkDestroySampler(device_, sampler, nullptr);
    }
    for (const auto view : texture_views_) {
        if (view) vkDestroyImageView(device_, view, nullptr);
    }
    for (const auto image : texture_images_) {
        if (image) vkDestroyImage(device_, image, nullptr);
    }
    for (const auto memory : texture_memories_) {
        if (memory) vkFreeMemory(device_, memory, nullptr);
    }
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    if (constant_buffer_) vkDestroyBuffer(device_, constant_buffer_, nullptr);
    if (constant_memory_) vkFreeMemory(device_, constant_memory_, nullptr);
    if (index_buffer_) vkDestroyBuffer(device_, index_buffer_, nullptr);
    if (index_memory_) vkFreeMemory(device_, index_memory_, nullptr);
    if (vertex_buffer_) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
    if (vertex_memory_) vkFreeMemory(device_, vertex_memory_, nullptr);
    device_ = VK_NULL_HANDLE;
    command_pool_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    vertex_buffer_ = VK_NULL_HANDLE;
    vertex_memory_ = VK_NULL_HANDLE;
    index_buffer_ = VK_NULL_HANDLE;
    index_memory_ = VK_NULL_HANDLE;
    constant_buffer_ = VK_NULL_HANDLE;
    constant_memory_ = VK_NULL_HANDLE;
    mapped_constants_ = nullptr;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    descriptor_sets_.clear();
    texture_images_.clear();
    texture_memories_.clear();
    texture_views_.clear();
    texture_samplers_.clear();
}

}  // namespace f2
