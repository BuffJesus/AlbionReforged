#include "f2/native_tonemap_renderer.h"

#include "f2/native_scene_color.h"

#include <d3dcompiler.h>

#include <cstddef>

// -----------------------------------------------------------------------------
// Fable II HDR -> LDR compositor with bloom (native D3D12).
//
// PORT SOURCE (do-not-re-derive): ghidra_out/rendering_pipeline.txt §D.3 — the retail final
// COMPOSITOR samples the resolved HDR scene (RGBA16F) + a bloom/glow buffer + an exposure value:
// "result = mul_sat(exposure * sceneColor) + bloom". This implements the exposure + saturate +
// bloom add; the bloom buffer is a thresholded, separably-blurred half-res copy of the HDR scene.
// All passes are full-screen triangles from SV_VertexID (no vertex/index buffers). Default exposure
// 1.0 + bloom_intensity 0.0 is bit-for-bit the old direct-to-R8G8B8A8_UNORM clamp.
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

constexpr char kCompositorShaderSource[] = R"(
Texture2D src   : register(t0);   // scene (bright/blur source; composite scene)
Texture2D bloom : register(t1);   // composite bloom
SamplerState smp : register(s0);
cbuffer CompositorCB : register(b0) { float4 params; }
// bright:    params.x = threshold
// blur:      params.xy = texel step (1/w,0) or (0,1/h)
// composite: params.x = exposure, params.y = bloom intensity

struct VSOUT { float4 position : SV_Position; float2 uv : TEXCOORD0; };

VSOUT vs_main(uint vertex_id : SV_VertexID) {
    VSOUT o;
    float2 uv = float2((vertex_id << 1) & 2u, vertex_id & 2u);
    o.uv = uv;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Threshold the HDR scene: keep only energy above the bloom threshold.
float4 ps_bright(VSOUT input) : SV_Target {
    float3 c = src.Sample(smp, input.uv).rgb;
    return float4(max(c - params.x, 0.0), 1.0);
}

// Separable 9-tap Gaussian along params.xy.
float4 ps_blur(VSOUT input) : SV_Target {
    const float w[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
    float2 step = params.xy;
    float3 c = src.Sample(smp, input.uv).rgb * w[0];
    [unroll] for (int i = 1; i < 5; ++i) {
        c += src.Sample(smp, input.uv + step * (float)i).rgb * w[i];
        c += src.Sample(smp, input.uv - step * (float)i).rgb * w[i];
    }
    return float4(c, 1.0);
}

// Retail composite: mul_sat(exposure * scene) + intensity * bloom (the UNORM target clamps).
float4 ps_composite(VSOUT input) : SV_Target {
    float3 scene = src.Sample(smp, input.uv).rgb;
    float3 glow  = bloom.Sample(smp, input.uv).rgb;
    float3 ldr = saturate(params.x * scene) + params.y * glow;
    return float4(ldr, 1.0);
}
)";

Microsoft::WRL::ComPtr<ID3DBlob> compile_entry(const char* entry, const char* target,
                                               Microsoft::WRL::ComPtr<ID3DBlob>& errors) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    D3DCompile(kCompositorShaderSource, sizeof(kCompositorShaderSource) - 1, "native_tonemap.hlsl",
               nullptr, nullptr, entry, target, 0, 0, &blob, &errors);
    return blob;
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC base_pso(ID3D12RootSignature* root, ID3DBlob* vs, ID3DBlob* ps,
                                            DXGI_FORMAT rtv_format) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {nullptr, 0};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtv_format;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = 0xFFFFFFFFu;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    return pso;
}

}  // namespace

bool NativeTonemapRenderer::initialise(ID3D12Device* device, DXGI_FORMAT output_format,
                                       std::string& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    auto vs = compile_entry("vs_main", "vs_5_0", errors);
    auto bright = compile_entry("ps_bright", "ps_5_0", errors);
    auto blur = compile_entry("ps_blur", "ps_5_0", errors);
    auto composite = compile_entry("ps_composite", "ps_5_0", errors);
    if (!vs || !bright || !blur || !composite) {
        error = "The native tonemap shaders could not be compiled.";
        return false;
    }

    // Two independent single-SRV tables (t0 = source/scene, t1 = bloom) + root constants (b0).
    D3D12_DESCRIPTOR_RANGE range0{};
    range0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range0.NumDescriptors = 1;
    range0.BaseShaderRegister = 0;  // t0
    range0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE range1 = range0;
    range1.BaseShaderRegister = 1;  // t1

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;  // b0
    params[2].Constants.Num32BitValues = 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 3;
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
        error = "The native tonemap root signature could not be created.";
        return false;
    }

    auto bright_pso = base_pso(root_signature_.Get(), vs.Get(), bright.Get(), kSceneColorFormat);
    auto blur_pso = base_pso(root_signature_.Get(), vs.Get(), blur.Get(), kSceneColorFormat);
    auto composite_pso = base_pso(root_signature_.Get(), vs.Get(), composite.Get(), output_format);
    if (FAILED(device->CreateGraphicsPipelineState(&bright_pso, IID_PPV_ARGS(&bright_pso_))) ||
        FAILED(device->CreateGraphicsPipelineState(&blur_pso, IID_PPV_ARGS(&blur_pso_))) ||
        FAILED(device->CreateGraphicsPipelineState(&composite_pso, IID_PPV_ARGS(&composite_pso_)))) {
        error = "The native tonemap pipelines could not be created.";
        return false;
    }

    // Fixed heaps: 4 shader-visible SRVs (scene + bright + blur_h + blur_v) and 3 bloom RTVs.
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.NumDescriptors = 4;
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.NumDescriptors = 3;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap_))) ||
        FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_)))) {
        error = "The native tonemap descriptor heaps could not be created.";
        return false;
    }
    srv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

void NativeTonemapRenderer::ensure_targets(ID3D12Device* device, ID3D12Resource* scene_target,
                                           std::uint32_t width, std::uint32_t height) {
    if (!srv_heap_ || !scene_target || width == 0 || height == 0) return;
    const std::uint32_t bw = width > 1 ? width / 2 : 1;
    const std::uint32_t bh = height > 1 ? height / 2 : 1;

    auto srv_cpu = [&](UINT i) {
        auto h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<std::size_t>(i) * srv_stride_;
        return h;
    };
    auto srv_gpu = [&](UINT i) {
        auto h = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<std::size_t>(i) * srv_stride_;
        return h;
    };
    auto rtv_cpu = [&](UINT i) {
        auto h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<std::size_t>(i) * rtv_stride_;
        return h;
    };

    // SRV [0] over the app's HDR scene target (full-res).
    D3D12_SHADER_RESOURCE_VIEW_DESC scene_srv{};
    scene_srv.Format = kSceneColorFormat;
    scene_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    scene_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    scene_srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(scene_target, &scene_srv, srv_cpu(0));
    scene_srv_ = srv_gpu(0);

    // Half-res bloom targets at SRV [1..3] / RTV [0..2].
    struct Slot { Target* t; UINT srv_index; UINT rtv_index; };
    const Slot slots[3] = {{&bright_, 1, 0}, {&blur_h_, 2, 1}, {&blur_v_, 3, 2}};
    for (const auto& s : slots) {
        s.t->resource.Reset();
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = bw;
        desc.Height = bh;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kSceneColorFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kSceneColorFormat;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                   IID_PPV_ARGS(&s.t->resource)))) {
            s.t->resource.Reset();
            return;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = kSceneColorFormat;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(s.t->resource.Get(), &rtv, rtv_cpu(s.rtv_index));
        s.t->rtv = rtv_cpu(s.rtv_index);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = kSceneColorFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(s.t->resource.Get(), &srv, srv_cpu(s.srv_index));
        s.t->srv = srv_gpu(s.srv_index);
        s.t->srv_index = s.srv_index;
    }
    bloom_width_ = bw;
    bloom_height_ = bh;
}

void NativeTonemapRenderer::barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r,
                                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &b);
}

void NativeTonemapRenderer::render(ID3D12GraphicsCommandList* cmd,
                                   D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv, std::uint32_t width,
                                   std::uint32_t height, float exposure, float bloom_threshold,
                                   float bloom_intensity) {
    if (!composite_pso_ || !bright_.resource || width == 0 || height == 0) return;

    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(root_signature_.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);

    const bool bloom_on = bloom_intensity > 0.0f;
    if (bloom_on) {
        const D3D12_VIEWPORT bloom_vp{0.0f, 0.0f, static_cast<float>(bloom_width_),
                                      static_cast<float>(bloom_height_), 0.0f, 1.0f};
        const D3D12_RECT bloom_sc{0, 0, static_cast<LONG>(bloom_width_),
                                  static_cast<LONG>(bloom_height_)};
        cmd->RSSetViewports(1, &bloom_vp);
        cmd->RSSetScissorRects(1, &bloom_sc);

        // Bright-pass: HDR scene -> bright_ (threshold).
        cmd->SetPipelineState(bright_pso_.Get());
        cmd->OMSetRenderTargets(1, &bright_.rtv, FALSE, nullptr);
        cmd->SetGraphicsRootDescriptorTable(0, scene_srv_);
        cmd->SetGraphicsRootDescriptorTable(1, scene_srv_);  // unused by ps_bright
        const float bright_c[4] = {bloom_threshold, 0.0f, 0.0f, 0.0f};
        cmd->SetGraphicsRoot32BitConstants(2, 4, bright_c, 0);
        cmd->DrawInstanced(3, 1, 0, 0);
        barrier(cmd, bright_.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Blur horizontal: bright_ -> blur_h_.
        cmd->SetPipelineState(blur_pso_.Get());
        cmd->OMSetRenderTargets(1, &blur_h_.rtv, FALSE, nullptr);
        cmd->SetGraphicsRootDescriptorTable(0, bright_.srv);
        const float blur_h_c[4] = {1.0f / static_cast<float>(bloom_width_), 0.0f, 0.0f, 0.0f};
        cmd->SetGraphicsRoot32BitConstants(2, 4, blur_h_c, 0);
        cmd->DrawInstanced(3, 1, 0, 0);
        barrier(cmd, blur_h_.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Blur vertical: blur_h_ -> blur_v_.
        cmd->OMSetRenderTargets(1, &blur_v_.rtv, FALSE, nullptr);
        cmd->SetGraphicsRootDescriptorTable(0, blur_h_.srv);
        const float blur_v_c[4] = {0.0f, 1.0f / static_cast<float>(bloom_height_), 0.0f, 0.0f};
        cmd->SetGraphicsRoot32BitConstants(2, 4, blur_v_c, 0);
        cmd->DrawInstanced(3, 1, 0, 0);
        barrier(cmd, blur_v_.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // Composite: saturate(exposure * scene) + intensity * bloom -> back buffer (full-res).
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                            0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->SetPipelineState(composite_pso_.Get());
    cmd->OMSetRenderTargets(1, &back_buffer_rtv, FALSE, nullptr);
    cmd->SetGraphicsRootDescriptorTable(0, scene_srv_);
    cmd->SetGraphicsRootDescriptorTable(1, bloom_on ? blur_v_.srv : scene_srv_);
    const float comp_c[4] = {exposure, bloom_on ? bloom_intensity : 0.0f, 0.0f, 0.0f};
    cmd->SetGraphicsRoot32BitConstants(2, 4, comp_c, 0);
    cmd->DrawInstanced(3, 1, 0, 0);

    if (bloom_on) {
        // Restore bloom targets to RENDER_TARGET for the next frame.
        barrier(cmd, bright_.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(cmd, blur_h_.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(cmd, blur_v_.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
}

}  // namespace f2
