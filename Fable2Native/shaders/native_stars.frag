#version 450

// Procedural night stars fragment shader (Vulkan mirror; retail kStarsPixelShaderHlsl):
// output the star brightness in rgb, alpha 0 — additive (ONE/ONE) over the sky.
layout(location = 0) in float v_colour;
layout(location = 0) out vec4 o_color;
void main() {
    o_color = vec4(vec3(v_colour), 0.0);
}
