#include "f2/native_ui_renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace f2 {

namespace {

constexpr std::size_t kVertexCapacity = 2048;

float channel(std::uint32_t color, unsigned shift) {
    return static_cast<float>((color >> shift) & 0xffu) / 255.0f;
}

}  // namespace

bool NativeUiRenderer::initialise(ID3D12Device* device, std::string& error) {
    if (!device) {
        error = "The native UI renderer received no D3D12 device.";
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = sizeof(Vertex) * kVertexCapacity;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&vertex_buffer_))) ||
        FAILED(vertex_buffer_->Map(0, nullptr, reinterpret_cast<void**>(&mapped_vertices_)))) {
        error = "The native UI vertex buffer could not be allocated.";
        return false;
    }
    vertex_capacity_ = kVertexCapacity;

    Microsoft::WRL::ComPtr<ID3DBlob> vertex_shader;
    Microsoft::WRL::ComPtr<ID3DBlob> pixel_shader;
    Microsoft::WRL::ComPtr<ID3DBlob> shader_errors;
    constexpr char shader_source[] = R"(
cbuffer Viewport : register(b0) { float2 viewport; };
Texture2D ui_texture : register(t0);
Texture2D detail_texture : register(t1);
SamplerState ui_sampler : register(s0);
cbuffer UiMaterial : register(b1) { uint combine_detail; uint key_black_matte; };
struct VSInput { float2 position : POSITION; float4 color : COLOR; float2 uv : TEXCOORD0; float2 detail_uv : TEXCOORD1; };
struct PSInput { float4 position : SV_POSITION; float4 color : COLOR; float2 uv : TEXCOORD0; float2 detail_uv : TEXCOORD1; };
PSInput vs_main(VSInput input) {
    PSInput output;
    output.position = float4(input.position.x / viewport.x * 2.0 - 1.0,
                             1.0 - input.position.y / viewport.y * 2.0, 0.0, 1.0);
    output.color = input.color;
    output.uv = input.uv;
    output.detail_uv = input.detail_uv;
    return output;
}
float4 ps_main(PSInput input) : SV_TARGET {
    const float4 sampled = ui_texture.Sample(ui_sampler, input.uv);
    // The frame atlas contains a black export matte, but also authored near-
    // black pixels in the metallic outline.  Keep the latter opaque so the
    // leather body cannot show through the rim.
    if (key_black_matte != 0 && max(sampled.r, max(sampled.g, sampled.b)) < 0.10) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    if (combine_detail != 0) {
        const float4 detail = detail_texture.Sample(ui_sampler, input.detail_uv);
        return input.color * float4(detail.rgb, sampled.a);
    }
    return input.color * sampled;
}
)";
    const auto compile = [&](const char* entry, const char* target,
                             Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
        return D3DCompile(shader_source, sizeof(shader_source) - 1, "native_ui.hlsl",
                          nullptr, nullptr, entry, target, 0, 0, &blob, &shader_errors);
    };
    if (FAILED(compile("vs_main", "vs_5_0", vertex_shader)) ||
        FAILED(compile("ps_main", "ps_5_0", pixel_shader))) {
        error = "The native UI shaders could not be compiled.";
        return false;
    }

    D3D12_ROOT_PARAMETER root_parameters[4]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].Constants.ShaderRegister = 0;
    root_parameters[0].Constants.Num32BitValues = 2;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_DESCRIPTOR_RANGE texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = 1;
    texture_range.BaseShaderRegister = 0;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[1].Constants.ShaderRegister = 1;
    root_parameters[1].Constants.Num32BitValues = 2;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[2].DescriptorTable.pDescriptorRanges = &texture_range;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE detail_range{};
    detail_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    detail_range.NumDescriptors = 1;
    detail_range.BaseShaderRegister = 1;
    root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[3].DescriptorTable.pDescriptorRanges = &detail_range;
    root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_description{};
    root_description.NumParameters = 4;
    root_description.pParameters = root_parameters;
    root_description.NumStaticSamplers = 1;
    root_description.pStaticSamplers = &sampler;
    root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> root_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_description, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &root_blob, &shader_errors)) ||
        FAILED(device->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                            root_blob->GetBufferSize(),
                                            IID_PPV_ARGS(&root_signature_)))) {
        error = "The native UI root signature could not be created.";
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_signature_.Get();
    pipeline.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
    pipeline.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
    pipeline.InputLayout = {input_layout, static_cast<UINT>(std::size(input_layout))};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.SampleMask = UINT_MAX;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipeline.SampleDesc.Count = 1;
    pipeline.RasterizerState = rasterizer;
    pipeline.BlendState = blend;
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.StencilEnable = FALSE;
    if (FAILED(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pipeline_state_)))) {
        error = "The native UI pipeline could not be created.";
        return false;
    }
    vertex_view_.BufferLocation = vertex_buffer_->GetGPUVirtualAddress();
    vertex_view_.SizeInBytes = static_cast<UINT>(sizeof(Vertex) * vertex_capacity_);
    vertex_view_.StrideInBytes = sizeof(Vertex);
    return true;
}

void NativeUiRenderer::render(ID3D12GraphicsCommandList* command_list,
                              std::uint32_t width, std::uint32_t height,
                              const std::vector<NativeUiQuad>& quads) {
    if (!command_list || !ready() || width == 0 || height == 0 || quads.empty()) return;
    const std::size_t vertices_needed = quads.size() * 6;
    if (vertices_needed > vertex_capacity_) return;

    std::size_t vertex_index = 0;
    for (const auto& quad : quads) {
        const float r = channel(quad.color, 0);
        const float g = channel(quad.color, 8);
        const float b = channel(quad.color, 16);
        const float a = channel(quad.color, 24);
        const float center_x = (quad.x0 + quad.x1) * 0.5f;
        const float center_y = (quad.y0 + quad.y1) * 0.5f;
        const float cosine = std::cos(quad.rotation_radians);
        const float sine = std::sin(quad.rotation_radians);
        const auto position = [&](float x, float y) {
            const float dx = x - center_x;
            const float dy = y - center_y;
            return std::array<float, 2>{center_x + dx * cosine - dy * sine,
                                        center_y + dx * sine + dy * cosine};
        };
        const auto p00 = position(quad.x0, quad.y0);
        const auto p10 = position(quad.x1, quad.y0);
        const auto p11 = position(quad.x1, quad.y1);
        const auto p01 = position(quad.x0, quad.y1);
        const Vertex vertices[] = {
            {p00[0], p00[1], r, g, b, a, quad.u0, quad.v0, quad.detail_u0, quad.detail_v0},
            {p10[0], p10[1], r, g, b, a, quad.u1, quad.v0, quad.detail_u1, quad.detail_v0},
            {p11[0], p11[1], r, g, b, a, quad.u1, quad.v1, quad.detail_u1, quad.detail_v1},
            {p00[0], p00[1], r, g, b, a, quad.u0, quad.v0, quad.detail_u0, quad.detail_v0},
            {p11[0], p11[1], r, g, b, a, quad.u1, quad.v1, quad.detail_u1, quad.detail_v1},
            {p01[0], p01[1], r, g, b, a, quad.u0, quad.v1, quad.detail_u0, quad.detail_v1},
        };
        std::memcpy(mapped_vertices_ + vertex_index, vertices, sizeof(vertices));
        vertex_index += std::size(vertices);
    }

    command_list->SetPipelineState(pipeline_state_.Get());
    command_list->SetGraphicsRootSignature(root_signature_.Get());
    const float viewport[] = {static_cast<float>(width), static_cast<float>(height)};
    command_list->SetGraphicsRoot32BitConstants(0, 2, viewport, 0);
    const D3D12_VIEWPORT render_viewport{0.0f, 0.0f,
                                         static_cast<float>(width), static_cast<float>(height),
                                         0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    command_list->RSSetViewports(1, &render_viewport);
    command_list->RSSetScissorRects(1, &scissor);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 1, &vertex_view_);
    std::size_t first_vertex = 0;
    for (const auto& quad : quads) {
        const std::uint32_t material[] = {
            quad.combine_detail ? 1u : 0u,
            quad.key_black_matte ? 1u : 0u};
        command_list->SetGraphicsRoot32BitConstants(1, 2, material, 0);
        command_list->SetGraphicsRootDescriptorTable(2, quad.texture);
        command_list->SetGraphicsRootDescriptorTable(3,
                                                       quad.combine_detail ? quad.detail_texture
                                                                           : quad.texture);
        command_list->DrawInstanced(6, 1, static_cast<UINT>(first_vertex), 0);
        first_vertex += 6;
    }
}

}  // namespace f2
