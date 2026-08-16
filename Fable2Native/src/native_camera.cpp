#include "f2/native_camera.h"

#include <algorithm>
#include <cmath>

namespace f2 {

void CameraController::update(const std::array<float, 3>& hero_position,
                              const std::array<float, 2>& look, bool look_is_mouse, float dt) {
    // Integrate look input into the orbit angles. Mouse is per-pixel (frame-relative);
    // stick is a rate (per-second). Right/forward stick raises yaw/pitch.
    if (look_is_mouse) {
        yaw += look[0] * config.mouse_yaw_sensitivity;
        pitch -= look[1] * config.mouse_pitch_sensitivity;  // screen-down = look down
    } else {
        yaw += look[0] * config.stick_yaw_speed * dt;
        pitch += look[1] * config.stick_pitch_speed * dt;
    }
    pitch = std::clamp(pitch, config.min_pitch, config.max_pitch);

    // Look target sits above the hero's feet; camera pulls back along -forward.
    target = {hero_position[0], hero_position[1] + config.height, hero_position[2]};
    const float cp = std::cos(pitch);
    const std::array<float, 3> fwd{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
    position = {target[0] - fwd[0] * config.distance,
                target[1] - fwd[1] * config.distance,
                target[2] - fwd[2] * config.distance};
}

}  // namespace f2
