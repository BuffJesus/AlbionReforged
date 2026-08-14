#version 450

layout(location = 0) out vec2 ndc;

void main() {
    vec2 corner;
    if (gl_VertexIndex == 0) corner = vec2(-1.0, -1.0);
    else if (gl_VertexIndex == 1) corner = vec2(-1.0, 3.0);
    else corner = vec2(3.0, -1.0);
    gl_Position = vec4(corner, 0.999, 1.0);
    ndc = corner;
}
