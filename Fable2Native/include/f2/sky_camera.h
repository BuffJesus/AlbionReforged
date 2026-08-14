#pragma once

#include <array>

namespace f2 {

// Camera basis the sky pass needs, in the SAME convention the world renderer uses
// (Y-up render space). Each world renderer (D3D12 + Vulkan) fills this via its
// compute_camera() so the sky's fullscreen-triangle rays line up pixel-for-pixel with
// the world geometry. Kept backend-neutral (no D3D12/Vulkan headers) so both the
// D3D12 and Vulkan sky/world renderers can share it.
struct SkyCamera {
    std::array<float, 3> position{};   // eye, world/render space
    std::array<float, 3> right{};      // orthonormal camera basis
    std::array<float, 3> up{};
    std::array<float, 3> forward{};
    float tan_half_fov_x = 1.0f;       // right.w
    float tan_half_fov_y = 1.0f;       // up.w
};

}  // namespace f2
