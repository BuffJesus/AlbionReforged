#pragma once

#include "native_scene.h"
#include "sky_camera.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace f2 {

// Procedural night star field (theme STARS_BRIGHTNESS; retail reference: Fable2AssetBrowser
// SkyDomeXex kStarsVertexShaderHlsl/kStarsPixelShaderHlsl — a faithful RE of the retail Xenos star
// ucode). Draws kStarCount (512) additive point-sprite quads generated entirely from SV_VertexID
// (no vertex/index buffer, no texture): a fixed hemisphere of hashed star directions with a
// per-star twinkle, projected through the shared SkyCamera. Drawn after the clouds/moon, behind
// the world. A no-op unless the scene authors stars (star_brightness > 0 → night).
class NativeSkyStarsRenderer {
public:
    bool initialise(ID3D12Device* device, std::string& error);
    void render(ID3D12GraphicsCommandList* command_list,
                const NativeScene& scene,
                const SkyCamera& camera,
                std::uint32_t width,
                std::uint32_t height,
                double elapsed_seconds);
    bool ready() const { return pipeline_state_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_;
    D3D12_GPU_VIRTUAL_ADDRESS constant_address_ = 0;
    void* mapped_constants_ = nullptr;
};

}  // namespace f2
