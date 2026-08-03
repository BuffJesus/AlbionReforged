#pragma once

#include "native_scene.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace f2 {

class NativeWorldRenderer {
public:
    bool initialise(ID3D12Device* device, const NativeScene& scene, std::string& error);
    void render(ID3D12GraphicsCommandList* command_list,
                const NativeScene& scene,
                std::uint32_t width,
                std::uint32_t height,
                double elapsed_seconds);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    D3D12_VERTEX_BUFFER_VIEW vertex_view_{};
    D3D12_INDEX_BUFFER_VIEW index_view_{};
    D3D12_GPU_VIRTUAL_ADDRESS constant_address_ = 0;
    std::uint32_t index_count_ = 0;
    void* mapped_constants_ = nullptr;
};

}  // namespace f2
