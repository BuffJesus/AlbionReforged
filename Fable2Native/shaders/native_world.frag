#version 450

layout(location = 0) in vec4 color;
layout(location = 1) in vec2 uv;
layout(set = 0, binding = 1) uniform sampler2D albedo;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = color * texture(albedo, uv);
}
