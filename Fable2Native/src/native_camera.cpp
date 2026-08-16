#include "f2/native_camera.h"

#include "f2/native_physics.h"

#include <algorithm>
#include <cmath>

namespace f2 {

void CameraController::update(const std::array<float, 3>& hero_position,
                              const std::array<float, 2>& look, bool look_is_mouse, float dt,
                              const NativeCollisionWorld* world) {
    // Integrate look input into the orbit angles. Mouse is per-pixel (frame-relative);
    // stick is a rate (per-second). Right/forward raises yaw/pitch.
    if (look_is_mouse) {
        yaw += look[0] * config.mouse_yaw_sensitivity;
        pitch -= look[1] * config.mouse_pitch_sensitivity;  // screen-down = look down
    } else {
        yaw += look[0] * config.stick_yaw_speed * dt;
        pitch += look[1] * config.stick_pitch_speed * dt;
    }
    pitch = std::clamp(pitch, config.min_pitch, config.max_pitch);

    // Look target above the hero's feet; camera pulls back along -forward.
    target = {hero_position[0], hero_position[1] + config.height, hero_position[2]};
    const float cp = std::cos(pitch);
    const std::array<float, 3> fwd{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
    std::array<float, 3> desired{target[0] - fwd[0] * config.distance,
                                 target[1] - fwd[1] * config.distance,
                                 target[2] - fwd[2] * config.distance};

    // Keep the camera above the terrain so it never clips through the ground.
    if (world) {
        const float gy = world->sample_ground(desired[0], desired[2], desired[1] + 100.0f);
        if (std::isfinite(gy) && desired[1] < gy + config.ground_margin) {
            desired[1] = gy + config.ground_margin;
        }
    }

    // Smooth the position toward the desired pose (snap on the first frame).
    if (!initialised_ || dt <= 0.0f) {
        position = desired;
        initialised_ = true;
    } else {
        const float a = 1.0f - std::exp(-config.follow_smoothing * dt);
        position = {position[0] + (desired[0] - position[0]) * a,
                    position[1] + (desired[1] - position[1]) * a,
                    position[2] + (desired[2] - position[2]) * a};
    }
}

}  // namespace f2
