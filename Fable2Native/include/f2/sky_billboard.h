#pragma once

#include "sky_camera.h"

#include <array>

namespace f2 {

// One billboard vertex: clip-space NDC position (xy) + texture uv. Backend-neutral so the D3D12
// and Vulkan celestial-billboard renderers build IDENTICAL geometry from the shared SkyCamera.
struct BillboardVertex {
    float x, y;
    float u, v;
};

// Build a camera-facing billboard's 6 vertices (two triangles) directly in clip-space NDC
// (+Y up, D3D convention — a flipped Vulkan viewport makes this match), exactly like the retail
// SkyboxRenderer.cpp draw_billboard: centre = direction*distance, then project each corner
// through the camera basis. Returns false when the billboard is behind the camera (caller skips).
inline bool build_billboard(const SkyCamera& cam, const std::array<float, 3>& direction,
                            float distance, float half_w, float half_h, float u0, float u1,
                            std::array<BillboardVertex, 6>& out) {
    const std::array<float, 3> centre{direction[0] * distance, direction[1] * distance,
                                      direction[2] * distance};
    auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    if (dot(centre, cam.forward) <= 0.0f) return false;
    const float corner_x[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
    const float corner_y[4] = {1.0f, 1.0f, -1.0f, -1.0f};
    std::array<BillboardVertex, 4> quad{};
    for (int c = 0; c < 4; ++c) {
        std::array<float, 3> world{};
        for (int a = 0; a < 3; ++a) {
            world[a] = centre[a] + cam.right[a] * corner_x[c] * half_w +
                       cam.up[a] * corner_y[c] * half_h;
        }
        float depth = dot(world, cam.forward);
        if (depth < 0.01f) depth = 0.01f;
        quad[c].x = dot(world, cam.right) / (depth * cam.tan_half_fov_x);
        quad[c].y = dot(world, cam.up) / (depth * cam.tan_half_fov_y);
        quad[c].u = corner_x[c] > 0.0f ? u1 : u0;
        quad[c].v = corner_y[c] > 0.0f ? 0.0f : 1.0f;
    }
    out = {quad[0], quad[1], quad[2], quad[2], quad[1], quad[3]};
    return true;
}

}  // namespace f2
