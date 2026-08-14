#include "f2/native_sky_stars_renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cstring>

// -----------------------------------------------------------------------------
// Fable II procedural night stars (native D3D12 port).
//
// PORT SOURCE (do-not-re-derive): Fable2AssetBrowser SkyDomeXex kStarsVertexShaderHlsl /
// kStarsPixelShaderHlsl — a faithful RE of the retail Xenos star ucode (packets 3..18). 512
// additive point-sprite quads generated from SV_VertexID: each star's direction is a hash of its
// index on the upper hemisphere, with a per-star twinkle (frac of a time-scaled phase). No vertex
// buffer, no texture. The retail shader projects in ENGINE space (x,z,y swap); we keep the math
// byte-identical and remap the direction's always-positive component (r2.z) to render-space UP (Y)
// so it lines up with our render-space SkyCamera — algebraically the same projection.
// -----------------------------------------------------------------------------

namespace f2 {
namespace {

constexpr int kStarCount = 512;
constexpr float kStarPointSize = 1.5f;  // SkyDomeXex.h

struct StarConstants {
    float camera_right[4]{};    // xyz + tan(fov_x/2) in w
    float camera_up[4]{};       // xyz + tan(fov_y/2) in w
    float camera_forward[4]{};  // xyz
    float star_params[4]{};     // x=time y=brightness z=half_pt_x(NDC) w=half_pt_y(NDC)
};

constexpr char kStarsShaderSource[] = R"(
cbuffer StarCB : register(b0) {
    float4 camera_right;    // w = tan(fov_x/2)
    float4 camera_up;       // w = tan(fov_y/2)
    float4 camera_forward;
    float4 star_params;     // x=time y=brightness z=half_pt_x w=half_pt_y
}
struct VSOUT { float4 position : SV_Position; float colour : TEXCOORD0; };

VSOUT vs_main(uint vertex_id : SV_VertexID) {
    const uint star = vertex_id / 6u;
    const uint corner = vertex_id % 6u;
    const float index = (float)star;
    float4 r0 = index * float4(732.051, 236.068, 645.751, 141.421);
    float4 r2 = frac(r0);
    float phase_rate = 2.5 * star_params.x;
    float2 dir_xy = r2.xy * 2.0 - 1.0;
    float4 r3 = r2.xywz + 1.0;
    float r0z = r2.z * r2.z;
    r3 = phase_rate * r3;
    float hash_sq = r2.w * r2.w;
    float len_sq = dot(dir_xy, dir_xy) + r0z;
    r3 = frac(r3);
    float inv_len = rsqrt(abs(len_sq));
    float twinkle = max(max(r3.x, r3.y), max(r3.z, r3.w));
    // Retail engine direction (dir_xy*inv_len, r2.z*inv_len) remapped to render space (r2.z -> Y up).
    float3 direction = float3(dir_xy.x * inv_len, r2.z * inv_len, dir_xy.y * inv_len);
    float value = hash_sq * twinkle;
    float3 world = direction * 2500.0;
    float depth = dot(world, camera_forward.xyz);
    VSOUT o;
    if (depth <= 0.01) { o.position = float4(0.0, 0.0, -10.0, 1.0); o.colour = 0.0; return o; }
    float2 ndc = float2(dot(world, camera_right.xyz) / (depth * camera_right.w),
                        dot(world, camera_up.xyz) / (depth * camera_up.w));
    const float2 corner_offsets[6] = {
        float2(-1, 1), float2(1, 1), float2(-1, -1),
        float2(-1, -1), float2(1, 1), float2(1, -1) };
    ndc += corner_offsets[corner] * star_params.zw;
    o.position = float4(ndc, 0.9985, 1.0);
    o.colour = value * star_params.y;
    return o;
}
float4 ps_main(VSOUT input) : SV_Target { return float4(input.colour.xxx, 0.0); }
)";

D3D12_HEAP_PROPERTIES upload_heap() {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = D3D12_HEAP_TYPE_UPLOAD;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

}  // namespace

bool NativeSkyStarsRenderer::initialise(ID3D12Device* device, std::string& error) {
    const auto cb_size = (sizeof(StarConstants) + 255u) & ~255u;
    D3D12_HEAP_PROPERTIES heap = upload_heap();
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = cb_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&constant_buffer_))) ||
        FAILED(constant_buffer_->Map(0, nullptr, &mapped_constants_))) {
        error = "D3D12 could not allocate the stars constant buffer.";
        return false;
    }
    constant_address_ = constant_buffer_->GetGPUVirtualAddress();

    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
    const auto compile = [&](const char* entry, const char* target,
                             Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
        return D3DCompile(kStarsShaderSource, sizeof(kStarsShaderSource) - 1, "native_stars.hlsl",
                          nullptr, nullptr, entry, target, 0, 0, &blob, &errors);
    };
    if (FAILED(compile("vs_main", "vs_5_0", vs)) || FAILED(compile("ps_main", "ps_5_0", ps))) {
        error = "The native stars shaders could not be compiled.";
        return false;
    }

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &param;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3DBlob> root_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
                                           &errors)) ||
        FAILED(device->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                           root_blob->GetBufferSize(),
                                           IID_PPV_ARGS(&root_signature_)))) {
        error = "The native stars root signature could not be created.";
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {nullptr, 0};  // procedural from SV_VertexID
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
    rt.BlendEnable = TRUE;  // additive ONE/ONE (retail bs_fx_add; PS alpha 0)
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ONE;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState = blend;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_state_)))) {
        error = "The native stars pipeline could not be created.";
        return false;
    }
    return true;
}

void NativeSkyStarsRenderer::render(ID3D12GraphicsCommandList* command_list,
                                    const NativeScene& scene, const SkyCamera& camera,
                                    std::uint32_t width, std::uint32_t height,
                                    double elapsed_seconds) {
    if (!pipeline_state_ || width == 0 || height == 0 || scene.star_brightness <= 0.0f) return;

    StarConstants c{};
    for (int i = 0; i < 3; ++i) {
        c.camera_right[i] = camera.right[i];
        c.camera_up[i] = camera.up[i];
        c.camera_forward[i] = camera.forward[i];
    }
    c.camera_right[3] = camera.tan_half_fov_x;
    c.camera_up[3] = camera.tan_half_fov_y;
    c.star_params[0] = static_cast<float>(elapsed_seconds);
    c.star_params[1] = scene.star_brightness;
    c.star_params[2] = kStarPointSize / static_cast<float>(width);
    c.star_params[3] = kStarPointSize / static_cast<float>(height);
    std::memcpy(mapped_constants_, &c, sizeof(c));

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width),
                                  static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
    command_list->SetPipelineState(pipeline_state_.Get());
    command_list->SetGraphicsRootSignature(root_signature_.Get());
    command_list->SetGraphicsRootConstantBufferView(0, constant_address_);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 0, nullptr);
    command_list->IASetIndexBuffer(nullptr);
    command_list->DrawInstanced(static_cast<UINT>(kStarCount) * 6u, 1, 0, 0);
}

}  // namespace f2
