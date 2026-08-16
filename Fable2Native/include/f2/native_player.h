#pragma once

// The controllable hero: a character controller driven by camera-relative input.
//
// Mirrors the retail input->desired-velocity->collide-and-slide->transform path
// (physics_collision_system.txt E). Movement SPEED has no retail constant — retail
// hero locomotion is ANIMATION ROOT MOTION (arrives with P5 skinned locomotion); the
// speeds here are flagged engineering defaults until then.

#include "native_input_state.h"
#include "native_physics.h"

#include <array>

namespace f2 {

struct PlayerMovementConfig {
    float walk_speed = 2.5f;  // ENGINEERING (retail = anim root motion, P5)
    float run_speed  = 6.0f;  // ENGINEERING
};

class NativePlayer {
public:
    PlayerMovementConfig config;
    CharacterController controller;

    void set_position(const std::array<float, 3>& p) { controller.position = p; }
    [[nodiscard]] const std::array<float, 3>& position() const noexcept { return controller.position; }
    [[nodiscard]] float facing_yaw() const noexcept { return facing_yaw_; }

    // Convert input.move (camera-relative planar intent) into a world desired velocity
    // and step the controller. `camera_yaw` orients the move to the view.
    void update(const NativeCollisionWorld& world, const InputState& input,
                float camera_yaw, float dt);

    // Map camera-relative planar input to a world-space desired velocity (unit test seam).
    static std::array<float, 2> desired_velocity(const std::array<float, 2>& move_input,
                                                 float camera_yaw, float speed);

private:
    float facing_yaw_ = 0.0f;  // hero heading (follows movement direction)
};

}  // namespace f2
