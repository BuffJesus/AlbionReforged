#include "f2/native_world_renderer.h"
#include "f2/native_texture.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace f2 {
namespace {

struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
    std::array<float, 4> color{};
    std::array<float, 2> uv{};
    // Baked per-instance ambient (.lmp SH probe DC term). rgb = ambient radiance,
    // w = 1 when a probe is present (PS uses it instead of the hemisphere floor), else 0.
    std::array<float, 4> probe{};
};

struct Constants {
    float view_projection[4][4]{};
    float sun_direction[4]{0.0f, -1.0f, 0.0f, 0.0f};  // xyz = normalised light dir (world), w unused
    float eye_time[4]{0.0f, 0.0f, 0.0f, 0.0f};         // xyz = camera eye (world), w = elapsed seconds
    float sun_color[4]{1.0f, 1.0f, 1.0f, 0.0f};        // rgb = directional sun colour (theme)
    float fog_color[4]{0.0f, 0.0f, 0.0f, 0.0f};        // rgb + w = max density (0 = off)
    float fog_range[4]{0.0f, 1.0f, 0.0f, 0.0f};        // x = start dist, y = end dist
    float viewport_size[4]{0.0f, 0.0f, 0.0f, 0.0f};    // xy = render width/height
};

// b1 point-light cbuffer (level_lights_effects_re.txt §3.1). Mirrors the HLSL layout:
// light_count (uint + float3 pad), then two float4 arrays. pos_range[i] = (x,y,z,range),
// color_intensity[i] = (r,g,b,intensity). Populated once from the cooked scene lights.
struct Lights {
    std::uint32_t light_count = 0;
    float pad[3]{0.0f, 0.0f, 0.0f};
    float pos_range[64][4]{};
    float color_intensity[64][4]{};
};

// b2 water material cbuffer. Ten float4s carry WaterFile::params[37], followed by the
// WaterTheme opacity in params[9].y. Each material gets a 256-byte-aligned slice.
struct WaterConstants {
    float params[10][4]{};
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
    };
    std::vector<DrawRange> draw_ranges;
};

D3D12_HEAP_PROPERTIES upload_heap() {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC buffer_description(std::size_t size) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

D3D12_RASTERIZER_DESC rasterizer_description() {
    D3D12_RASTERIZER_DESC description{};
    description.FillMode = D3D12_FILL_MODE_SOLID;
    // Draw both faces: the MDL triangle-strip winding (0xFFFF restart, per-step flip)
    // doesn't map cleanly to a single front-face convention yet.
    description.CullMode = D3D12_CULL_MODE_NONE;
    description.FrontCounterClockwise = FALSE;
    description.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.DepthClipEnable = TRUE;  // real D32 depth buffer; clip on the z row
    description.MultisampleEnable = FALSE;
    description.AntialiasedLineEnable = FALSE;
    description.ForcedSampleCount = 0;
    description.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return description;
}

D3D12_BLEND_DESC blend_description() {
    D3D12_BLEND_DESC description{};
    description.AlphaToCoverageEnable = FALSE;
    description.IndependentBlendEnable = FALSE;
    auto& target = description.RenderTarget[0];
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return description;
}

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

std::array<float, 4> material_color(const NativeScene& scene, std::uint32_t index) {
    if (index < scene.materials.size()) return scene.materials[index].base_color;
    return {0.25f, 0.65f, 0.95f, 1.0f};
}

// Place a model-local vertex into the world: scale, then Euler-rotate (Rz*Ry*Rx),
// then translate. Cooked level instances (cook_levels.py) carry yaw in rotation[1]
// about the world up axis — prop_instance_xform @ LevelLoader.cpp:1328.
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
            // Rotate the normal into world space (uniform scale + no translation).
            const auto world_normal = normalise(
                place_vertex(source.normal, instance.rotation, 1.0f, {0.0f, 0.0f, 0.0f}));
            // Per-instance baked order-1 SH ambient (.lmp probe, exact game shader eval,
            // ghidra_out/prop_ambient_shader_re.txt): amb.c = C0 + N.(C1,C2,C3) per channel,
            // against the OBJECT-space normal in GAME axes. Our mesh normals are stored render-
            // axes ({x,z,y} swap), so un-swap: game (x,y,z) = source.normal (x,z,y). w=1 flags
            // "use the probe"; w=0 -> PS keeps its hemisphere fallback (terrain/foliage/no-probe).
            std::array<float, 4> probe{0.0f, 0.0f, 0.0f, 0.0f};
            if (instance.has_probe) {
                const auto& s = instance.sh;
                const float gx = source.normal[0], gy = source.normal[2], gz = source.normal[1];
                probe = {std::max(0.0f, s[0] + gx * s[1] + gy * s[2] + gz * s[3]),
                         std::max(0.0f, s[4] + gx * s[5] + gy * s[6] + gz * s[7]),
                         std::max(0.0f, s[8] + gx * s[9] + gy * s[10] + gz * s[11]),
                         1.0f};
            }
            geometry.vertices.push_back({world, world_normal, color, source.uv, probe});
        }
        const auto first_index = static_cast<std::uint32_t>(geometry.indices.size());
        for (const auto index : mesh.indices) geometry.indices.push_back(base + index);
        const bool is_water = mesh.material < scene.materials.size() &&
                              scene.materials[mesh.material].name == "water";
        const bool is_character = mesh.name.rfind("hero", 0) == 0;
        geometry.draw_ranges.push_back({first_index,
                                        static_cast<std::uint32_t>(mesh.indices.size()),
                                        std::min(mesh.material, NativeWorldRenderer::kMaxMaterialTextures / 3 - 1),
                                        is_water, is_character});
    }

    if (!geometry.vertices.empty()) return geometry;

    // Asset-free fallback: a small floor and pyramid prove the native world
    // handoff before a cooked level is available.
    geometry.vertices = {
        {{-2.0f, 0.0f, -2.0f}, {0.0f, 1.0f, 0.0f}, {0.18f, 0.30f, 0.42f, 1.0f}, {}},
        {{2.0f, 0.0f, -2.0f}, {0.0f, 1.0f, 0.0f}, {0.18f, 0.30f, 0.42f, 1.0f}, {}},
        {{2.0f, 0.0f, 2.0f}, {0.0f, 1.0f, 0.0f}, {0.18f, 0.30f, 0.42f, 1.0f}, {}},
        {{-2.0f, 0.0f, 2.0f}, {0.0f, 1.0f, 0.0f}, {0.18f, 0.30f, 0.42f, 1.0f}, {}},
        {{0.0f, 2.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.95f, 0.62f, 0.18f, 1.0f}, {}},
    };
    geometry.indices = {0, 1, 2, 0, 2, 3, 0, 4, 1, 1, 4, 2,
                        2, 4, 3, 3, 4, 0};
    geometry.draw_ranges.push_back({0, static_cast<std::uint32_t>(geometry.indices.size()), 0});
    return geometry;
}

bool create_upload_buffer(ID3D12Device* device, const void* data, std::size_t size,
                          Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
                          std::string& error) {
    const auto heap = upload_heap();
    const auto description = buffer_description(size);
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&resource)))) {
        error = "D3D12 could not allocate a world upload buffer.";
        return false;
    }
    void* mapped = nullptr;
    if (FAILED(resource->Map(0, nullptr, &mapped))) {
        error = "D3D12 could not map a world upload buffer.";
        return false;
    }
    std::memcpy(mapped, data, size);
    resource->Unmap(0, nullptr);
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
        const auto without_data = texture_root / std::filesystem::path(
            requested.lexically_relative(requested.root_path() / "data"));
        if (std::filesystem::is_regular_file(without_data)) return without_data;
    }
    return {};
}

bool create_texture(ID3D12Device* device, ID3D12CommandQueue* queue,
                    const NativeTexture& source,
                    Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
                    std::string& error) {
    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = source.width;
    texture_description.Height = source.height;
    texture_description.DepthOrArraySize = 1;
    texture_description.MipLevels = 1;
    texture_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_description.SampleDesc.Count = 1;
    texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    auto default_heap = upload_heap();
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &texture_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)))) {
        error = "D3D12 could not allocate the native texture.";
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0;
    UINT64 row_size = 0;
    UINT64 upload_size = 0;
    device->GetCopyableFootprints(&texture_description, 0, 1, 0, &footprint,
                                  &row_count, &row_size, &upload_size);
    const auto upload_description = buffer_description(upload_size);
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    const auto upload_heap_properties = upload_heap();
    if (FAILED(device->CreateCommittedResource(
            &upload_heap_properties, D3D12_HEAP_FLAG_NONE, &upload_description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) {
        error = "D3D12 could not allocate the native texture upload buffer.";
        return false;
    }
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) {
        error = "D3D12 could not map the native texture upload buffer.";
        return false;
    }
    auto* destination = static_cast<std::uint8_t*>(mapped) + footprint.Offset;
    const auto source_row_pitch = static_cast<std::size_t>(source.width) * 4;
    for (std::uint32_t row = 0; row < source.height; ++row) {
        std::memcpy(destination + row * footprint.Footprint.RowPitch,
                    source.rgba8.data() + row * source_row_pitch, source_row_pitch);
    }
    upload->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&command_list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        error = "D3D12 could not create the native texture upload commands.";
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION destination_location{};
    destination_location.pResource = texture.Get();
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination_location.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = upload.Get();
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source_location.PlacedFootprint = footprint;
    command_list->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(1, &barrier);
    if (FAILED(command_list->Close())) {
        error = "D3D12 could not close the native texture upload commands.";
        return false;
    }
    ID3D12CommandList* lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    constexpr UINT64 fence_value = 1;
    if (FAILED(queue->Signal(fence.Get(), fence_value))) {
        error = "D3D12 could not submit the native texture upload.";
        return false;
    }
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        error = "D3D12 could not create the native texture upload event.";
        return false;
    }
    if (fence->GetCompletedValue() < fence_value) {
        fence->SetEventOnCompletion(fence_value, event);
        WaitForSingleObject(event, INFINITE);
    }
    CloseHandle(event);
    return true;
}

}  // namespace

bool NativeWorldRenderer::initialise(ID3D12Device* device, ID3D12CommandQueue* queue,
                                     const NativeScene& scene,
                                     const std::filesystem::path& texture_root,
                                     D3D12_CPU_DESCRIPTOR_HANDLE texture_cpu_handle,
                                     D3D12_GPU_DESCRIPTOR_HANDLE texture_gpu_handle,
                                     std::string& error) {
    const auto geometry = make_geometry(scene);
    if (geometry.vertices.empty() || geometry.indices.empty()) {
        error = "The native world has no renderable geometry.";
        return false;
    }
    // Fit the camera to the baked world-space geometry (a cooked level spans hundreds
    // of units; the test pyramid spans a few) so render() frames whatever we loaded.
    // A cooked `focus` (town bounds excluding horizon backdrop props) takes precedence so the
    // ~1000wu Tattered Spire vista doesn't blow up the fit and shrink the town to a dot.
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
    if (!create_upload_buffer(device, geometry.vertices.data(),
                              geometry.vertices.size() * sizeof(Vertex), vertex_buffer_, error) ||
        !create_upload_buffer(device, geometry.indices.data(),
                              geometry.indices.size() * sizeof(std::uint32_t), index_buffer_, error)) {
        return false;
    }

    // Three descriptor slots per material: albedo (t0) + normal (t1) + spec/"material" (t2),
    // interleaved (world_shading_model_re.txt §7, ladder step 3).
    const auto material_count = std::max<std::size_t>(
        1, std::min<std::size_t>(scene.materials.size(), kMaxMaterialTextures / 3));
    texture_descriptor_stride_ = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    textures_.resize(material_count * 3);
    D3D12_SHADER_RESOURCE_VIEW_DESC texture_view{};
    texture_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    texture_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texture_view.Texture2D.MipLevels = 1;
    const auto make_srv = [&](std::size_t slot, const NativeTexture& src, std::string& err) -> bool {
        if (!create_texture(device, queue, src, textures_[slot], err)) return false;
        auto handle = texture_cpu_handle;
        handle.ptr += slot * texture_descriptor_stride_;
        device->CreateShaderResourceView(textures_[slot].Get(), &texture_view, handle);
        return true;
    };
    for (std::size_t material_index = 0; material_index < material_count; ++material_index) {
        NativeTexture albedo{1, 1, {255, 255, 255, 255}};
        NativeTexture normal{1, 1, {128, 128, 255, 255}};  // flat tangent-space normal default
        // Default spec/"material" mask = black → spec_mask 0 = no highlight (§3: default?0:sSamp.r).
        NativeTexture spec{1, 1, {0, 0, 0, 255}};
        if (material_index < scene.materials.size()) {
            const auto& material = scene.materials[material_index];
            const auto albedo_path = resolve_texture(material, texture_root);
            std::string texture_error;
            if (!albedo_path.empty()) load_dds_rgba8(albedo_path, albedo, texture_error);
            if (!material.normal.empty()) {
                NativeMaterial normal_ref;
                normal_ref.albedo = material.normal;  // reuse resolve_texture for the normal path
                const auto normal_path = resolve_texture(normal_ref, texture_root);
                if (!normal_path.empty()) load_dds_rgba8(normal_path, normal, texture_error);
            }
            if (!material.material.empty()) {
                NativeMaterial spec_ref;
                spec_ref.albedo = material.material;  // reuse resolve_texture for the spec path
                const auto spec_path = resolve_texture(spec_ref, texture_root);
                if (!spec_path.empty()) load_dds_rgba8(spec_path, spec, texture_error);
            }
        }
        if (!make_srv(material_index * 3, albedo, error)) return false;
        if (!make_srv(material_index * 3 + 1, normal, error)) return false;
        if (!make_srv(material_index * 3 + 2, spec, error)) return false;
    }
    texture_gpu_handle_ = texture_gpu_handle;
    draw_ranges_.clear();
    for (const auto& range : geometry.draw_ranges) {
        draw_ranges_.push_back({range.first_index, range.index_count, range.material_index,
                                range.is_water});
    }

    const auto constant_size = (sizeof(Constants) + 255u) & ~255u;
    const auto heap = upload_heap();
    const auto description = buffer_description(constant_size);
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&constant_buffer_))) ||
        FAILED(constant_buffer_->Map(0, nullptr, &mapped_constants_))) {
        error = "D3D12 could not allocate the world camera buffer.";
        return false;
    }
    constant_address_ = constant_buffer_->GetGPUVirtualAddress();

    // b1 Lights cbuffer: upload the cooked point lights once. Static (the light set
    // is fixed for a level), so it's an upload buffer written at init and never mapped
    // per frame. Additive to the sun/hemisphere term in the PS.
    Lights lights{};
    const auto light_n = std::min<std::size_t>(scene.lights.size(), kMaxPointLights);
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
    if (!create_upload_buffer(device, &lights, sizeof(lights), light_buffer_, error)) {
        return false;
    }
    light_address_ = light_buffer_->GetGPUVirtualAddress();

    const std::size_t water_stride = 256;
    const std::size_t water_size = std::max<std::size_t>(water_stride, material_count * water_stride);
    std::vector<std::uint8_t> water_data(water_size, 0);
    for (std::size_t material_index = 0; material_index < material_count; ++material_index) {
        WaterConstants constants{};
        if (material_index < scene.materials.size()) {
            const auto& material = scene.materials[material_index];
            for (std::size_t i = 0; i < material.water_params.size(); ++i)
                constants.params[i / 4][i % 4] = material.water_params[i];
            constants.params[9][1] = material.water_opacity;
        }
        std::memcpy(water_data.data() + material_index * water_stride, &constants,
                    sizeof(constants));
    }
    if (!create_upload_buffer(device, water_data.data(), water_data.size(), water_buffer_, error)) {
        return false;
    }
    water_address_ = water_buffer_->GetGPUVirtualAddress();

    Microsoft::WRL::ComPtr<ID3DBlob> vertex_shader;
    Microsoft::WRL::ComPtr<ID3DBlob> pixel_shader;
    Microsoft::WRL::ComPtr<ID3DBlob> water_pixel_shader;
    Microsoft::WRL::ComPtr<ID3DBlob> shader_errors;
    constexpr char shader_source[] = R"(
cbuffer Camera : register(b0) {
    row_major float4x4 view_projection;
    float4 sun_direction;
    float4 eye_time;
    float4 sun_color;
    float4 fog_color;   // rgb = fog colour, w = max density (0 = off)
    float4 fog_range;   // x = start dist, y = end dist
    float4 viewport_size;
};
// Local point lights (level_lights_effects_re.txt §3.1): lamp posts, lanterns, braziers.
cbuffer Lights : register(b1) {
    uint light_count; float3 _light_pad;
    float4 light_pos_range[64];        // xyz = render-space pos, w = range (wu)
    float4 light_color_intensity[64];  // rgb = colour (0..1), w = intensity
};
cbuffer Water : register(b2) { float4 water_params[10]; };
cbuffer DrawFlags : register(b3) {
    uint is_character;
    float3 character_offset;
    float4 character_motion;
};
Texture2D albedo : register(t0);
Texture2D normalTex : register(t1);
Texture2D specTex : register(t2);
Texture2D<float> scene_depth : register(t3);
SamplerState albedo_sampler : register(s0);
struct VSInput { float3 position : POSITION; float3 normal : NORMAL; float4 color : COLOR0; float2 uv : TEXCOORD0; float4 probe : COLOR1; };
struct PSInput { float4 position : SV_POSITION; float3 normal : NORMAL; float3 world_pos : TEXCOORD1; float4 color : COLOR0; float2 uv : TEXCOORD0; float4 probe : COLOR1; };
PSInput vs_main(VSInput input) {
    PSInput output;
    float3 motion = float3(0.0, sin(character_motion.x) * 0.045 * character_motion.y, 0.0);
    float3 world_position = input.position +
                            (is_character != 0 ? character_offset + motion : 0.0);
    output.position = mul(float4(world_position, 1.0), view_projection);
    output.normal = input.normal;
    output.world_pos = world_position;
    output.color = input.color;
    output.uv = input.uv;
    output.probe = input.probe;
    return output;
}
float4 ps_main(PSInput input) : SV_TARGET {
    float4 base = input.color * albedo.Sample(albedo_sampler, input.uv);
    // Alpha-test cutout: foliage (leaves/grass) textures carry punch-through alpha (DXT1
    // 1-bit), so discard transparent texels — otherwise leaf quads render as solid cards.
    // Opaque building textures decode to alpha=1, so they are unaffected.
    clip(base.a - 0.5);
    // Normal mapping (world_shading_model_re.txt §7, ladder step 2): perturb the geometric
    // normal by the 2-channel BC5 tangent-space normal, using a derivative (ddx/ddy) cotangent
    // frame so no per-vertex tangent is needed. A flat (128,128,255) default = no perturbation.
    float3 Ngeo = normalize(input.normal);
    float3 dp1 = ddx(input.world_pos), dp2 = ddy(input.world_pos);
    float2 du1 = ddx(input.uv), du2 = ddy(input.uv);
    float3 dp2perp = cross(dp2, Ngeo), dp1perp = cross(Ngeo, dp1);
    float3 T = dp2perp * du1.x + dp1perp * du2.x;
    float3 B = dp2perp * du1.y + dp1perp * du2.y;
    float invmax = rsqrt(max(dot(T, T), dot(B, B)));
    float2 nxy = normalTex.Sample(albedo_sampler, input.uv).rg * 2.0 - 1.0;
    float nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
    float3 N = normalize(nxy.x * T * invmax + nxy.y * B * invmax + nz * Ngeo);
    // Light model (§7): hemisphere ambient (cool sky above, dim ground bounce below by world-up
    // N.y) + N·L sun diffuse — replaces the flat 0.35 that read dark/flat.
    float ndl = saturate(dot(N, -sun_direction.xyz));
    float hemi = 0.5 + 0.5 * N.y;
    float3 ambient = lerp(float3(0.18, 0.20, 0.24), float3(0.55, 0.58, 0.62), hemi);
    // Per-prop baked ambient (.lmp LightmapFile SH probe DC term, world_shading §lmp): when a
    // probe is present (probe.w>0.5) it replaces the synthetic hemisphere floor, so static
    // props get their real baked GI (fixes the dark building faces). No-probe geometry (terrain,
    // foliage) keeps the hemisphere fallback unchanged.
    if (input.probe.w > 0.5) ambient = input.probe.rgb;
    // Warm directional sun (theme main_light_colour) tints the N.L term; ambient stays the
    // baked/hemisphere term. Cool ambient + warm sun = the retail daytime split.
    float3 lit = base.rgb * (ambient + ndl * sun_color.rgb);
    // Specular highlight (world_shading_model_re.txt §7, ladder step 3): Blinn-Phong gated by the
    // spec/"material" mask (t2). Grayscale mask.r modulates a pow(N·H, k) lobe using the sun as the
    // key light and the camera eye (eye_time.xyz) for the view vector. Default mask=0 → no spec.
    float specMask = specTex.Sample(albedo_sampler, input.uv).r;
    float3 Vdir = normalize(eye_time.xyz - input.world_pos);
    float3 Hdir = normalize(-sun_direction.xyz + Vdir);
    float spec = pow(saturate(dot(N, Hdir)), 32.0) * specMask;
    lit += spec * sun_color.rgb;
    // Additive local point lights (level_lights_effects_re.txt §3.1): diffuse N·L with a
    // soft linear-squared falloff clamped at each light's Range. Added AFTER the
    // hemisphere+sun term so lamps/braziers glow warm over the global lighting.
    [loop] for (uint li = 0; li < light_count; ++li) {
        float3 d = light_pos_range[li].xyz - input.world_pos;
        float r = light_pos_range[li].w;
        float dist = length(d);
        if (dist < r) {
            float3 L = d / max(dist, 1e-3);
            float ndl_p = saturate(dot(N, L));
            float atten = saturate(1.0 - dist / r);
            atten *= atten;  // ~inverse-square feel
            lit += base.rgb * light_color_intensity[li].rgb *
                   (ndl_p * atten * light_color_intensity[li].w);
        }
    }
    // Distance fog toward the theme fog colour (fog_color.w = max density, 0 = off). Ties distant
    // world geometry to the horizon/backdrop. Matches the Vulkan world PS.
    if (fog_color.w > 0.0) {
        float fd = length(eye_time.xyz - input.world_pos);
        float f = saturate((fd - fog_range.x) / max(fog_range.y - fog_range.x, 1.0)) * fog_color.w;
        lit = lerp(lit, fog_color.rgb, f);
    }
    float3 color = lit;
    return float4(color, base.a);
}
// Animated translucent WATER (water_system_re.txt §5): authored dual-scrolled bump normal,
// Fresnel deep↔surface colour, sky reflection, and sun glitter. All level-specific values arrive
// through WaterConstants (b2), never as chapter2slums shader literals.
float4 ps_water(PSInput input) : SV_TARGET {
    float3 wp = input.world_pos;
    float t = eye_time.w;
    float2 p = wp.xz;
    float fresnel_bias = water_params[0].x;
    float reflection_bias = water_params[0].y;
    float2 uv0 = p * float2(water_params[1].z, water_params[1].w) +
                 float2(water_params[0].z, water_params[0].w) * t;
    float2 uv1 = p * float2(water_params[2].x, water_params[2].y) +
                 float2(water_params[1].x, water_params[1].y) * t;
    float2 n0 = normalTex.Sample(albedo_sampler, uv0).xy * 2.0 - 1.0;
    float2 n1 = normalTex.Sample(albedo_sampler, uv1).xy * 2.0 - 1.0;
    float2 nxy = (n0 + n1) * 0.35;                        // damped authored ripple slope
    // Retail's reflection stand-in uses m_ReflectionScale (params[25]) as a scalar for
    // both components; params[26] is retained for the dropped screen-space refraction tile.
    float3 N = normalize(float3(nxy.x, 1.0, nxy.y));
    float3 V = normalize(eye_time.xyz - wp);
    // Retail's Fresnel normal is intentionally almost horizontal; its authored NORMAL_SCALE is
    // WaterFile::params[24], not the upward reflection normal.
    // Keep reflection and glitter on a broad upward normal; the high-frequency normal map
    // produces white noise when applied directly to this term.
    float3 Nf = normalize(float3(nxy.x, 1.0, nxy.y));
    float fres = fresnel_bias +
                 (1.0 - fresnel_bias) * pow(1.0 - saturate(dot(V, Nf)), 5.0);
    float3 SURFACE = float3(water_params[4].z, water_params[4].w, water_params[5].x);
    float3 DEEP = water_params[5].yzw;
    float3 watercol = lerp(DEEP, SURFACE, fres);
    float3 reflection_ray = reflect(-V, N);
    reflection_ray.y = abs(reflection_ray.y);
    float sky_t = saturate(reflection_ray.y * 0.5 + 0.5);
    // The reflection stand-in follows the same resolved chapter2slums theme endpoints as the
    // native sky pass: complementary horizon → sky_colour zenith (env_theme_colors_re §0).
    float3 sky = lerp(float3(0.222, 0.5789, 1.11),
                      float3(0.6549, 0.8157, 1.0), sky_t);
    float fres_reflect = saturate(fres + reflection_bias);
    float distf = saturate(length(eye_time.xyz - wp) / 75.0);
    float refl_strength = saturate(water_params[7].y);
    float refl = refl_strength * lerp(fres_reflect, 1.0, distf);
    float3 col = watercol * (1.0 - refl_strength) + sky * refl;
    float3 L = normalize(sun_direction.xyz);             // authored light-travel direction
    float3 Ng = Nf;
    float glit = pow(saturate(dot(V, reflect(L, Ng))), water_params[9].x) *
                 water_params[8].w;
    // Keep the authored glitter power, but attenuate its brightness for the native HDR-less
    // target; the retail compositor applies an exposure stage that is not present here.
    col += glit * sun_color.rgb * 0.25;
    // The authored PF40 normal map supplies the water ripple detail and glitter response.
    // Retail emits the refraction coefficient as alpha; with ONE/SRC_ALPHA blending the
    // framebuffer behind the surface supplies the scene/refraction term. The actual retail
    // scene-depth edge factor is unavailable until the depth copy is exposed to this pass.
    float refr_k = (1.0 - distf) * refl_strength * (1.0 - fres_reflect);
    // Reversed-Z depth delta: a flat water surface has a larger depth value than the
    // terrain beneath it. Near the authored shoreline the values converge, so soften the
    // refraction alpha instead of leaving a hard polygon edge. The copied depth texture
    // contains 0 for the sky/far clear, which intentionally leaves open water fully visible.
    float2 screen_uv = saturate(input.position.xy / viewport_size.xy);
    float scene_z = scene_depth.SampleLevel(albedo_sampler, screen_uv, 0);
    float water_z = input.position.z;
    float shoreline = lerp(0.05, 1.0, saturate((water_z - scene_z) * 256.0));
    refr_k *= shoreline;
    // Distance fog on the water surface too (coherent with opaque geometry).
    if (fog_color.w > 0.0) {
        float ffd = length(eye_time.xyz - wp);
        float ff = saturate((ffd - fog_range.x) / max(fog_range.y - fog_range.x, 1.0)) * fog_color.w;
        col = lerp(col, fog_color.rgb, ff);
    }
    return float4(col, saturate(refr_k));
}
)";
    const auto compile = [&](const char* entry, const char* target,
                             Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
        return D3DCompile(shader_source, sizeof(shader_source) - 1, "native_world.hlsl",
                          nullptr, nullptr, entry, target, 0, 0, &blob, &shader_errors);
    };
    if (FAILED(compile("vs_main", "vs_5_0", vertex_shader)) ||
        FAILED(compile("ps_main", "ps_5_0", pixel_shader)) ||
        FAILED(compile("ps_water", "ps_5_0", water_pixel_shader))) {
        error = "The native world shaders could not be compiled.";
        return false;
    }

    D3D12_ROOT_PARAMETER root_parameters[6]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // PS reads sun_direction too
    D3D12_DESCRIPTOR_RANGE texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = 3;  // t0 albedo + t1 normal + t2 spec
    texture_range.BaseShaderRegister = 0;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &texture_range;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // b1 point-light array (PS only). New param index 2 — leaves b0 (index 0) and the
    // texture table (index 1) undisturbed.
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[2].Descriptor.ShaderRegister = 1;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[3].Descriptor.ShaderRegister = 2;
    root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE depth_range{};
    depth_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depth_range.NumDescriptors = 1;
    depth_range.BaseShaderRegister = 3;
    root_parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[4].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[4].DescriptorTable.pDescriptorRanges = &depth_range;
    root_parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[5].Constants.ShaderRegister = 3;
    root_parameters[5].Constants.Num32BitValues = 8;
    root_parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_description{};
    root_description.NumParameters = 6;
    root_description.pParameters = root_parameters;
    root_description.NumStaticSamplers = 1;
    root_description.pStaticSamplers = &sampler;
    root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> root_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_description,
                                           D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
                                           &shader_errors)) ||
        FAILED(device->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                            root_blob->GetBufferSize(),
                                            IID_PPV_ARGS(&root_signature_)))) {
        error = "The native world root signature could not be created.";
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_signature_.Get();
    pipeline.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
    pipeline.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
    pipeline.InputLayout = {input_layout, static_cast<UINT>(std::size(input_layout))};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipeline.SampleDesc.Count = 1;
    pipeline.SampleMask = 0xFFFFFFFFu;  // 0 (zero-init default) writes no samples -> nothing renders
    pipeline.RasterizerState = rasterizer_description();
    pipeline.BlendState = blend_description();
    pipeline.DepthStencilState.DepthEnable = TRUE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    // REVERSED-Z: near maps to 1, far to 0 (cleared to 0), so the huge near..far span of a
    // town + horizon-vista scene keeps floating-point depth precision even under the free-fly
    // camera's small near plane — kills the Z-fighting that showed as "light through seams" on
    // terrain/castle when the camera rotated. Pairs with the reversed z-row in render() and the
    // 0.0 depth clear in native_frontend_app.
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pipeline.DepthStencilState.StencilEnable = FALSE;
    pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    if (FAILED(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pipeline_state_)))) {
        error = "The native world pipeline could not be created.";
        return false;
    }

    // Water pipeline: same VS/layout, the animated water PS, retail ONE/SRC_ALPHA color blend,
    // depth-test on but depth-WRITE off (translucent surface over the terrain).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC water = pipeline;
    water.PS = {water_pixel_shader->GetBufferPointer(), water_pixel_shader->GetBufferSize()};
    auto& wt = water.BlendState.RenderTarget[0];
    wt.BlendEnable = TRUE;
    wt.SrcBlend = D3D12_BLEND_ONE;
    wt.DestBlend = D3D12_BLEND_SRC_ALPHA;
    wt.BlendOp = D3D12_BLEND_OP_ADD;
    wt.SrcBlendAlpha = D3D12_BLEND_ONE;
    wt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    wt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    wt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    water.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateGraphicsPipelineState(&water, IID_PPV_ARGS(&water_pipeline_)))) {
        error = "The native world water pipeline could not be created.";
        return false;
    }

    vertex_view_.BufferLocation = vertex_buffer_->GetGPUVirtualAddress();
    vertex_view_.SizeInBytes = static_cast<UINT>(geometry.vertices.size() * sizeof(Vertex));
    vertex_view_.StrideInBytes = sizeof(Vertex);
    index_view_.BufferLocation = index_buffer_->GetGPUVirtualAddress();
    index_view_.SizeInBytes = static_cast<UINT>(geometry.indices.size() * sizeof(std::uint32_t));
    index_view_.Format = DXGI_FORMAT_R32_UINT;
    index_count_ = static_cast<std::uint32_t>(geometry.indices.size());
    return true;
}

SkyCamera NativeWorldRenderer::compute_camera(std::uint32_t width, std::uint32_t height,
                                              double elapsed_seconds) const {
    const float angle = static_cast<float>(elapsed_seconds * 0.25);
    const float dist = scene_radius_ * 2.4f;
    // Free-fly override: eye + yaw/pitch look direction (level inspection). Otherwise the
    // auto-orbit that frames the whole scene.
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
    const float aspect =
        height == 0 ? 1.0f : static_cast<float>(width) / static_cast<float>(height);
    const float tan_half_y = std::tan(0.5f);  // render() uses y_scale = 1/tan(0.5)
    SkyCamera cam;
    cam.position = eye;
    cam.right = right;
    cam.up = camera_up;
    cam.forward = forward;
    cam.tan_half_fov_x = tan_half_y * aspect;
    cam.tan_half_fov_y = tan_half_y;
    return cam;
}

void NativeWorldRenderer::render(ID3D12GraphicsCommandList* command_list,
                                 const NativeScene& scene, std::uint32_t width,
                                 std::uint32_t height, double elapsed_seconds) {
    if (!pipeline_state_ || width == 0 || height == 0) return;

    const auto camera = compute_camera(width, height, elapsed_seconds);
    const std::array<float, 3> eye = camera.position;
    const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    const auto forward = camera.forward;
    const auto right = camera.right;
    const auto camera_up = camera.up;
    const float eye_dot_right = dot(eye, right);
    const float eye_dot_up = dot(eye, camera_up);
    const float eye_dot_forward = dot(eye, forward);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float y_scale = 1.0f / std::tan(0.5f);
    const float x_scale = y_scale / aspect;
    // A small near plane when free-flying so close geometry doesn't clip; the orbit sits
    // far enough out to afford a generous near for depth precision.
    const float near_plane = free_camera_ ? 0.5f : std::max(0.1f, scene_radius_ * 0.05f);
    const float far_plane = scene_radius_ * 8.0f + 10.0f;
    Constants constants{};
    constants.view_projection[0][0] = right[0] * x_scale;
    constants.view_projection[1][0] = right[1] * x_scale;
    constants.view_projection[2][0] = right[2] * x_scale;
    constants.view_projection[3][0] = -eye_dot_right * x_scale;
    constants.view_projection[0][1] = camera_up[0] * y_scale;
    constants.view_projection[1][1] = camera_up[1] * y_scale;
    constants.view_projection[2][1] = camera_up[2] * y_scale;
    constants.view_projection[3][1] = -eye_dot_up * y_scale;
    // Reversed-Z depth row: clip.z = near/(far-near) * (far - view_depth); with clip.w =
    // view_depth this gives z_ndc = near→1, far→0 (see DepthFunc GREATER_EQUAL + 0.0 clear).
    const float rz = near_plane / (far_plane - near_plane);
    constants.view_projection[0][2] = -rz * forward[0];
    constants.view_projection[1][2] = -rz * forward[1];
    constants.view_projection[2][2] = -rz * forward[2];
    constants.view_projection[3][2] = rz * (eye_dot_forward + far_plane);
    constants.view_projection[0][3] = forward[0];
    constants.view_projection[1][3] = forward[1];
    constants.view_projection[2][3] = forward[2];
    constants.view_projection[3][3] = -eye_dot_forward;
    const auto sun = normalise(scene.sun_direction);
    constants.sun_direction[0] = sun[0];
    constants.sun_direction[1] = sun[1];
    constants.sun_direction[2] = sun[2];
    constants.sun_direction[3] = 0.0f;
    constants.sun_color[0] = scene.sun_color[0];
    constants.sun_color[1] = scene.sun_color[1];
    constants.sun_color[2] = scene.sun_color[2];
    constants.sun_color[3] = 0.0f;
    constants.viewport_size[0] = static_cast<float>(width);
    constants.viewport_size[1] = static_cast<float>(height);
    constants.eye_time[0] = eye[0];
    constants.eye_time[1] = eye[1];
    constants.eye_time[2] = eye[2];
    constants.eye_time[3] = static_cast<float>(elapsed_seconds);
    constants.fog_color[0] = scene.fog_color[0];
    constants.fog_color[1] = scene.fog_color[1];
    constants.fog_color[2] = scene.fog_color[2];
    constants.fog_color[3] = scene.fog_max;
    constants.fog_range[0] = scene.fog_start;
    constants.fog_range[1] = scene.fog_end;
    std::memcpy(mapped_constants_, &constants, sizeof(constants));

    // The world render must set its OWN viewport/scissor — nothing else does before it,
    // and an unset (0x0) viewport rasterises no pixels (silent: no validation error).
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width),
                                  static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);

    command_list->SetGraphicsRootSignature(root_signature_.Get());
    command_list->SetGraphicsRootConstantBufferView(0, constant_address_);
    command_list->SetGraphicsRootDescriptorTable(1, texture_gpu_handle_);
    command_list->SetGraphicsRootConstantBufferView(2, light_address_);  // b1 point lights
    const bool depth_copy_ready = scene_depth_source_ && scene_depth_copy_;
    if (depth_copy_ready) {
        command_list->SetGraphicsRootDescriptorTable(4, scene_depth_gpu_handle_);
    }
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 1, &vertex_view_);
    command_list->IASetIndexBuffer(&index_view_);
    // Opaque pass first, then the translucent water pass over it (so water blends with the
    // terrain/riverbed already in the depth+colour buffers).
    const auto draw_pass = [&](bool water, ID3D12PipelineState* pso) {
        command_list->SetPipelineState(pso);
        for (const auto& range : draw_ranges_) {
            if (range.is_water != water) continue;
            auto texture_handle = texture_gpu_handle_;
            texture_handle.ptr += static_cast<std::size_t>(range.material_index) * 3 *
                                 texture_descriptor_stride_;
            command_list->SetGraphicsRootDescriptorTable(1, texture_handle);
            command_list->SetGraphicsRootConstantBufferView(
                3, water_address_ + static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(range.material_index) * 256);
            struct DrawConstants {
                std::uint32_t is_character;
                float character_offset[3];
                float character_motion[4];
            } draw_constants{range.is_character ? 1u : 0u,
                             {character_offset_[0], character_offset_[1], character_offset_[2]},
                             {character_motion_phase_, character_motion_strength_, 0.0f, 0.0f}};
            command_list->SetGraphicsRoot32BitConstants(5, 8, &draw_constants, 0);
            command_list->DrawIndexedInstanced(range.index_count, 1, range.first_index, 0, 0);
        }
    };
    draw_pass(false, pipeline_state_.Get());
    if (depth_copy_ready) {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = scene_depth_source_;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = scene_depth_copy_;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(2, barriers);
        command_list->CopyResource(scene_depth_copy_, scene_depth_source_);
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        command_list->ResourceBarrier(2, barriers);
    }
    // The water shader samples the copied depth unconditionally. If depth resources could not
    // be created (for example during a transient device/resize failure), keep the opaque scene
    // valid and avoid issuing a draw with an unbound t3 descriptor.
    if (depth_copy_ready) draw_pass(true, water_pipeline_.Get());
}

}  // namespace f2
