#include "f2/native_tonemap_renderer.h"

#include <d3dcompiler.h>

// -----------------------------------------------------------------------------
// Fable II HDR -> LDR compositor (native D3D12).
//
// PORT SOURCE (do-not-re-derive): ghidra_out/rendering_pipeline.txt §D.3 — the retail final
// COMPOSITOR pixel shader samples the resolved HDR scene (RGBA16F) + bloom + an auto-exposure
// value and does "result = mul_sat(exposure * sceneColor) + bloom". This baseline implements the
// exposure + saturate half (bloom is added later); a full-screen triangle from SV_VertexID, no
// vertex/index buffer. Default exposure 1.0 == the old UNORM clamp, so it is a no-op on the already
// bounded sky/UI-free World frames until exposure is tuned.
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

constexpr char kTonemapShaderSource[] = R"(
Texture2D scene : register(t0);
SamplerState smp : register(s0);
cbuffer TonemapCB : register(b0) { float4 params; }  // x = exposure

struct VSOUT { float4 position : SV_Position; float2 uv : TEXCOORD0; };

VSOUT vs_main(uint vertex_id : SV_VertexID) {
    VSOUT o;
    float2 uv = float2((vertex_id << 1) & 2u, vertex_id & 2u);
    o.uv = uv;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VSOUT input) : SV_Target {
    float3 hdr = scene.Sample(smp, input.uv).rgb;
    float3 ldr = saturate(params.x * hdr);   // retail: mul_sat(exposure * scene)
    return float4(ldr, 1.0);
}
)";

}  // namespace

bool NativeTonemapRenderer::initialise(ID3D12Device* device, DXGI_FORMAT output_format,
                                       std::string& error) {
    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
    const auto compile = [&](const char* entry, const char* target,
                             Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
        return D3DCompile(kTonemapShaderSource, sizeof(kTonemapShaderSource) - 1,
                          "native_tonemap.hlsl", nullptr, nullptr, entry, target, 0, 0, &blob,
                          &errors);
    };
    if (FAILED(compile("vs_main", "vs_5_0", vs)) || FAILED(compile("ps_main", "ps_5_0", ps))) {
        error = "The native tonemap shaders could not be compiled.";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1;
    srv_range.BaseShaderRegister = 0;  // t0
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srv_range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;  // b0
    params[1].Constants.Num32BitValues = 4;  // float4 params
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;  // s0
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
        error = "The native tonemap root signature could not be created.";
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {nullptr, 0};  // full-screen triangle from SV_VertexID
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = output_format;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = 0xFFFFFFFFu;
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = FALSE;
    pso.RasterizerState = raster;
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState = blend;  // opaque write (the compositor replaces the back buffer)
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.StencilEnable = FALSE;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_state_)))) {
        error = "The native tonemap pipeline could not be created.";
        return false;
    }
    return true;
}

void NativeTonemapRenderer::render(ID3D12GraphicsCommandList* command_list,
                                   D3D12_GPU_DESCRIPTOR_HANDLE hdr_srv, std::uint32_t width,
                                   std::uint32_t height, float exposure) {
    if (!pipeline_state_ || width == 0 || height == 0) return;

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width),
                                  static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
    command_list->SetPipelineState(pipeline_state_.Get());
    command_list->SetGraphicsRootSignature(root_signature_.Get());
    command_list->SetGraphicsRootDescriptorTable(0, hdr_srv);
    const float params[4] = {exposure, 0.0f, 0.0f, 0.0f};
    command_list->SetGraphicsRoot32BitConstants(1, 4, params, 0);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 0, nullptr);
    command_list->IASetIndexBuffer(nullptr);
    command_list->DrawInstanced(3, 1, 0, 0);
}

}  // namespace f2
