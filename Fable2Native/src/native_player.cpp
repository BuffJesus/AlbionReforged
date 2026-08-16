#include "f2/native_player.h"

#include <cmath>

namespace f2 {

std::array<float, 2> NativePlayer::desired_velocity(const std::array<float, 2>& move_input,
                                                    float camera_yaw, float speed) {
    // move_input: x = strafe (right +), y = forward (+). Orient to the camera yaw so
    // "forward" is into the screen. Renderer forward at pitch 0 = (sin(yaw), 0, cos(yaw));
    // right = (cos(yaw), 0, -sin(yaw)).
    const float sy = std::sin(camera_yaw), cy = std::cos(camera_yaw);
    const float fwd_x = sy, fwd_z = cy;
    const float right_x = cy, right_z = -sy;
    float vx = right_x * move_input[0] + fwd_x * move_input[1];
    float vz = right_z * move_input[0] + fwd_z * move_input[1];
    // Clamp the input direction to unit length so diagonals aren't faster, then scale.
    const float mag = std::sqrt(vx * vx + vz * vz);
    if (mag > 1.0f) { vx /= mag; vz /= mag; }
    return {vx * speed, vz * speed};
}

void NativePlayer::update(const NativeCollisionWorld& world, const InputState& input,
                          float camera_yaw, float dt) {
    // Run when the left stick is pushed near full, or a run button (Left thumb) is held.
    const float move_mag = std::sqrt(input.move[0] * input.move[0] + input.move[1] * input.move[1]);
    const bool running = move_mag > 0.9f || input.down(PadButton::LeftThumb);
    const float speed = running ? config.run_speed : config.walk_speed;

    const std::array<float, 2> desired = desired_velocity(input.move, camera_yaw, speed);
    controller.move(world, desired, dt);

    // Face the movement direction when actually moving (for a future hero mesh yaw).
    if (desired[0] * desired[0] + desired[1] * desired[1] > 1e-4f) {
        facing_yaw_ = std::atan2(desired[0], desired[1]);
    }
}

}  // namespace f2
