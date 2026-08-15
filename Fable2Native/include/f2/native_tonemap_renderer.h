#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace f2 {

// Full-screen HDR -> LDR compositor (the retail COMPOSITOR pass, ghidra_out/rendering_pipeline.txt
// §D.3: "result = mul_sat(exposure * sceneColor) + bloom"). Samples the RGBA16F scene target that
// every World pass renders into (see native_scene_color.h) and writes the tonemapped/exposed LDR
// result to the swap-chain back buffer, before the UI overlay.
//
// Stage 1 (this class' baseline) applies saturate(exposure * scene). With the default exposure of
// 1.0 that is bit-for-bit the same clamp the old direct-to-R8G8B8A8_UNORM path produced, so the
// verified World frames are unchanged until exposure/bloom are dialed in.
class NativeTonemapRenderer {
public:
    bool initialise(ID3D12Device* device, DXGI_FORMAT output_format, std::string& error);
    bool ready() const { return pipeline_state_ != nullptr; }

    // hdr_srv = GPU handle of the scene RGBA16F SRV, which must live in the shader-visible
    // descriptor heap the caller has already bound (SetDescriptorHeaps) before this call.
    void render(ID3D12GraphicsCommandList* command_list, D3D12_GPU_DESCRIPTOR_HANDLE hdr_srv,
                std::uint32_t width, std::uint32_t height, float exposure);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
};

}  // namespace f2
