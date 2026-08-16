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
    std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
    std::array<float, 4> probe{};  // rgb = baked .lmp SH ambient, w = has-probe flag
};

struct Constants {
    std::array<float, 16> view_projection{};
    std::array<float, 4> sun_direction{0.0f, -1.0f, 0.0f, 0.0f};  // xyz = light dir (world)
    std::array<float, 4> sun_color{1.0f, 1.0f, 1.0f, 0.0f};       // rgb = directional sun colour
    std::array<float, 4> eye_time{0.0f, 0.0f, 0.0f, 0.0f};        // xyz = camera eye, w = seconds
    std::array<float, 4> fog_color{0.0f, 0.0f, 0.0f, 0.0f};       // rgb + w = max density (0 = off)
    std::array<float, 4> fog_range{0.0f, 1.0f, 0.0f, 0.0f};       // x = start dist, y = end dist
    // Theme sky endpoints so the water reflection tracks the actual rendered sky per time-of-day
    // (night = dark) instead of a hardcoded daytime gradient. Mirrors the D3D12 layout.
    std::array<float, 4> sky_zenith{0.6549f, 0.8157f, 1.0f, 1.0f};
    std::array<float, 4> sky_horizon{0.222f, 0.5789f, 1.11f, 1.0f};
    // Sun shadow map (retail "Render ShadowBuffers"): the ortho light view-projection the shadow
    // depth was rendered with, so the frag shader can project each world_pos into the shadow map
    // and compare depth. shadow_params: x=texel size (1/res), y=depth bias, z=enabled (1/0),
    // w=strength (how dark the shadowed sun term goes). Mirrors the D3D12 Constants layout.
    std::array<float, 16> light_view_projection{};
    std::array<float, 4> shadow_params{0.0f, 0.0f, 0.0f, 1.0f};
};

// b1-equivalent point-light UBO (level_lights_effects_re.txt §3.1); mirrors the D3D12 layout.
struct Lights {
    std::uint32_t light_count = 0;
    float pad[3]{0.0f, 0.0f, 0.0f};
    float pos_range[64][4]{};        // xyz = pos, w = range
    float color_intensity[64][4]{};  // rgb = colour, w = intensity
};

struct WaterConstants {
    std::array<std::array<float, 4>, 10> params{};
};

struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    struct DrawRange {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
        bool is_water = false;
        bool is_character = false;
        std::array<float, 3> center{};
        float radius = 0.0f;
        float max_draw_distance = 0.0f;  // 0 = never cull
    };
    std::vector<DrawRange> draw_ranges;
    struct CharacterMesh {
        std::uint32_t base_vertex = 0;
        std::uint32_t vertex_count = 0;
        std::array<float, 3> rotation{};
        float scale = 1.0f;
        std::array<float, 3> position{};
    };
    std::vector<CharacterMesh> character_meshes;
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

template <typename Range>
bool range_distance_culled(const Range& range, const std::array<float, 3>& eye) {
    if (range.max_draw_distance <= 0.0f) return false;
    const float dx = range.center[0] - eye[0];
    const float dy = range.center[1] - eye[1];
    const float dz = range.center[2] - eye[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz) - range.radius;
    return dist > range.max_draw_distance;
}

Geometry make_geometry(const NativeScene& scene) {
    Geometry geometry;
    for (const auto& instance : scene.instances) {
        if (instance.mesh >= scene.meshes.size()) continue;
        const auto& mesh = scene.meshes[instance.mesh];
        const auto color = material_color(scene, mesh.material);
        const auto base = static_cast<std::uint32_t>(geometry.vertices.size());
        std::array<float, 3> lo{}, hi{};
        bool have_bounds = false;
        for (const auto& source : mesh.vertices) {
            const auto world = place_vertex(source.position, instance.rotation,
                                            instance.scale, instance.position);
            if (!have_bounds) { lo = world; hi = world; have_bounds = true; }
            else for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], world[a]); hi[a] = std::max(hi[a], world[a]);
            }
            const auto world_normal = normalise(
                place_vertex(source.normal, instance.rotation, 1.0f, {0.0f, 0.0f, 0.0f}));
            // Per-instance baked order-1 SH ambient (.lmp probe), evaluated against the
            // object-space normal in game axes (un-swap our {x,z,y} render normal) — the exact
            // game shader eval (ghidra_out/prop_ambient_shader_re.txt). Mirrors the D3D12 path.
            std::array<float, 4> probe{0.0f, 0.0f, 0.0f, 0.0f};
            if (instance.has_probe) {
                const auto& s = instance.sh;
                const float gx = source.normal[0], gy = source.normal[2], gz = source.normal[1];
                probe = {std::max(0.0f, s[0] + gx * s[1] + gy * s[2] + gz * s[3]),
                         std::max(0.0f, s[4] + gx * s[5] + gy * s[6] + gz * s[7]),
                         std::max(0.0f, s[8] + gx * s[9] + gy * s[10] + gz * s[11]),
                         1.0f};
            }
            geometry.vertices.push_back({world, color, source.uv, world_normal, probe});
        }
        const auto first_index = static_cast<std::uint32_t>(geometry.indices.size());
        for (const auto index : mesh.indices) geometry.indices.push_back(base + index);
        const bool is_water = mesh.material < scene.materials.size() &&
                              scene.materials[mesh.material].name == "water";
        const bool is_character = mesh.name.rfind("hero", 0) == 0;
        if (is_character) {
            geometry.character_meshes.push_back(
                {base, static_cast<std::uint32_t>(mesh.vertices.size()), instance.rotation,
                 instance.scale, instance.position});
        }
        std::array<float, 3> center{0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        if (have_bounds) {
            for (int a = 0; a < 3; ++a) center[a] = 0.5f * (lo[a] + hi[a]);
            for (int a = 0; a < 3; ++a) radius = std::max(radius, 0.5f * (hi[a] - lo[a]));
        }
        geometry.draw_ranges.push_back({first_index,
                                        static_cast<std::uint32_t>(mesh.indices.size()),
                                        std::min(mesh.material,
                                                 NativeVulkanWorldRenderer::kMaxMaterialTextures - 1),
                                        is_water, is_character, center, radius,
                                        instance.max_draw_distance});
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
                                           VkImageView depth_resolve_view,
                                           const std::filesystem::path& texture_root,
                                           const std::filesystem::path& shader_directory,
                                           const NativeScene& scene,
                                           std::string& error) {
    device_ = device;
    command_pool_ = command_pool;
    queue_ = queue;
    depth_resolve_view_ = depth_resolve_view;
    depth_resolve_enabled_ = depth_resolve_view_ != VK_NULL_HANDLE;
    const auto geometry = make_geometry(scene);
    if (geometry.vertices.empty() || geometry.indices.empty()) {
        error = "The native Vulkan world has no renderable geometry.";
        return false;
    }
    sun_direction_ = normalise(scene.sun_direction);
    sun_color_ = scene.sun_color;
    scene_fog_color_ = scene.fog_color;
    scene_fog_start_ = scene.fog_start;
    scene_fog_end_ = scene.fog_end;
    scene_fog_max_ = scene.fog_max;
    scene_sky_zenith_ = scene.sky_color;
    scene_sky_horizon_ = scene.sky_horizon_color;
    // Bounds -> auto-frame the orbit camera (mirror native_world_renderer.cpp) so the whole
    // town is in view instead of the old fixed radius-7 demo orbit. A cooked `focus` (town
    // bounds excluding horizon backdrop props) wins so the ~1000wu spire vista doesn't blow
    // up the fit. Mirrors the D3D12 renderer.
    if (scene.has_focus) {
        scene_center_ = scene.focus_center;
        scene_radius_ = std::max(scene.focus_radius, 1.0f);
    } else {
        std::array<float, 3> lo{geometry.vertices[0].position};
        std::array<float, 3> hi = lo;
        for (const auto& v : geometry.vertices) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], v.position[a]);
                hi[a] = std::max(hi[a], v.position[a]);
            }
        }
        for (int a = 0; a < 3; ++a) scene_center_[a] = 0.5f * (lo[a] + hi[a]);
        float r = 0.0f;
        for (int a = 0; a < 3; ++a) r = std::max(r, 0.5f * (hi[a] - lo[a]));
        scene_radius_ = std::max(r, 1.0f);
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
    // Static point-light UBO (b1-equivalent), filled once from the cooked scene lights.
    {
        Lights lights{};
        const std::size_t light_n = std::min<std::size_t>(scene.lights.size(), 64);
        lights.light_count = static_cast<std::uint32_t>(light_n);
        for (std::size_t i = 0; i < light_n; ++i) {
            const auto& src = scene.lights[i];
            lights.pos_range[i][0] = src.position[0];
            lights.pos_range[i][1] = src.position[1];
            lights.pos_range[i][2] = src.position[2];
            lights.pos_range[i][3] = src.range;
            lights.color_intensity[i][0] = src.color[0];
            lights.color_intensity[i][1] = src.color[1];
            lights.color_intensity[i][2] = src.color[2];
            lights.color_intensity[i][3] = src.intensity;
        }
        if (!upload_buffer(physical_device, device, &lights, sizeof(lights),
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, lights_buffer_, lights_memory_, error)) {
            return false;
        }
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

    // Per-material tangent-space normal maps (t1). Default = flat (128,128,255) = no perturbation.
    normal_images_.resize(material_count);
    normal_memories_.resize(material_count);
    normal_views_.resize(material_count);
    normal_samplers_.resize(material_count);
    for (std::size_t material_index = 0; material_index < material_count; ++material_index) {
        NativeTexture normal_texture{1, 1, {128, 128, 255, 255}};
        if (material_index < scene.materials.size() &&
            !scene.materials[material_index].normal.empty()) {
            NativeMaterial normal_ref;
            normal_ref.albedo = scene.materials[material_index].normal;  // reuse resolve_texture
            const auto normal_path = resolve_texture(normal_ref, texture_root);
            if (!normal_path.empty()) {
                std::string texture_error;
                load_dds_rgba8(normal_path, normal_texture, texture_error);
            }
        }
        if (!create_texture(physical_device, device_, command_pool_, queue_, normal_texture,
                            normal_images_[material_index], normal_memories_[material_index],
                            normal_views_[material_index], normal_samplers_[material_index], error)) {
            return false;
        }
    }

    // Per-material spec/"material" masks (t2). Default = black (0,0,0) = no highlight.
    spec_images_.resize(material_count);
    spec_memories_.resize(material_count);
    spec_views_.resize(material_count);
    spec_samplers_.resize(material_count);
    for (std::size_t material_index = 0; material_index < material_count; ++material_index) {
        NativeTexture spec_texture{1, 1, {0, 0, 0, 255}};
        if (material_index < scene.materials.size() &&
            !scene.materials[material_index].material.empty()) {
            NativeMaterial spec_ref;
            spec_ref.albedo = scene.materials[material_index].material;  // reuse resolve_texture
            const auto spec_path = resolve_texture(spec_ref, texture_root);
            if (!spec_path.empty()) {
                std::string texture_error;
                load_dds_rgba8(spec_path, spec_texture, texture_error);
            }
        }
        if (!create_texture(physical_device, device_, command_pool_, queue_, spec_texture,
                            spec_images_[material_index], spec_memories_[material_index],
                            spec_views_[material_index], spec_samplers_[material_index], error)) {
            return false;
        }
    }

    water_buffers_.resize(material_count, VK_NULL_HANDLE);
    water_memories_.resize(material_count, VK_NULL_HANDLE);
    for (std::size_t material_index = 0; material_index < material_count; ++material_index) {
        WaterConstants constants{};
        if (material_index < scene.materials.size()) {
            const auto& material = scene.materials[material_index];
            for (std::size_t i = 0; i < material.water_params.size(); ++i)
                constants.params[i / 4][i % 4] = material.water_params[i];
            constants.params[9][1] = material.water_opacity;
        }
        if (!upload_buffer(physical_device, device_, &constants, sizeof(constants),
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, water_buffers_[material_index],
                           water_memories_[material_index], error)) return false;
    }

    // Sun shadow map (retail "Render ShadowBuffers"): a D32 depth image + comparison sampler + a
    // depth-only render pass/framebuffer. The pipeline is built alongside the world pipeline below
    // (it reuses pipeline_layout_). On any failure the shadow term is left disabled (shadow_ready_
    // = false) so the world still renders exactly as before.
    {
        VkImageCreateInfo shadow_image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        shadow_image_info.imageType = VK_IMAGE_TYPE_2D;
        shadow_image_info.format = VK_FORMAT_D32_SFLOAT;
        shadow_image_info.extent = {kShadowSize, kShadowSize, 1};
        shadow_image_info.mipLevels = 1;
        shadow_image_info.arrayLayers = 1;
        shadow_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        shadow_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        shadow_image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT;
        shadow_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool shadow_ok = vkCreateImage(device_, &shadow_image_info, nullptr, &shadow_image_) ==
                         VK_SUCCESS;
        if (shadow_ok) {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device_, shadow_image_, &req);
            std::uint32_t type_index = 0;
            shadow_ok = find_memory_type(physical_device, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type_index);
            if (shadow_ok) {
                VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                alloc.allocationSize = req.size;
                alloc.memoryTypeIndex = type_index;
                shadow_ok = vkAllocateMemory(device_, &alloc, nullptr, &shadow_memory_) ==
                                VK_SUCCESS &&
                            vkBindImageMemory(device_, shadow_image_, shadow_memory_, 0) ==
                                VK_SUCCESS;
            }
        }
        if (shadow_ok) {
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = shadow_image_;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = VK_FORMAT_D32_SFLOAT;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;
            shadow_ok = vkCreateImageView(device_, &view_info, nullptr, &shadow_view_) == VK_SUCCESS;
        }
        if (shadow_ok) {
            // Comparison sampler = hardware PCF. LESS_OR_EQUAL against standard-Z depth, clamp so
            // samples outside the map read the border (opaque white -> lit). Mirrors the D3D12 s1.
            VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler_info.magFilter = VK_FILTER_LINEAR;
            sampler_info.minFilter = VK_FILTER_LINEAR;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            sampler_info.compareEnable = VK_TRUE;
            sampler_info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            sampler_info.maxLod = 1.0f;
            shadow_ok = vkCreateSampler(device_, &sampler_info, nullptr, &shadow_sampler_) ==
                        VK_SUCCESS;
        }
        if (shadow_ok) {
            // Depth-only render pass: clear the depth on load, store it for sampling, and leave the
            // image in SHADER_READ_ONLY so the world pass can sample it without an extra barrier.
            VkAttachmentDescription depth_attachment{};
            depth_attachment.format = VK_FORMAT_D32_SFLOAT;
            depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth_attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkAttachmentReference depth_ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.pDepthStencilAttachment = &depth_ref;
            // Dependencies: (a) previous frame's fragment reads finish before this write; (b) this
            // depth write finishes before the world fragment shader samples it.
            VkSubpassDependency deps[2]{};
            deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            deps[0].dstSubpass = 0;
            deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            deps[1].srcSubpass = 0;
            deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            rp_info.attachmentCount = 1;
            rp_info.pAttachments = &depth_attachment;
            rp_info.subpassCount = 1;
            rp_info.pSubpasses = &subpass;
            rp_info.dependencyCount = 2;
            rp_info.pDependencies = deps;
            shadow_ok = vkCreateRenderPass(device_, &rp_info, nullptr, &shadow_render_pass_) ==
                        VK_SUCCESS;
        }
        if (shadow_ok) {
            VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb_info.renderPass = shadow_render_pass_;
            fb_info.attachmentCount = 1;
            fb_info.pAttachments = &shadow_view_;
            fb_info.width = kShadowSize;
            fb_info.height = kShadowSize;
            fb_info.layers = 1;
            shadow_ok = vkCreateFramebuffer(device_, &fb_info, nullptr, &shadow_framebuffer_) ==
                        VK_SUCCESS;
        }
        shadow_ready_ = shadow_ok;
    }

    VkDescriptorSetLayoutBinding bindings[8]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;  // normal map
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3;  // point-light UBO (b1-equivalent)
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[4].binding = 4;  // spec/"material" mask (t2)
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[5].binding = 5;  // authored WaterFile params + WaterTheme opacity
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[6].binding = 6;  // resolved opaque depth; fallback albedo is used when unavailable
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[7].binding = 7;  // sun shadow map (sampler2DShadow, comparison sampler)
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 8;
    layout_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        error = "Vulkan could not create the world descriptor layout.";
        return false;
    }

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<std::uint32_t>(material_count * 3)},
        // 5 combined image samplers per set: albedo, normal, spec, scene depth, sun shadow map.
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<std::uint32_t>(material_count * 5)},
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
    VkDescriptorBufferInfo lights_info{lights_buffer_, 0, sizeof(Lights)};
    std::vector<VkDescriptorImageInfo> image_infos(material_count);   // albedo (t0)
    std::vector<VkDescriptorImageInfo> normal_infos(material_count);  // normal map (t1)
    std::vector<VkDescriptorImageInfo> spec_infos(material_count);    // spec/"material" (t2)
    std::vector<VkDescriptorImageInfo> depth_infos(material_count);   // resolved opaque depth
    std::vector<VkDescriptorImageInfo> shadow_infos(material_count);  // sun shadow map (binding 7)
    std::vector<VkDescriptorBufferInfo> water_infos(material_count);
    std::vector<VkWriteDescriptorSet> writes(material_count * 8);
    for (std::size_t material_index = 0; material_index < material_count; ++material_index) {
        image_infos[material_index] = {texture_samplers_[material_index],
                                       texture_views_[material_index],
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        normal_infos[material_index] = {normal_samplers_[material_index],
                                        normal_views_[material_index],
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        spec_infos[material_index] = {spec_samplers_[material_index],
                                      spec_views_[material_index],
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        depth_infos[material_index] = {
            depth_resolve_enabled_ ? texture_samplers_[material_index] : texture_samplers_[0],
            depth_resolve_enabled_ ? depth_resolve_view_ : texture_views_[0],
            depth_resolve_enabled_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        water_infos[material_index] = {water_buffers_[material_index], 0, sizeof(WaterConstants)};
        // Sun shadow map (binding 7). When the shadow resources are live, bind the shadow depth
        // view + comparison sampler; otherwise fall back to material 0's texture (unused — the
        // frag's sun_shadow() early-returns when shadow_params.z is disabled).
        shadow_infos[material_index] = {
            shadow_ready_ ? shadow_sampler_ : texture_samplers_[0],
            shadow_ready_ ? shadow_view_ : texture_views_[0],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        auto& uniform_write = writes[material_index * 8];
        uniform_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        uniform_write.dstSet = descriptor_sets_[material_index];
        uniform_write.dstBinding = 0;
        uniform_write.descriptorCount = 1;
        uniform_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniform_write.pBufferInfo = &buffer_info;
        auto& image_write = writes[material_index * 8 + 1];
        image_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        image_write.dstSet = descriptor_sets_[material_index];
        image_write.dstBinding = 1;
        image_write.descriptorCount = 1;
        image_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        image_write.pImageInfo = &image_infos[material_index];
        auto& normal_write = writes[material_index * 8 + 2];
        normal_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        normal_write.dstSet = descriptor_sets_[material_index];
        normal_write.dstBinding = 2;
        normal_write.descriptorCount = 1;
        normal_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        normal_write.pImageInfo = &normal_infos[material_index];
        auto& lights_write = writes[material_index * 8 + 3];
        lights_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        lights_write.dstSet = descriptor_sets_[material_index];
        lights_write.dstBinding = 3;
        lights_write.descriptorCount = 1;
        lights_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        lights_write.pBufferInfo = &lights_info;
        auto& spec_write = writes[material_index * 8 + 4];
        spec_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        spec_write.dstSet = descriptor_sets_[material_index];
        spec_write.dstBinding = 4;
        spec_write.descriptorCount = 1;
        spec_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        spec_write.pImageInfo = &spec_infos[material_index];
        auto& water_write = writes[material_index * 8 + 5];
        water_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        water_write.dstSet = descriptor_sets_[material_index];
        water_write.dstBinding = 5;
        water_write.descriptorCount = 1;
        water_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        water_write.pBufferInfo = &water_infos[material_index];
        auto& depth_write = writes[material_index * 8 + 6];
        depth_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        depth_write.dstSet = descriptor_sets_[material_index];
        depth_write.dstBinding = 6;
        depth_write.descriptorCount = 1;
        depth_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        depth_write.pImageInfo = &depth_infos[material_index];
        auto& shadow_write = writes[material_index * 8 + 7];
        shadow_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        shadow_write.dstSet = descriptor_sets_[material_index];
        shadow_write.dstBinding = 7;
        shadow_write.descriptorCount = 1;
        shadow_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadow_write.pImageInfo = &shadow_infos[material_index];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    draw_ranges_.clear();
    for (const auto& range : geometry.draw_ranges) {
        draw_ranges_.push_back({range.first_index, range.index_count, range.material_index,
                                range.is_water, range.is_character, range.center, range.radius,
                                range.max_draw_distance});
    }
    character_meshes_.clear();
    for (const auto& cm : geometry.character_meshes) {
        character_meshes_.push_back({cm.base_vertex, cm.vertex_count, cm.rotation, cm.scale,
                                     cm.position});
    }
    vertex_count_ = static_cast<std::uint32_t>(geometry.vertices.size());

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
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 36},      // normal
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48},   // baked SH probe
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 5;
    vertex_input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    // Match D3D12's CULL_NONE. The cooked MDL triangle winding is not guaranteed to use
    // one global front-face convention (and the native D3D path intentionally draws both).
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples == 0 ? VK_SAMPLE_COUNT_1_BIT : samples;
    // Depth test + write against the frontend's D32 depth attachment (occlusion).
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reversed-Z
    // Opaque world geometry uses a non-blended PSO. Water gets a separate PSO below because
    // Vulkan blend state is pipeline-static; retail water itself uses ONE/SRC_ALPHA.
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_FALSE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    // Push constants select the water path, depth availability, and optional hero offset.
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = 48;
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &descriptor_set_layout_;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &push_range;
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
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_) != VK_SUCCESS) {
        error = "Vulkan could not create the native world pipeline.";
        vkDestroyShaderModule(device_, vertex_module, nullptr);
        vkDestroyShaderModule(device_, fragment_module, nullptr);
        return false;
    }
    // Water PSO: the shader emits the refraction coefficient as alpha and the framebuffer behind
    // the surface supplies the scene term (water_system_re.txt §5-6).
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    // Match D3D12: the translucent water surface tests against the opaque depth buffer
    // but must not replace it, otherwise later water ranges can occlude one another and
    // the pass no longer behaves like a composited surface.
    depth_stencil.depthWriteEnable = VK_FALSE;
    // Under MSAA the frontend resolves color in the second subpass, where water is drawn over
    // the opaque multisample image. The single-sample render pass remains one subpass.
    pipeline_info.subpass = samples > VK_SAMPLE_COUNT_1_BIT ? 1u : 0u;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                  &water_pipeline_) != VK_SUCCESS) {
        error = "Vulkan could not create the native water pipeline.";
        vkDestroyShaderModule(device_, vertex_module, nullptr);
        vkDestroyShaderModule(device_, fragment_module, nullptr);
        return false;
    }
    // Sun shadow pipeline (retail "Render ShadowBuffers"): a depth-only replay of opaque geometry
    // from the sun ortho POV. Reuses pipeline_layout_ (Camera UBO at binding 0 carries the light
    // VP). Single-sample, no color attachment, depth-write ON, LESS_OR_EQUAL (standard Z), CULL_NONE
    // (mixed MDL winding), and a fixed depth bias to kill acne (mirrors the D3D12 5000 / 2.0 bias).
    if (shadow_ready_) {
        std::vector<std::uint32_t> shadow_code;
        VkShaderModule shadow_module = VK_NULL_HANDLE;
        if (!read_spirv(shader_directory / "native_world_shadow.vert.spv", shadow_code, error) ||
            !create_shader_module(device_, shadow_code, shadow_module)) {
            error = "Vulkan could not create the native world shadow shader module.";
            return false;
        }
        VkPipelineShaderStageCreateInfo shadow_stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        shadow_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        shadow_stage.module = shadow_module;
        shadow_stage.pName = "main";
        VkPipelineMultisampleStateCreateInfo shadow_multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        shadow_multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineRasterizationStateCreateInfo shadow_rasterizer = rasterizer;
        shadow_rasterizer.cullMode = VK_CULL_MODE_NONE;
        shadow_rasterizer.depthBiasEnable = VK_TRUE;
        // Mirror the D3D12 shadow PSO bias (DepthBias 5000 + SlopeScaledDepthBias 2.0). Vulkan's
        // constant factor is scaled by the D32 format's minimum resolvable difference (r ~ 2^-23),
        // so 5000 units gives a comparable small push; the frag adds shadow_params.y = 0.0015 too.
        shadow_rasterizer.depthBiasConstantFactor = 5000.0f;
        shadow_rasterizer.depthBiasSlopeFactor = 2.0f;
        VkPipelineDepthStencilStateCreateInfo shadow_depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        shadow_depth.depthTestEnable = VK_TRUE;
        shadow_depth.depthWriteEnable = VK_TRUE;
        shadow_depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;  // standard Z (near=0, far=1)
        VkPipelineColorBlendStateCreateInfo shadow_blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        shadow_blend.attachmentCount = 0;  // depth only
        VkGraphicsPipelineCreateInfo shadow_pipeline_info{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        shadow_pipeline_info.stageCount = 1;
        shadow_pipeline_info.pStages = &shadow_stage;
        shadow_pipeline_info.pVertexInputState = &vertex_input;
        shadow_pipeline_info.pInputAssemblyState = &input_assembly;
        shadow_pipeline_info.pViewportState = &viewport;
        shadow_pipeline_info.pRasterizationState = &shadow_rasterizer;
        shadow_pipeline_info.pMultisampleState = &shadow_multisample;
        shadow_pipeline_info.pDepthStencilState = &shadow_depth;
        shadow_pipeline_info.pColorBlendState = &shadow_blend;
        shadow_pipeline_info.pDynamicState = &dynamic;
        shadow_pipeline_info.layout = pipeline_layout_;
        shadow_pipeline_info.renderPass = shadow_render_pass_;
        shadow_pipeline_info.subpass = 0;
        const bool shadow_pipe_ok =
            vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &shadow_pipeline_info, nullptr,
                                      &shadow_pipeline_) == VK_SUCCESS;
        vkDestroyShaderModule(device_, shadow_module, nullptr);
        if (!shadow_pipe_ok) {
            error = "Vulkan could not create the native world shadow pipeline.";
            return false;
        }
    }

    vkDestroyShaderModule(device_, vertex_module, nullptr);
    vkDestroyShaderModule(device_, fragment_module, nullptr);
    (void)color_format;
    index_count_ = static_cast<std::uint32_t>(geometry.indices.size());
    return true;
}

SkyCamera NativeVulkanWorldRenderer::compute_camera(std::uint32_t width, std::uint32_t height,
                                                    double elapsed_seconds) const {
    // Same orbit/free-fly basis as render_pass() below, exposed for the sky pass so its rays
    // line up with world geometry. Mirrors NativeWorldRenderer::compute_camera (D3D12).
    const float angle = static_cast<float>(elapsed_seconds * 0.25);
    const float dist = scene_radius_ * 2.4f;
    std::array<float, 3> eye;
    std::array<float, 3> forward;
    if (free_camera_) {
        eye = free_eye_;
        const float cp = std::cos(free_pitch_);
        forward = normalise({cp * std::sin(free_yaw_), std::sin(free_pitch_),
                             cp * std::cos(free_yaw_)});
    } else {
        eye = {scene_center_[0] + std::sin(angle) * dist,
               scene_center_[1] + dist * 0.55f,
               scene_center_[2] + std::cos(angle) * dist};
        const std::array<float, 3> target{scene_center_[0], scene_center_[1], scene_center_[2]};
        forward = normalise(subtract(target, eye));
    }
    const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    const auto right = normalise(cross(up, forward));
    const auto camera_up = cross(forward, right);
    const float aspect = height == 0 ? 1.0f
                                     : static_cast<float>(width) / static_cast<float>(height);
    const float tan_half_y = std::tan(0.5f);
    SkyCamera cam;
    cam.position = eye;
    cam.right = right;
    cam.up = camera_up;
    cam.forward = forward;
    cam.tan_half_fov_x = tan_half_y * aspect;
    cam.tan_half_fov_y = tan_half_y;
    return cam;
}

std::array<float, 16> NativeVulkanWorldRenderer::compute_view_projection(
    std::uint32_t width, std::uint32_t height, double elapsed_seconds) const {
    // Same projection*view render_pass() builds, exposed for the cloud pass. Mirrors it exactly.
    const auto cam = compute_camera(width, height, elapsed_seconds);
    const auto eye = cam.position;
    const auto forward = cam.forward;
    const auto right = cam.right;
    const auto camera_up = cam.up;
    std::array<float, 16> view{};
    view[0] = right[0]; view[1] = camera_up[0]; view[2] = -forward[0]; view[3] = 0.0f;
    view[4] = right[1]; view[5] = camera_up[1]; view[6] = -forward[1]; view[7] = 0.0f;
    view[8] = right[2]; view[9] = camera_up[2]; view[10] = -forward[2]; view[11] = 0.0f;
    view[12] = -dot(right, eye); view[13] = -dot(camera_up, eye);
    view[14] = dot(forward, eye); view[15] = 1.0f;
    const float aspect =
        height == 0 ? 1.0f : static_cast<float>(width) / static_cast<float>(height);
    const float scale = 1.0f / std::tan(0.5f);
    const float near_plane = free_camera_ ? 0.5f : std::max(0.1f, scene_radius_ * 0.05f);
    const float far_plane = scene_radius_ * 8.0f + 10.0f;
    std::array<float, 16> projection{};
    projection[0] = scale / aspect;
    projection[5] = scale;
    projection[10] = near_plane / (far_plane - near_plane);
    projection[11] = -1.0f;
    projection[14] = (near_plane * far_plane) / (far_plane - near_plane);
    return multiply(projection, view);
}

// Sun ortho view-projection for the shadow map: fits a box around the scene, looking along the sun
// travel direction, standard Z in [0,1]. Ported EXACTLY from the D3D12 renderer — the math is
// backend-neutral (row-vector x row-major). The Vulkan shadow pass renders with a negative-height
// viewport (like the world pass), so the framebuffer store orientation matches D3D12 and the frag
// shader's uv Y-flip (0.5,-0.5) is byte-for-byte identical.
std::array<float, 16> NativeVulkanWorldRenderer::compute_light_view_projection(
    const std::array<float, 3>& sun) const {
    const std::array<float, 3> up_ref =
        std::abs(sun[1]) > 0.99f ? std::array<float, 3>{0.0f, 0.0f, 1.0f}
                                 : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    const auto forward = normalise(sun);                 // light travels along +sun
    const auto right = normalise(cross(up_ref, forward));
    const auto up = cross(forward, right);
    const float radius = std::max(scene_radius_, 1.0f);
    const std::array<float, 3> eye = {scene_center_[0] - forward[0] * radius * 2.0f,
                                      scene_center_[1] - forward[1] * radius * 2.0f,
                                      scene_center_[2] - forward[2] * radius * 2.0f};
    const float sx = 1.0f / radius;
    const float sy = 1.0f / radius;
    const float near_plane = radius * 0.05f;
    const float far_plane = radius * 4.0f;
    const float inv_depth = 1.0f / (far_plane - near_plane);
    const float edr = dot(eye, right), edu = dot(eye, up), edf = dot(eye, forward);
    // Row-major, [row*4+col]. This is the SAME storage as the D3D12 compute_light_view_projection.
    std::array<float, 16> m{};
    auto at = [&](int r, int c) -> float& { return m[r * 4 + c]; };
    at(0, 0) = right[0] * sx;   at(1, 0) = right[1] * sx;   at(2, 0) = right[2] * sx;
    at(3, 0) = -edr * sx;
    at(0, 1) = up[0] * sy;      at(1, 1) = up[1] * sy;      at(2, 1) = up[2] * sy;
    at(3, 1) = -edu * sy;
    at(0, 2) = forward[0] * inv_depth; at(1, 2) = forward[1] * inv_depth;
    at(2, 2) = forward[2] * inv_depth; at(3, 2) = (-edf - near_plane) * inv_depth;
    at(3, 3) = 1.0f;  // ortho: w = 1
    // Convention check: D3D12 does clip[c] = sum_r v[r]*array[r*4+c] (row-vector x row-major).
    // GLSL builds a mat4 column-major from these 16 floats (g[col][row]=array[col*4+row]) and does
    // g*v -> result[row] = sum_col array[col*4+row]*v[col], which is the SAME sum. So these exact
    // row-major floats, fed to GLSL's light_view_projection * vec4(pos), reproduce the D3D12 clip.
    return m;
}

void NativeVulkanWorldRenderer::render_pass(VkCommandBuffer command_buffer,
                                            std::uint32_t width,
                                            std::uint32_t height,
                                            double elapsed_seconds,
                                            bool water) {
    if (!pipeline_ || !mapped_constants_ || width == 0 || height == 0) return;

    const float angle = static_cast<float>(elapsed_seconds * 0.25);
    const float dist = scene_radius_ * 2.4f;
    // Free-fly override (level inspection) or the auto-orbit that frames the whole scene.
    std::array<float, 3> eye;
    std::array<float, 3> forward;
    if (free_camera_) {
        eye = free_eye_;
        const float cp = std::cos(free_pitch_);
        forward = normalise({cp * std::sin(free_yaw_), std::sin(free_pitch_),
                             cp * std::cos(free_yaw_)});
    } else {
        eye = {scene_center_[0] + std::sin(angle) * dist,
               scene_center_[1] + dist * 0.55f,
               scene_center_[2] + std::cos(angle) * dist};
        const std::array<float, 3> target{scene_center_[0], scene_center_[1], scene_center_[2]};
        forward = normalise(subtract(target, eye));
    }
    const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    camera_eye_ = eye;  // cache for the shadow pass (runs before this next frame). D3D12 parity.
    // Match D3D12's camera basis exactly. The Vulkan viewport handles the API's vertical
    // origin separately; the world-space right/up vectors must remain identical.
    const auto right = normalise(cross(up, forward));
    const auto camera_up = cross(forward, right);
    std::array<float, 16> view{};
    view[0] = right[0]; view[1] = camera_up[0]; view[2] = -forward[0]; view[3] = 0.0f;
    view[4] = right[1]; view[5] = camera_up[1]; view[6] = -forward[1]; view[7] = 0.0f;
    view[8] = right[2]; view[9] = camera_up[2]; view[10] = -forward[2]; view[11] = 0.0f;
    view[12] = -dot(right, eye); view[13] = -dot(camera_up, eye);
    view[14] = dot(forward, eye); view[15] = 1.0f;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float scale = 1.0f / std::tan(0.5f);
    const float near_plane = free_camera_ ? 0.5f : std::max(0.1f, scene_radius_ * 0.05f);
    const float far_plane = scene_radius_ * 8.0f + 10.0f;
    std::array<float, 16> projection{};
    projection[0] = scale / aspect;
    projection[5] = scale;
    // REVERSED-Z (near->1, far->0; cleared to 0, tested GREATER_OR_EQUAL) — preserves depth
    // precision across the town..horizon-vista span and kills the seam Z-fighting. Mirrors the
    // D3D12 renderer.
    projection[10] = near_plane / (far_plane - near_plane);
    projection[11] = -1.0f;
    projection[14] = (near_plane * far_plane) / (far_plane - near_plane);
    Constants constants{multiply(projection, view)};
    constants.sun_direction = {sun_direction_[0], sun_direction_[1], sun_direction_[2], 0.0f};
    constants.sun_color = {sun_color_[0], sun_color_[1], sun_color_[2], 0.0f};
    constants.eye_time = {eye[0], eye[1], eye[2], static_cast<float>(elapsed_seconds)};
    constants.fog_color = {scene_fog_color_[0], scene_fog_color_[1], scene_fog_color_[2],
                           scene_fog_max_};
    constants.fog_range = {scene_fog_start_, scene_fog_end_, 0.0f, 0.0f};
    constants.sky_zenith = {scene_sky_zenith_[0], scene_sky_zenith_[1], scene_sky_zenith_[2], 1.0f};
    constants.sky_horizon = {scene_sky_horizon_[0], scene_sky_horizon_[1], scene_sky_horizon_[2],
                             1.0f};
    // Sun shadow map: enable only when the shadow resources are live AND the sun is above the
    // horizon (travelling downward -> sun.y < 0). Mirrors the D3D12 render() shadow gate + params.
    const bool shadows_on = shadow_ready_ && sun_direction_[1] < -0.05f;
    constants.light_view_projection = compute_light_view_projection(sun_direction_);
    constants.shadow_params = {
        shadows_on ? 1.0f / static_cast<float>(kShadowSize) : 0.0f,  // texel size (1/res)
        0.0015f,                                                     // NDC-z depth bias
        shadows_on ? 1.0f : 0.0f,                                    // enabled
        // strength = retail ShadowScaleBias (globals.gdb rec 01b5fc17 = 0.8; town theme 0x72d66d23
        // inherits it). shadow = sampled*0.8 + 0.2 (scale/bias form, scale+bias=1 per water PS
        // c139 {0.95,0.05}) → shadowed sun term floors at 0.2. D3D12 parity. (Was a guessed 0.7.)
        0.8f};                                                       // ShadowScaleBias (globals.gdb)
    std::memcpy(mapped_constants_, &constants, sizeof(constants));

    VkViewport viewport{0.0f, static_cast<float>(height), static_cast<float>(width),
                        -static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer_, &offset);
    vkCmdBindIndexBuffer(command_buffer, index_buffer_, 0, VK_INDEX_TYPE_UINT32);
    for (const auto& range : draw_ranges_) {
        if (range.is_water != water) continue;
        if (range_distance_culled(range, camera_eye_)) continue;  // draw-distance LOD gate
        const auto material_index = std::min<std::size_t>(range.material_index,
                                                           descriptor_sets_.size() - 1);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1,
                                &descriptor_sets_[material_index], 0, nullptr);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          water ? water_pipeline_ : pipeline_);
        struct PushConstants {
            std::uint32_t is_water = 0;
            std::uint32_t has_scene_depth = 0;
            std::uint32_t is_character = 0;
            std::uint32_t padding = 0;
            std::array<float, 4> character_offset{};
            std::array<float, 4> character_motion{};
        } push_constants;
        push_constants.is_water = water ? 1u : 0u;
        push_constants.has_scene_depth = depth_resolve_enabled_ ? 1u : 0u;
        push_constants.is_character = range.is_character ? 1u : 0u;
        push_constants.character_offset = {character_offset_[0], character_offset_[1],
                                           character_offset_[2], 0.0f};
        push_constants.character_motion = {character_motion_phase_, character_motion_strength_,
                                           0.0f, 0.0f};
        vkCmdPushConstants(command_buffer, pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push_constants), &push_constants);
        vkCmdDrawIndexed(command_buffer, range.index_count, 1, range.first_index, 0, 0);
    }
}

void NativeVulkanWorldRenderer::set_character_pose(
    std::size_t mesh_index, const std::vector<std::array<float, 3>>& model_positions) {
    if (mesh_index >= character_meshes_.size() || vertex_memory_ == VK_NULL_HANDLE) return;
    const auto& cm = character_meshes_[mesh_index];
    if (model_positions.size() != cm.vertex_count) return;
    if (static_cast<std::uint64_t>(cm.base_vertex) + cm.vertex_count > vertex_count_) return;
    void* mapped = nullptr;
    if (vkMapMemory(device_, vertex_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
        return;
    auto* verts = static_cast<Vertex*>(mapped);
    for (std::uint32_t i = 0; i < cm.vertex_count; ++i) {
        verts[cm.base_vertex + i].position =
            place_vertex(model_positions[i], cm.rotation, cm.scale, cm.position);
    }
    vkUnmapMemory(device_, vertex_memory_);
}

void NativeVulkanWorldRenderer::render(VkCommandBuffer command_buffer,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       double elapsed_seconds) {
    render_pass(command_buffer, width, height, elapsed_seconds, false);
    render_pass(command_buffer, width, height, elapsed_seconds, true);
}

void NativeVulkanWorldRenderer::render_opaque(VkCommandBuffer command_buffer,
                                              std::uint32_t width,
                                              std::uint32_t height,
                                              double elapsed_seconds) {
    render_pass(command_buffer, width, height, elapsed_seconds, false);
}

void NativeVulkanWorldRenderer::render_water(VkCommandBuffer command_buffer,
                                             std::uint32_t width,
                                             std::uint32_t height,
                                             double elapsed_seconds) {
    render_pass(command_buffer, width, height, elapsed_seconds, true);
}

void NativeVulkanWorldRenderer::render_shadow(VkCommandBuffer command_buffer,
                                              std::uint32_t width,
                                              std::uint32_t height,
                                              double elapsed_seconds) {
    (void)width;
    (void)height;
    (void)elapsed_seconds;
    if (!shadow_ready_ || !shadow_pipeline_) return;
    // Gate on the sun above the horizon (travelling downward -> sun.y < -0.05), matching render().
    if (sun_direction_[1] >= -0.05f) return;
    // The light VP lives in the shared Camera UBO, written by the world render_pass() recorded
    // AFTER this in the same single-buffered frame; the GPU reads the final host-coherent value
    // for both passes (same contract as the D3D12 renderer). Depth-only replay of opaque geometry.
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};  // standard-Z far value (LESS_OR_EQUAL, near=0)
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = shadow_render_pass_;
    pass.framebuffer = shadow_framebuffer_;
    pass.renderArea.extent = {kShadowSize, kShadowSize};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    // Negative-height viewport, exactly like the world pass, so the shadow map stores with the same
    // framebuffer orientation as the D3D12 shadow map — the frag uv Y-flip (0.5,-0.5) then matches.
    VkViewport viewport{0.0f, static_cast<float>(kShadowSize), static_cast<float>(kShadowSize),
                        -static_cast<float>(kShadowSize), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {kShadowSize, kShadowSize}};
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer_, &offset);
    vkCmdBindIndexBuffer(command_buffer, index_buffer_, 0, VK_INDEX_TYPE_UINT32);
    // The shadow VS only reads the Camera UBO (binding 0), which is identical across every
    // per-material descriptor set, so bind material 0's set once. Reuses pipeline_layout_.
    if (!descriptor_sets_.empty()) {
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                0, 1, &descriptor_sets_[0], 0, nullptr);
    }
    // A push-constant range is declared on the layout; the shadow VS ignores it, but supply a
    // zeroed block so no stale/undefined push data is read.
    struct PushConstants {
        std::uint32_t is_water = 0;
        std::uint32_t has_scene_depth = 0;
        std::uint32_t is_character = 0;
        std::uint32_t padding = 0;
        std::array<float, 4> character_offset{};
        std::array<float, 4> character_motion{};
    } push_constants;
    vkCmdPushConstants(command_buffer, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push_constants), &push_constants);
    for (const auto& range : draw_ranges_) {
        if (range.is_water) continue;  // water doesn't cast shadows
        if (range_distance_culled(range, camera_eye_)) continue;  // culled = no shadow either
        vkCmdDrawIndexed(command_buffer, range.index_count, 1, range.first_index, 0, 0);
    }
    vkCmdEndRenderPass(command_buffer);
}

void NativeVulkanWorldRenderer::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    if (mapped_constants_) vkUnmapMemory(device_, constant_memory_);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (water_pipeline_) vkDestroyPipeline(device_, water_pipeline_, nullptr);
    if (shadow_pipeline_) vkDestroyPipeline(device_, shadow_pipeline_, nullptr);
    if (shadow_framebuffer_) vkDestroyFramebuffer(device_, shadow_framebuffer_, nullptr);
    if (shadow_render_pass_) vkDestroyRenderPass(device_, shadow_render_pass_, nullptr);
    if (shadow_sampler_) vkDestroySampler(device_, shadow_sampler_, nullptr);
    if (shadow_view_) vkDestroyImageView(device_, shadow_view_, nullptr);
    if (shadow_image_) vkDestroyImage(device_, shadow_image_, nullptr);
    if (shadow_memory_) vkFreeMemory(device_, shadow_memory_, nullptr);
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
    for (const auto sampler : normal_samplers_) {
        if (sampler) vkDestroySampler(device_, sampler, nullptr);
    }
    for (const auto view : normal_views_) {
        if (view) vkDestroyImageView(device_, view, nullptr);
    }
    for (const auto image : normal_images_) {
        if (image) vkDestroyImage(device_, image, nullptr);
    }
    for (const auto memory : normal_memories_) {
        if (memory) vkFreeMemory(device_, memory, nullptr);
    }
    for (const auto sampler : spec_samplers_) {
        if (sampler) vkDestroySampler(device_, sampler, nullptr);
    }
    for (const auto view : spec_views_) {
        if (view) vkDestroyImageView(device_, view, nullptr);
    }
    for (const auto image : spec_images_) {
        if (image) vkDestroyImage(device_, image, nullptr);
    }
    for (const auto memory : spec_memories_) {
        if (memory) vkFreeMemory(device_, memory, nullptr);
    }
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    if (constant_buffer_) vkDestroyBuffer(device_, constant_buffer_, nullptr);
    if (constant_memory_) vkFreeMemory(device_, constant_memory_, nullptr);
    if (lights_buffer_) vkDestroyBuffer(device_, lights_buffer_, nullptr);
    if (lights_memory_) vkFreeMemory(device_, lights_memory_, nullptr);
    for (const auto buffer : water_buffers_) {
        if (buffer) vkDestroyBuffer(device_, buffer, nullptr);
    }
    for (const auto memory : water_memories_) {
        if (memory) vkFreeMemory(device_, memory, nullptr);
    }
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
    lights_buffer_ = VK_NULL_HANDLE;
    lights_memory_ = VK_NULL_HANDLE;
    water_buffers_.clear();
    water_memories_.clear();
    mapped_constants_ = nullptr;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    water_pipeline_ = VK_NULL_HANDLE;
    shadow_pipeline_ = VK_NULL_HANDLE;
    shadow_framebuffer_ = VK_NULL_HANDLE;
    shadow_render_pass_ = VK_NULL_HANDLE;
    shadow_sampler_ = VK_NULL_HANDLE;
    shadow_view_ = VK_NULL_HANDLE;
    shadow_image_ = VK_NULL_HANDLE;
    shadow_memory_ = VK_NULL_HANDLE;
    shadow_ready_ = false;
    descriptor_sets_.clear();
    texture_images_.clear();
    texture_memories_.clear();
    texture_views_.clear();
    texture_samplers_.clear();
    normal_images_.clear();
    normal_memories_.clear();
    normal_views_.clear();
    normal_samplers_.clear();
    spec_images_.clear();
    spec_memories_.clear();
    spec_views_.clear();
    spec_samplers_.clear();
}

}  // namespace f2
