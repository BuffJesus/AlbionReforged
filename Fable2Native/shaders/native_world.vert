#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_normal;
layout(location = 4) in vec4 in_probe;

layout(set = 0, binding = 0) uniform Camera {
    mat4 view_projection;
    vec4 sun_direction;   // xyz = light dir (world)
    vec4 sun_color;       // rgb = directional sun colour
} camera;
layout(push_constant) uniform Push {
    uint is_water;
    uint has_scene_depth;
    uint is_character;
    uint _padding;
    vec4 character_offset;
    vec4 character_motion;
} pc;

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 uv;
layout(location = 2) out vec3 normal;
layout(location = 3) out vec4 probe;
layout(location = 4) out vec3 world_pos;

void main() {
    vec3 motion = vec3(0.0, sin(pc.character_motion.x) * 0.045 * pc.character_motion.y, 0.0);
    vec3 world_position = in_position +
                          (pc.is_character != 0u ? pc.character_offset.xyz + motion : vec3(0.0));
    gl_Position = camera.view_projection * vec4(world_position, 1.0);
    color = in_color;
    uv = in_uv;
    normal = in_normal;
    probe = in_probe;
    world_pos = world_position;   // make_geometry bakes world-space positions
}
