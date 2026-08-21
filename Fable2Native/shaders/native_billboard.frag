#version 450

// Celestial billboard fragment shader (Vulkan mirror; SkyboxRenderer.cpp kSkyElementPixelShader):
// element texture tinted by the HDR-scaled + tonemapped element colour (rgb tint, a alpha).
layout(set = 0, binding = 0) uniform BillboardCB { vec4 element_colour; } cb;
layout(set = 0, binding = 1) uniform sampler2D element_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 texel = texture(element_tex, v_uv);
    o_color = vec4(texel.rgb * cb.element_colour.rgb, texel.a * cb.element_colour.a);
}
