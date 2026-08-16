#pragma once

// Grounded ACT layer for a townsperson / creature NPC.
//
// From ghidra_out/npc_ai_brain_system.txt an NPC splits into DECIDE and ACT:
//   DECIDE = CECAIBrain (typeId 71) + the AIManager Lua coroutines that score a
//            BEHAVIOUR GROUP. This is SCRIPTED (Lua) — NOT ported here; it awaits the
//            Lua VM (P6) and the behaviour-scoring RE gap. The NpcState machine below
//            is an ENGINEERING STAND-IN so a placed NPC does something visible.
//   ACT    = native components the brain drives, reproduced here (grounded):
//            CECPerception (61)  -> can_see() = raycast LOS + range
//            char controller     -> move_to() = collision-resolved motor
//            LOD gate            -> update_lod() (IsEntityWithinDistanceOfLODCentre)
//
// Motor speed is an ENGINEERING default (retail NPC locomotion is animation root
// motion, P5). Everything invented is flagged; the grounded parts cite the spec.

#include "native_physics.h"

#include <array>

namespace f2 {

struct NpcPerceptionConfig {
    float eye_height = 1.6f;    // sight ray origin/target height above feet
    float sight_range = 20.0f;  // ENGINEERING: no retail ACT-layer constant
    float notice_range = 6.0f;  // ENGINEERING
};

struct NpcMovementConfig {
    float walk_speed = 1.5f;    // ENGINEERING (retail = anim root motion, P5)
    float arrive_radius = 0.6f;
};

// Stand-in brain state (ENGINEERING — the real DECIDE layer is Lua behaviour scoring).
enum class NpcState {
    Idle,     // standing (default)
    Notice,   // saw the target within notice range: turn to face it
    Patrol,   // moving toward an externally-set goal (a real brain would set it)
};

class NpcController {
public:
    CharacterController controller;  // grounded: the char controller (physics_collision)
    NpcPerceptionConfig perception;
    NpcMovementConfig movement;
    NpcState state = NpcState::Idle;
    bool active = true;              // LOD gate result; inactive NPCs skip their tick

    void set_position(const std::array<float, 3>& p) { controller.position = p; }
    [[nodiscard]] const std::array<float, 3>& position() const noexcept { return controller.position; }
    [[nodiscard]] float facing_yaw() const noexcept { return facing_yaw_; }

    // Perception (grounded): LOS unobstructed AND within sight range. Sight ray runs at
    // eye height (CECPerception CanDirectlySee = the physics ray, npc_ai_brain_system).
    [[nodiscard]] bool can_see(const NativeCollisionWorld& world,
                               const std::array<float, 3>& target) const;

    // LOD gate (grounded): within `radius` of the LOD centre (IsEntityWithinDistanceOf
    // LODCentre 0x8229BFD0). Sets + returns `active`.
    bool update_lod(const std::array<float, 3>& lod_centre, float radius);

    // Motor (grounded): step toward `goal` at walk_speed via the collision-resolved
    // character controller. Returns true once within arrive_radius.
    bool move_to(const NativeCollisionWorld& world, const std::array<float, 3>& goal, float dt);

    // Give the NPC a patrol goal (a real brain drives this; the stand-in never invents
    // autonomous wandering). Sets state to Patrol.
    void set_patrol_goal(const std::array<float, 3>& goal);

    // ACT tick: LOD gate -> perception -> stand-in brain -> motor. `target` = the player
    // (or a point of interest). Idle/Notice are automatic; Patrol runs if a goal is set.
    void update(const NativeCollisionWorld& world, const std::array<float, 3>& target,
                const std::array<float, 3>& lod_centre, float lod_radius, float dt);

private:
    void face(const std::array<float, 3>& toward);
    std::array<float, 3> patrol_goal_{0.0f, 0.0f, 0.0f};
    bool have_patrol_goal_ = false;
    float facing_yaw_ = 0.0f;
};

}  // namespace f2
