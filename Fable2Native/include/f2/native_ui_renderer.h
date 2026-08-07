#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace f2 {

struct NativeUiQuad {
    D3D12_GPU_DESCRIPTOR_HANDLE texture{};
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    std::uint32_t color = 0xffffffffu;
    float rotation_radians = 0.0f;
    D3D12_GPU_DESCRIPTOR_HANDLE detail_texture{};
    float detail_u0 = 0.0f;
    float detail_v0 = 0.0f;
    float detail_u1 = 1.0f;
    float detail_v1 = 1.0f;
    bool combine_detail = false;
    bool key_black_matte = false;
};

class NativeUiRenderer {
public:
    bool initialise(ID3D12Device* device, std::string& error);
    bool ready() const { return pipeline_state_ != nullptr; }
    void render(ID3D12GraphicsCommandList* command_list,
                std::uint32_t width, std::uint32_t height,
                const std::vector<NativeUiQuad>& quads);

private:
    struct Vertex {
        float x = 0.0f;
        float y = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
        float u = 0.0f;
        float v = 0.0f;
        float detail_u = 0.0f;
        float detail_v = 0.0f;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    D3D12_VERTEX_BUFFER_VIEW vertex_view_{};
    Vertex* mapped_vertices_ = nullptr;
    std::size_t vertex_capacity_ = 0;
};

}  // namespace f2
