#include "f2/native_sky_billboard_renderer.h"

#include "f2/native_texture.h"
#include "f2/sky_billboard.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

// -----------------------------------------------------------------------------
// Fable II celestial billboards (native D3D12 port) — the night moon + its glare halo.
// Port source: Fable2AssetBrowser SkyboxRenderer.cpp draw_billboard @1558-1734 + the moon block
// @1701-1734 + SkyXex::k*Distance/SizeScale. Colour is the retail HDR-scaled + Reinhard-tonemapped
// element colour. Drawn after the clouds, behind the world (camera-facing screen-space quads).
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

// SkyXexDecomp.h billboard constants.
constexpr float kMoonBillboardDistance = 4000.0f;
constexpr float kMoonSizeScale = 300.0f;
constexpr float kMoonGlareDistance = 1000.0f;
constexpr float kMoonGlareSizeScale = 300.0f;
constexpr float kMoonPhaseUStep = 0.125f;  // 8 phases across the strip
constexpr float kGate = 0.000099999997f;

struct BillboardConstants {
    float colour[4];  // rgb tint, a alpha (matches kSkyElementPixelShader element_colour)
};

constexpr char kBillboardShaderSource[] = R"(
cbuffer BillboardCB : register(b0) { float4 element_colour; }
Texture2D element_tex : register(t0);
SamplerState element_sampler : register(s0);
struct VSIN  { float2 position : POSITION; float2 uv : TEXCOORD0; };
struct VSOUT { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VSOUT vs_main(VSIN input) {
    VSOUT o;
    o.position = float4(input.position, 0.9985, 1.0);  // no depth test; z is a valid NDC filler
    o.uv = input.uv;
    return o;
}
float4 ps_main(VSOUT input) : SV_Target {
    float4 texel = element_tex.Sample(element_sampler, input.uv);
    return float4(texel.rgb * element_colour.rgb, texel.a * element_colour.a);
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

// Upload an RGBA8 texture to a DEFAULT-heap Texture2D (PIXEL_SHADER_RESOURCE). Same pattern as the
// cloud/sky renderers' synchronous one-time upload.
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
    for (UINT y = 0; y < tex.height; ++y) {
        std::memcpy(dst + std::size_t(y) * footprint.Footprint.RowPitch,
                    tex.rgba8.data() + std::size_t(y) * tex.width * 4,
                    std::size_t(tex.width) * 4);
    }
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&cmd))) ||
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

void tonemap(float c[3]) {
    for (int i = 0; i < 3; ++i) c[i] = c[i] / (1.0f + c[i]);  // Reinhard host stand-in
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> make_pso(ID3D12Device* device,
                                                     ID3D12RootSignature* root, ID3DBlob* vs,
                                                     ID3DBlob* ps, bool additive) {
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root;
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
    rt.DestBlend = additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
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
    Microsoft::WRL::ComPtr<ID3D12PipelineState> state;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&state)))) return nullptr;
    return state;
}

}  // namespace

bool NativeSkyBillboardRenderer::initialise(ID3D12Device* device, ID3D12CommandQueue* queue,
                                            std::string& error) {
    device_ = device;
    queue_ = queue;

    constant_stride_ = (sizeof(BillboardConstants) + 255u) & ~255u;
    const auto heap = upload_heap();
    {
        const auto desc = buffer_desc(std::size_t(constant_stride_) * kMaxBillboards);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&constant_buffer_))) ||
            FAILED(constant_buffer_->Map(0, nullptr, &mapped_constants_))) {
            error = "D3D12 could not allocate the billboard constant buffer.";
            return false;
        }
        constant_address_ = constant_buffer_->GetGPUVirtualAddress();
    }
    {
        const auto desc = buffer_desc(std::size_t(sizeof(BillboardVertex)) * 6 * kMaxBillboards);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&vertex_buffer_))) ||
            FAILED(vertex_buffer_->Map(0, nullptr, &mapped_vertices_))) {
            error = "D3D12 could not allocate the billboard vertex buffer.";
            return false;
        }
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = kMaxBillboards;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap_)))) {
            error = "D3D12 could not create the billboard descriptor heap.";
            return false;
        }
        srv_descriptor_size_ =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
    const auto compile = [&](const char* entry, const char* target,
                             Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
        return D3DCompile(kBillboardShaderSource, sizeof(kBillboardShaderSource) - 1,
                          "native_billboard.hlsl", nullptr, nullptr, entry, target, 0, 0, &blob,
                          &errors);
    };
    if (FAILED(compile("vs_main", "vs_5_0", vs)) || FAILED(compile("ps_main", "ps_5_0", ps))) {
        error = "The native billboard shaders could not be compiled.";
        return false;
    }

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
        error = "The native billboard root signature could not be created.";
        return false;
    }
    alpha_pso_ = make_pso(device, root_signature_.Get(), vs.Get(), ps.Get(), false);
    additive_pso_ = make_pso(device, root_signature_.Get(), vs.Get(), ps.Get(), true);
    if (!alpha_pso_ || !additive_pso_) {
        error = "The native billboard pipelines could not be created.";
        return false;
    }
    return true;
}

void NativeSkyBillboardRenderer::ensure_scene(ID3D12Device* device, ID3D12CommandQueue* queue,
                                              const NativeScene& scene) {
    if (bound_scene_ == &scene) return;
    bound_scene_ = &scene;
    moon_ready_ = glare_ready_ = false;
    moon_texture_.Reset();
    glare_texture_.Reset();
    if (!scene.has_moon) return;

    D3D12_CPU_DESCRIPTOR_HANDLE base = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    auto load = [&](const std::string& path, std::uint32_t slot,
                    Microsoft::WRL::ComPtr<ID3D12Resource>& out) -> bool {
        if (path.empty()) return false;
        NativeTexture tex;
        std::string err;
        if (!load_dds_rgba8(path, tex, err)) return false;
        auto resource = upload_texture_rgba8(device, queue, tex);
        if (!resource) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = base;
        handle.ptr += std::size_t(slot) * srv_descriptor_size_;
        device->CreateShaderResourceView(resource.Get(), &srv, handle);
        out = resource;
        return true;
    };
    moon_ready_ = load(scene.moon.texture, 0, moon_texture_);
    glare_ready_ = load(scene.moon.glare_texture, 1, glare_texture_);
}

void NativeSkyBillboardRenderer::render(ID3D12GraphicsCommandList* command_list,
                                        const NativeScene& scene, const SkyCamera& camera,
                                        std::uint32_t width, std::uint32_t height) {
    if (!alpha_pso_ || width == 0 || height == 0 || !scene.has_moon) return;
    ensure_scene(device_, queue_, scene);
    if (!moon_ready_) return;
    const NativeMoon& moon = scene.moon;
    if (moon.intensity < kGate) return;

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width),
                                  static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetGraphicsRootSignature(root_signature_.Get());
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    std::uint32_t draw = 0;
    auto emit = [&](std::uint32_t srv_slot, ID3D12PipelineState* pso,
                    const std::array<BillboardVertex, 6>& verts, const float colour[4]) {
        auto* vb = static_cast<std::uint8_t*>(mapped_vertices_) +
                   std::size_t(draw) * 6 * sizeof(BillboardVertex);
        std::memcpy(vb, verts.data(), 6 * sizeof(BillboardVertex));
        BillboardConstants c{};
        std::memcpy(c.colour, colour, sizeof(c.colour));
        auto* cb = static_cast<std::uint8_t*>(mapped_constants_) +
                   std::size_t(draw) * constant_stride_;
        std::memcpy(cb, &c, sizeof(c));
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = vertex_buffer_->GetGPUVirtualAddress() +
                             std::size_t(draw) * 6 * sizeof(BillboardVertex);
        vbv.SizeInBytes = 6 * sizeof(BillboardVertex);
        vbv.StrideInBytes = sizeof(BillboardVertex);
        D3D12_GPU_DESCRIPTOR_HANDLE srv = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += std::size_t(srv_slot) * srv_descriptor_size_;
        command_list->SetPipelineState(pso);
        command_list->IASetVertexBuffers(0, 1, &vbv);
        command_list->SetGraphicsRootConstantBufferView(
            0, constant_address_ + std::size_t(draw) * constant_stride_);
        command_list->SetGraphicsRootDescriptorTable(1, srv);
        command_list->DrawInstanced(6, 1, 0, 0);
        ++draw;
    };

    const float exposure = moon.exposure;
    // Moon disc (alpha-blended): colour = tonemap(0.5*I*E, 0.5*I*E, T*E), alpha = T.
    std::array<BillboardVertex, 6> verts{};
    const float u0 = static_cast<float>(std::clamp(moon.phase, 0, 7)) * kMoonPhaseUStep;
    const float u1 = u0 + kMoonPhaseUStep;
    if (build_billboard(camera, moon.direction, kMoonBillboardDistance,
                        moon.size * kMoonSizeScale, moon.size * kMoonSizeScale, u0, u1, verts)) {
        float colour[4] = {0.5f * moon.intensity * exposure, 0.5f * moon.intensity * exposure,
                           moon.transparency * exposure, moon.transparency};
        tonemap(colour);
        emit(0, alpha_pso_.Get(), verts, colour);
    }
    // Moon glare (additive): colour = tonemap(GI*E)^3, alpha = 1.
    if (glare_ready_ && moon.glare_intensity >= kGate &&
        build_billboard(camera, moon.direction, kMoonGlareDistance,
                        moon.glare_size * kMoonGlareSizeScale, moon.glare_size * kMoonGlareSizeScale,
                        0.0f, 1.0f, verts)) {
        const float scale = moon.glare_intensity * exposure;
        float colour[4] = {scale, scale, scale, 1.0f};
        tonemap(colour);
        emit(1, additive_pso_.Get(), verts, colour);
    }
}

}  // namespace f2
