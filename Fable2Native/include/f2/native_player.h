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
#include <cmath>

namespace f2 {

struct PlayerMovementConfig {
    // GROUNDED: retail hero locomotion IS animation root motion (p2_feel_constants: no code speed
    // literal), and these are the MEASURED intrinsic root speeds of the child hero's walk/run
    // clips (anim_runtime_sampler_re.txt §B, net-root-translation). Moving at the clip's own root
    // speed makes the feet track the ground (no foot-slide) at playback rate 1.0. FLAGGED: the
    // walk<->run THRESHOLD + the in-between blend remain an engineering choice (§C).
    float walk_speed = 0.77f;  // GROUNDED: walk clip id_02EE1AA7 root speed (wu/s)
    float run_speed  = 4.20f;  // GROUNDED: run clip id_8C7D7F7E root speed (wu/s)
};

class NativePlayer {
public:
    PlayerMovementConfig config;
    CharacterController controller;

    void set_position(const std::array<float, 3>& p) { controller.position = p; }
    [[nodiscard]] const std::array<float, 3>& position() const noexcept { return controller.position; }
    [[nodiscard]] float facing_yaw() const noexcept { return facing_yaw_; }

    // Override the movement-derived heading (Physics.SetFacingVector — scripts turn the hero
    // to face a point during dialogue/cutscene setup). update() overwrites this again whenever
    // the hero is actually moving, so a script-set facing holds only while the hero is still.
    void set_facing_yaw(float yaw) noexcept { facing_yaw_ = yaw; }

    // Planar (XZ) speed of the last controller step (wu/s) — feeds locomotion-clip selection
    // and Physics.GetVelocity. Read from the controller's velocity (no duplicate state).
    [[nodiscard]] float planar_speed() const noexcept {
        return std::sqrt(controller.velocity[0] * controller.velocity[0] +
                         controller.velocity[2] * controller.velocity[2]);
    }

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
