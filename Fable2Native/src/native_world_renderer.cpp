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
    std::array<float, 4> color{};
    std::array<float, 2> uv{};
};

struct Constants {
    float view_projection[4][4]{};
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
    description.CullMode = D3D12_CULL_MODE_BACK;
    description.FrontCounterClockwise = FALSE;
    description.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.DepthClipEnable = TRUE;
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
                color,
                source.uv});
        }
        const auto first_index = static_cast<std::uint32_t>(geometry.indices.size());
        for (const auto index : mesh.indices) geometry.indices.push_back(base + index);
        geometry.draw_ranges.push_back({first_index,
                                        static_cast<std::uint32_t>(mesh.indices.size()),
                                        std::min(mesh.material, NativeWorldRenderer::kMaxMaterialTextures - 1)});
    }

    if (!geometry.vertices.empty()) return geometry;

    // Asset-free fallback: a small floor and pyramid prove the native world
    // handoff before a cooked level is available.
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
    if (!create_upload_buffer(device, geometry.vertices.data(),
                              geometry.vertices.size() * sizeof(Vertex), vertex_buffer_, error) ||
        !create_upload_buffer(device, geometry.indices.data(),
                              geometry.indices.size() * sizeof(std::uint32_t), index_buffer_, error)) {
        return false;
    }

    const auto material_count = std::max<std::size_t>(
        1, std::min<std::size_t>(scene.materials.size(), kMaxMaterialTextures));
    texture_descriptor_stride_ = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    textures_.resize(material_count);
    D3D12_SHADER_RESOURCE_VIEW_DESC texture_view{};
    texture_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    texture_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texture_view.Texture2D.MipLevels = 1;
    for (std::size_t material_index = 0; material_index < material_count; ++material_index) {
        NativeTexture native_texture{1, 1, {255, 255, 255, 255}};
        if (material_index < scene.materials.size()) {
            const auto texture_path = resolve_texture(scene.materials[material_index], texture_root);
            if (!texture_path.empty()) {
                std::string texture_error;
                load_dds_rgba8(texture_path, native_texture, texture_error);
            }
        }
        if (!create_texture(device, queue, native_texture, textures_[material_index], error)) return false;
        auto material_cpu_handle = texture_cpu_handle;
        material_cpu_handle.ptr += material_index * texture_descriptor_stride_;
        device->CreateShaderResourceView(textures_[material_index].Get(), &texture_view,
                                         material_cpu_handle);
    }
    texture_gpu_handle_ = texture_gpu_handle;
    draw_ranges_.clear();
    for (const auto& range : geometry.draw_ranges) {
        draw_ranges_.push_back({range.first_index, range.index_count, range.material_index});
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

    Microsoft::WRL::ComPtr<ID3DBlob> vertex_shader;
    Microsoft::WRL::ComPtr<ID3DBlob> pixel_shader;
    Microsoft::WRL::ComPtr<ID3DBlob> shader_errors;
    constexpr char shader_source[] = R"(
cbuffer Camera : register(b0) { float4x4 view_projection; };
Texture2D albedo : register(t0);
SamplerState albedo_sampler : register(s0);
struct VSInput { float3 position : POSITION; float4 color : COLOR; float2 uv : TEXCOORD0; };
struct PSInput { float4 position : SV_POSITION; float4 color : COLOR; float2 uv : TEXCOORD0; };
PSInput vs_main(VSInput input) {
    PSInput output;
    output.position = mul(float4(input.position, 1.0), view_projection);
    output.color = input.color;
    output.uv = input.uv;
    return output;
}
float4 ps_main(PSInput input) : SV_TARGET { return input.color * albedo.Sample(albedo_sampler, input.uv); }
)";
    const auto compile = [&](const char* entry, const char* target,
                             Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
        return D3DCompile(shader_source, sizeof(shader_source) - 1, "native_world.hlsl",
                          nullptr, nullptr, entry, target, 0, 0, &blob, &shader_errors);
    };
    if (FAILED(compile("vs_main", "vs_5_0", vertex_shader)) ||
        FAILED(compile("ps_main", "ps_5_0", pixel_shader))) {
        error = "The native world shaders could not be compiled.";
        return false;
    }

    D3D12_ROOT_PARAMETER root_parameters[2]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[0].Descriptor.ShaderRegister = 0;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_DESCRIPTOR_RANGE texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = 1;
    texture_range.BaseShaderRegister = 0;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &texture_range;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_description{};
    root_description.NumParameters = 2;
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
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
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
    pipeline.RasterizerState = rasterizer_description();
    pipeline.BlendState = blend_description();
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.StencilEnable = FALSE;
    if (FAILED(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pipeline_state_)))) {
        error = "The native world pipeline could not be created.";
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

void NativeWorldRenderer::render(ID3D12GraphicsCommandList* command_list,
                                 const NativeScene&, std::uint32_t width,
                                 std::uint32_t height, double elapsed_seconds) {
    if (!pipeline_state_ || width == 0 || height == 0) return;

    const float angle = static_cast<float>(elapsed_seconds * 0.25);
    const std::array<float, 3> eye{std::sin(angle) * 7.0f, 4.0f, std::cos(angle) * 7.0f};
    const std::array<float, 3> target{0.0f, 0.7f, 0.0f};
    const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    const auto forward = normalise(subtract(target, eye));
    const auto right = normalise(cross(up, forward));
    const auto camera_up = cross(forward, right);
    const float eye_dot_right = dot(eye, right);
    const float eye_dot_up = dot(eye, camera_up);
    const float eye_dot_forward = dot(eye, forward);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float y_scale = 1.0f / std::tan(0.5f);
    const float x_scale = y_scale / aspect;
    const float near_plane = 0.1f;
    const float far_plane = 100.0f;
    Constants constants{};
    constants.view_projection[0][0] = right[0] * x_scale;
    constants.view_projection[1][0] = right[1] * x_scale;
    constants.view_projection[2][0] = right[2] * x_scale;
    constants.view_projection[3][0] = -eye_dot_right * x_scale;
    constants.view_projection[0][1] = camera_up[0] * y_scale;
    constants.view_projection[1][1] = camera_up[1] * y_scale;
    constants.view_projection[2][1] = camera_up[2] * y_scale;
    constants.view_projection[3][1] = -eye_dot_up * y_scale;
    constants.view_projection[0][2] = forward[0] * far_plane / (far_plane - near_plane);
    constants.view_projection[1][2] = forward[1] * far_plane / (far_plane - near_plane);
    constants.view_projection[2][2] = forward[2] * far_plane / (far_plane - near_plane);
    constants.view_projection[3][2] =
        (near_plane * eye_dot_forward * far_plane) / (far_plane - near_plane);
    constants.view_projection[0][3] = forward[0];
    constants.view_projection[1][3] = forward[1];
    constants.view_projection[2][3] = forward[2];
    constants.view_projection[3][3] = -eye_dot_forward;
    std::memcpy(mapped_constants_, &constants, sizeof(constants));

    command_list->SetPipelineState(pipeline_state_.Get());
    command_list->SetGraphicsRootSignature(root_signature_.Get());
    command_list->SetGraphicsRootConstantBufferView(0, constant_address_);
    command_list->SetGraphicsRootDescriptorTable(1, texture_gpu_handle_);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 1, &vertex_view_);
    command_list->IASetIndexBuffer(&index_view_);
    for (const auto& range : draw_ranges_) {
        auto texture_handle = texture_gpu_handle_;
        texture_handle.ptr += static_cast<std::size_t>(range.material_index) *
                             texture_descriptor_stride_;
        command_list->SetGraphicsRootDescriptorTable(1, texture_handle);
        command_list->DrawIndexedInstanced(range.index_count, 1, range.first_index, 0, 0);
    }
}

}  // namespace f2
