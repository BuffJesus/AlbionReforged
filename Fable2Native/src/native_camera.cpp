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

    // Look target above the hero's feet; the camera sits back along -forward (centred
    // behind — the default follow, not over-the-shoulder; see header).
    target = {hero_position[0], hero_position[1] + config.height, hero_position[2]};
    const float cp = std::cos(pitch);
    const std::array<float, 3> fwd{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};

    // Camera collision: raycast from the focus back toward the eye; if a wall is closer
    // than the ideal boom length, pull the eye in to just short of it (retail §2, the
    // DoesCameraRayIntersect focus->eye physics ray). Boom retracts fast, extends slow.
    float allowed = config.distance;
    if (world) {
        const std::array<float, 3> eye_dir{-fwd[0], -fwd[1], -fwd[2]};
        const float reach = config.distance + config.collision_margin;
        const float hit = world->raycast_walls(target, eye_dir, reach);
        if (hit < reach) allowed = std::max(config.min_distance, hit - config.collision_margin);
    }
    if (current_distance_ < 0.0f) {
        current_distance_ = allowed;  // snap on the first frame
    } else {
        const float rate = (allowed < current_distance_) ? config.boom_in_speed : config.boom_out_speed;
        const float step = rate * dt;
        if (std::abs(allowed - current_distance_) <= step) current_distance_ = allowed;
        else current_distance_ += (allowed > current_distance_) ? step : -step;
    }

    position = {target[0] - fwd[0] * current_distance_, target[1] - fwd[1] * current_distance_,
                target[2] - fwd[2] * current_distance_};

    // Keep the eye above the terrain so it never clips through the ground.
    if (world) {
        const float gy = world->sample_ground(position[0], position[2], position[1] + 100.0f);
        if (std::isfinite(gy) && position[1] < gy + config.ground_margin) {
            position[1] = gy + config.ground_margin;
        }
    }
}

}  // namespace f2
