#pragma once

#include <d3d12.h>

namespace f2 {

// Shared HDR scene-colour format for the D3D12 World-state passes
// (sky / clouds / billboards / stars / world / water).
//
// The retail engine renders the world into an HDR RGBA16F target and then runs a
// COMPOSITOR pass (tonemap/exposure + bloom) down to the LDR back buffer
// (ghidra_out/rendering_pipeline.txt §D.3). Native mirrors that: every World pass
// writes into an R16G16B16A16_FLOAT scene target, and NativeTonemapRenderer resolves
// it to the swap-chain back buffer. All World-pass PSOs must declare this RTV format.
constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

}  // namespace f2
