#include "f2/native_cloud_renderer.h"

#include "f2/native_texture.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

// -----------------------------------------------------------------------------
// Fable II scrolling cloud layers (native D3D12 port).
//
// PORT SOURCE (do-not-re-derive): Fable2AssetBrowser SkyboxRenderer.cpp kCloudVertexShader /
// kCloudPixelShader + CloudRuntime.{h,cpp} (BuildVertices / BuildConfig / ShaderNormalStrength /
// AlphaTestThreshold). RE authority: ghidra_out/sky_system_re.txt §Phase-2. Each theme cloud
// Layer is a density-map texture (density in .a, tint in .rgb) scrolled across a flat quad at the
// layer's height. The cook (cook_levels.py) resolves the theme Clouds record -> cloud_layer opcodes.
//
// This pass draws AFTER the sky atmosphere and BEFORE the opaque world, alpha-blended with no
// depth test/write, layers sorted high->low (back-to-front). It shares the world renderer's exact
// view_projection so the cloud quads line up with the world; the world opaque pass then draws over
// the lower part of the screen, leaving the clouds as the distant backdrop band.
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

struct CloudVertex {
    float x, y, z;
    float u, v;
};
static_assert(sizeof(CloudVertex) == 20);

// GPU cbuffer (matches the HLSL below; 11 * 16 bytes like the AB CloudConstants).
struct CloudConstants {
    float view_projection[16];   // row-major
    float viewer_position[4];    // xyz eye, w = 1
    float viewer_direction[4];   // xyz forward
    float light_position[4];
    float light_colour[4];
    float layer_params[4];       // x=transparency y=ambient z=brightness w=ShaderNormalStrength
    float uv_scale_offset[4];    // xy = texture scale, zw = accumulated scroll offset
    float cloud_globals[4];      // x = global brightness, z = alpha-test reference
};
static_assert(sizeof(CloudConstants) == 11 * 16);

constexpr char kCloudShaderSource[] = R"(
cbuffer CloudCB : register(b0) {
    row_major float4x4 view_projection;
    float4 viewer_position;
    float4 viewer_direction;
    float4 light_position;
    float4 light_colour;
    float4 layer_params;      // x=transparency y=ambient z=brightness w=normal-up
    float4 uv_scale_offset;   // xy=scale, zw=scroll offset
    float4 cloud_globals;     // x=global brightness, z=alpha ref
}
Texture2D cloud_density : register(t0);
SamplerState cloud_sampler : register(s0);

struct VSIN  { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct VSOUT {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

VSOUT vs_main(VSIN input) {
    VSOUT o;
    // Vertices are already in render space (X, height, Z); the cook bakes the XEX Z-up->Y-up swap.
    o.world_position = input.position;
    o.position = mul(float4(input.position, 1.0), view_projection);
    o.uv = input.uv;
    return o;
}

// Faithful port of SkyboxRenderer.cpp kCloudPixelShader: density in .a, distance fade beyond
// 1000u, 4-neighbour gradient normal lighting, alpha-test discard at the retail 5/255 reference.
float4 ps_main(VSOUT input) : SV_Target {
    const float2 uv = input.uv * uv_scale_offset.xy + uv_scale_offset.zw;
    const float4 centre = cloud_density.Sample(cloud_sampler, uv);
    const float density = centre.a;

    const float distance_xy = length(input.world_position.xz - viewer_position.xz);
    const float distance_fade = 1.0 - saturate((distance_xy - 1000.0) * 0.001);
    const float alpha = density * layer_params.x * distance_fade;

    float3 rgb = 0.0;
    [branch]
    if (alpha > 0.001) {
        const float plus_x  = cloud_density.Sample(cloud_sampler, uv, int2( 1,  0)).a;
        const float plus_y  = cloud_density.Sample(cloud_sampler, uv, int2( 0,  1)).a;
        const float minus_x = cloud_density.Sample(cloud_sampler, uv, int2(-1,  0)).a;
        const float minus_y = cloud_density.Sample(cloud_sampler, uv, int2( 0, -1)).a;
        const float3 normal = normalize(float3(plus_x - minus_x, layer_params.w, plus_y - minus_y));
        const float3 light_direction = normalize(input.world_position - light_position.xyz);
        const float grazing = 1.0 - saturate(dot(normal, -viewer_direction.xyz));
        const float back  = saturate(dot(light_direction, -normal));
        const float front = saturate(dot(light_direction, normal));
        const float lighting = front * (1.0 + pow(grazing, 32.0)) + back * back * (1.0 - density);
        rgb = ((lighting * light_colour.rgb + layer_params.yyy) *
               layer_params.zzz * centre.rgb) * cloud_globals.x;
    }
    // Xenos fixed-function strict-greater alpha test (context 2 = 5/255).
    if (alpha <= cloud_globals.z) discard;
    return float4(rgb, alpha);
}
)";

D3D12_HEAP_PROPERTIES upload_heap() {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = D3D12_HEAP_TYPE_UPLOAD;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_DESC buffer_desc(std::size_t size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

std::array<float, 3> normalise3(std::array<float, 3> v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l <= 1e-6f) return {0.0f, 1.0f, 0.0f};
    return {v[0] / l, v[1] / l, v[2] / l};
}

// CloudRuntime::ShaderNormalStrength — the shader's normal-up term = clamp(1 - authored, 0.1, 2).
float shader_normal_strength(float authored) {
    float t = 1.0f - authored;
    if (!(t >= 0.1f)) t = 0.1f;
    if (t > 2.0f) t = 2.0f;
    return t;
}

// Upload an RGBA8 texture to a DEFAULT-heap Texture2D and leave it in PIXEL_SHADER_RESOURCE.
Microsoft::WRL::ComPtr<ID3D12Resource> upload_texture_rgba8(ID3D12Device* device,
                                                            ID3D12CommandQueue* queue,
                                                            const NativeTexture& tex) {
    using Microsoft::WRL::ComPtr;
    if (tex.width == 0 || tex.height == 0 ||
        tex.rgba8.size() < std::size_t(tex.width) * tex.height * 4) {
        return nullptr;
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = tex.width;
    desc.Height = tex.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES def = upload_heap();
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&texture)))) {
        return nullptr;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_size = 0, upload_size = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &upload_size);
    const auto up_desc = buffer_desc(upload_size);
    const auto up_heap = upload_heap();
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&upload)))) {
        return nullptr;
    }
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return nullptr;
    auto* dst = static_cast<std::uint8_t*>(mapped) + footprint.Offset;
    const auto* src = tex.rgba8.data();
    for (UINT y = 0; y < tex.height; ++y) {
        std::memcpy(dst + std::size_t(y) * footprint.Footprint.RowPitch,
                    src + std::size_t(y) * tex.width * 4, std::size_t(tex.width) * 4);
    }
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&cmd))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return nullptr;
    }
    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = texture.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = upload.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint = footprint;
    cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &barrier);
    if (FAILED(cmd->Close())) return nullptr;
    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1))) return nullptr;
    if (fence->GetCompletedValue() < 1) {
        HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (event) {
            fence->SetEventOnCompletion(1, event);
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }
    }
    return texture;
}

}  // namespace

bool NativeCloudRenderer::initialise(ID3D12Device* device, ID3D12CommandQueue* queue,
                                     std::string& error) {
    device_ = device;
    queue_ = queue;

    // Constant buffer: a ring of kMaxLayers 256-byte-aligned CloudConstants (one draw per layer).
    constant_stride_ = (sizeof(CloudConstants) + 255u) & ~255u;
    {
        const auto heap = upload_heap();
        const auto desc = buffer_desc(std::size_t(constant_stride_) * kMaxLayers);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&constant_buffer_))) ||
            FAILED(constant_buffer_->Map(0, nullptr, &mapped_constants_))) {
            error = "D3D12 could not allocate the cloud constant buffer.";
            return false;
        }
        constant_address_ = constant_buffer_->GetGPUVirtualAddress();
    }

    // Dynamic vertex buffer: kMaxLayers * 4 verts, written per frame.
    {
        const auto heap = upload_heap();
        const auto desc = buffer_desc(std::size_t(sizeof(CloudVertex)) * 4 * kMaxLayers);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&vertex_buffer_))) ||
            FAILED(vertex_buffer_->Map(0, nullptr, &mapped_vertices_))) {
            error = "D3D12 could not allocate the cloud vertex buffer.";
            return false;
        }
    }

    // Static index buffer {0,1,2,1,3,2} (CloudRuntime::Indices()).
    {
        const std::uint16_t indices[6] = {0, 1, 2, 1, 3, 2};
        const auto heap = upload_heap();
        const auto desc = buffer_desc(sizeof(indices));
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&index_buffer_)))) {
            error = "D3D12 could not allocate the cloud index buffer.";
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(index_buffer_->Map(0, nullptr, &mapped))) {
            error = "D3D12 could not map the cloud index buffer.";
            return false;
        }
        std::memcpy(mapped, indices, sizeof(indices));
        index_buffer_->Unmap(0, nullptr);
        index_view_.BufferLocation = index_buffer_->GetGPUVirtualAddress();
        index_view_.SizeInBytes = sizeof(indices);
        index_view_.Format = DXGI_FORMAT_R16_UINT;
    }

    // Density SRV heap (one descriptor per layer).
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = kMaxLayers;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap_)))) {
            error = "D3D12 could not create the cloud descriptor heap.";
            return false;
        }
        srv_descriptor_size_ =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Shaders.
    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
    const auto compile = [&](const char* entry, const char* target,
                             Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
        return D3DCompile(kCloudShaderSource, sizeof(kCloudShaderSource) - 1, "native_cloud.hlsl",
                          nullptr, nullptr, entry, target, 0, 0, &blob, &errors);
    };
    if (FAILED(compile("vs_main", "vs_5_0", vs)) || FAILED(compile("ps_main", "ps_5_0", ps))) {
        error = "The native cloud shaders could not be compiled.";
        return false;
    }

    // Root signature: CBV b0 + SRV table t0 + static wrap sampler s0.
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = params;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> root_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
                                           &errors)) ||
        FAILED(device->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                           root_blob->GetBufferSize(),
                                           IID_PPV_ARGS(&root_signature_)))) {
        error = "The native cloud root signature could not be created.";
        return false;
    }

    // PSO: alpha blend, no depth test/write, cull none.
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {layout, 2};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = 0xFFFFFFFFu;
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = FALSE;
    pso.RasterizerState = raster;
    D3D12_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState = blend;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_state_)))) {
        error = "The native cloud pipeline could not be created.";
        return false;
    }
    return true;
}

void NativeCloudRenderer::ensure_scene(ID3D12Device* device, ID3D12CommandQueue* queue,
                                       const NativeScene& scene) {
    if (bound_scene_ == &scene) return;
    bound_scene_ = &scene;
    layer_ready_.fill(false);
    for (auto& t : density_textures_) t.Reset();
    layer_texture_name_.fill(std::string{});

    D3D12_CPU_DESCRIPTOR_HANDLE base = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    const std::size_t count = std::min<std::size_t>(scene.clouds.size(), kMaxLayers);
    for (std::size_t i = 0; i < count; ++i) {
        const NativeCloudLayer& layer = scene.clouds[i];
        if (layer.density_map.empty()) continue;
        NativeTexture tex;
        std::string err;
        if (!load_dds_rgba8(layer.density_map, tex, err)) continue;
        auto resource = upload_texture_rgba8(device, queue, tex);
        if (!resource) continue;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = base;
        handle.ptr += std::size_t(i) * srv_descriptor_size_;
        device->CreateShaderResourceView(resource.Get(), &srv, handle);
        density_textures_[i] = resource;
        layer_texture_name_[i] = layer.density_map;
        layer_ready_[i] = true;
    }
}

void NativeCloudRenderer::render(ID3D12GraphicsCommandList* command_list, const NativeScene& scene,
                                 const std::array<float, 16>& view_projection,
                                 const std::array<float, 3>& eye,
                                 const std::array<float, 3>& forward, std::uint32_t width,
                                 std::uint32_t height, double elapsed_seconds) {
    if (!pipeline_state_ || width == 0 || height == 0 || scene.clouds.empty()) return;
    ensure_scene(device_, queue_, scene);

    // Active layers: bound density + transparency > 0. Sort high->low (back-to-front), matching
    // CloudRuntime::BuildActiveOrder (height descending).
    std::vector<int> order;
    const std::size_t count = std::min<std::size_t>(scene.clouds.size(), kMaxLayers);
    for (std::size_t i = 0; i < count; ++i) {
        if (layer_ready_[i] && scene.clouds[i].transparency > 0.0f) order.push_back(int(i));
    }
    if (order.empty()) return;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return scene.clouds[a].height > scene.clouds[b].height;
    });

    const auto sun_toward = normalise3({-scene.sun_direction[0], -scene.sun_direction[1],
                                        -scene.sun_direction[2]});

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width),
                                  static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetPipelineState(pipeline_state_.Get());
    command_list->SetGraphicsRootSignature(root_signature_.Get());
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetIndexBuffer(&index_view_);

    for (std::size_t draw = 0; draw < order.size(); ++draw) {
        const int li = order[draw];
        const NativeCloudLayer& layer = scene.clouds[li];

        // Quad (render space): centred at world origin, at Y = height, spanning ±size in X/Z.
        CloudVertex quad[4] = {
            {-layer.size_x, layer.height, -layer.size_y, 0.0f, 0.0f},
            { layer.size_x, layer.height, -layer.size_y, 1.0f, 0.0f},
            {-layer.size_x, layer.height,  layer.size_y, 0.0f, 1.0f},
            { layer.size_x, layer.height,  layer.size_y, 1.0f, 1.0f},
        };
        auto* vb = static_cast<std::uint8_t*>(mapped_vertices_) +
                   std::size_t(draw) * 4 * sizeof(CloudVertex);
        std::memcpy(vb, quad, sizeof(quad));

        CloudConstants c{};
        std::memcpy(c.view_projection, view_projection.data(), sizeof(c.view_projection));
        c.viewer_position[0] = eye[0];
        c.viewer_position[1] = eye[1];
        c.viewer_position[2] = eye[2];
        c.viewer_position[3] = 1.0f;
        c.viewer_direction[0] = forward[0];
        c.viewer_direction[1] = forward[1];
        c.viewer_direction[2] = forward[2];
        // AB: light_position = camera - sun_toward * 2000 (sun_toward = toward the sun).
        c.light_position[0] = eye[0] - sun_toward[0] * 2000.0f;
        c.light_position[1] = eye[1] - sun_toward[1] * 2000.0f;
        c.light_position[2] = eye[2] - sun_toward[2] * 2000.0f;
        c.light_position[3] = 1.0f;
        c.light_colour[0] = scene.sun_color[0];
        c.light_colour[1] = scene.sun_color[1];
        c.light_colour[2] = scene.sun_color[2];
        c.light_colour[3] = 1.0f;
        c.layer_params[0] = layer.transparency;
        c.layer_params[1] = layer.ambient;
        c.layer_params[2] = layer.brightness;
        c.layer_params[3] = shader_normal_strength(layer.normal_strength);
        // Scroll offset = frac(velocity * kVelocityScale * elapsed), stateless accumulation.
        const float scroll_x = layer.velocity_x * 0.001f * static_cast<float>(elapsed_seconds);
        const float scroll_y = layer.velocity_y * 0.001f * static_cast<float>(elapsed_seconds);
        c.uv_scale_offset[0] = layer.texture_scale_x;
        c.uv_scale_offset[1] = layer.texture_scale_y;
        c.uv_scale_offset[2] = scroll_x - std::floor(scroll_x);
        c.uv_scale_offset[3] = scroll_y - std::floor(scroll_y);
        c.cloud_globals[0] = scene.cloud_global_brightness;
        c.cloud_globals[2] = scene.cloud_alpha_ref;
        auto* cb = static_cast<std::uint8_t*>(mapped_constants_) +
                   std::size_t(draw) * constant_stride_;
        std::memcpy(cb, &c, sizeof(c));

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = vertex_buffer_->GetGPUVirtualAddress() +
                             std::size_t(draw) * 4 * sizeof(CloudVertex);
        vbv.SizeInBytes = 4 * sizeof(CloudVertex);
        vbv.StrideInBytes = sizeof(CloudVertex);
        command_list->IASetVertexBuffers(0, 1, &vbv);
        command_list->SetGraphicsRootConstantBufferView(
            0, constant_address_ + std::size_t(draw) * constant_stride_);
        D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += std::size_t(li) * srv_descriptor_size_;
        command_list->SetGraphicsRootDescriptorTable(1, srv);
        command_list->DrawIndexedInstanced(6, 1, 0, 0, 0);
    }
}

}  // namespace f2
