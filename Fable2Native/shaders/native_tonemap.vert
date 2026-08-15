#version 450

// Fullscreen-triangle vertex shader for the HDR->LDR compositor (Vulkan mirror of the D3D12
// native_tonemap.hlsl vs_main). Emits a single oversized triangle from gl_VertexIndex; the UV is
// carried in [0,2] and clipped to [0,1] over the screen. Vulkan clip space matches D3D12 here
// because the position math already accounts for the flipped Y (uv.y*2-1 with a negated NDC).
layout(location = 0) out vec2 v_uv;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
