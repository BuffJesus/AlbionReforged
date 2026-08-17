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
    // Retail hero locomotion IS animation root motion (p2_feel_constants: no code speed literal),
    // so the hero moves at the loaded walk/run clip's OWN measured root speed and the feet track
    // the ground at playback rate 1.0. NativeGame::load_hero_anim_package OVERRIDES these with the
    // cooked clips' measured root speeds (data-backed) — these are only the fallback when no hero
    // anim package is loaded. FLAGGED: the walk<->run threshold + in-between blend are engineering
    // (anim_runtime_sampler_re.txt §C).
    float walk_speed = 1.30f;  // fallback (overridden by the cooked walk clip's measured root speed)
    float run_speed  = 1.30f;  // fallback (overridden by the cooked run clip's measured root speed)
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
