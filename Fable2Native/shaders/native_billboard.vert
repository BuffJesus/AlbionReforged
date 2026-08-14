#version 450

// Celestial billboard vertex shader (Vulkan mirror of native_sky_billboard_renderer.cpp VS).
// Positions arrive already in clip-space NDC (+Y up; a flipped Vulkan viewport makes this match),
// built on the CPU from the shared SkyCamera by build_billboard().
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec2 v_uv;
void main() {
    v_uv = in_uv;
    gl_Position = vec4(in_position, 0.9985, 1.0);  // no depth test; z is a valid NDC filler
}
