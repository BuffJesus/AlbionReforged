#pragma once

// Minimal Havok-free collision layer for the native port.
//
// Retail uses Havok (CECPhysicsSimulationCharacterControlled @0x82630B30; the
// controller struct entity->comp+0x20->+0x38; ground probe GetGroundPositionAtPosition
// @0x821E1160; ECType==3 gate @0x8218DD00). We do NOT ship Havok — this reproduces the
// CONTRACTS the movement code queries (ground sample + collide-and-slide against static
// geometry) with a lightweight AABB world + a terrain heightfield built from the scene.
//
// Grounded constants (ghidra_out/p2_feel_constants.txt):
//   capsule_radius 1.0 (globals.gdb char-controlled Radius field), max-slope 70deg
//   (0x82100efc), ground-clamp threshold 1.0 (0x82099490). Height / gravity / step are
//   flagged engineering defaults (no retail source — Havok Cinfo thunk-blocked).

#include "native_scene.h"

#include <array>
#include <cstdint>
#include <vector>

namespace f2 {

struct Aabb {
    std::array<float, 3> min{0.0f, 0.0f, 0.0f};
    std::array<float, 3> max{0.0f, 0.0f, 0.0f};
};

// Character-controller tunables. Values grounded where cited; others flagged.
struct CharacterControllerConfig {
    float capsule_radius = 1.0f;   // GROUNDED: char-controlled physics Radius (globals.gdb rec 0x2dbe4141)
    float capsule_height = 2.0f;   // ENGINEERING: Havok Cinfo default not RE'd
    float cos_max_slope  = 0.342020143f;  // cos(70deg) — GROUNDED max-slope default (0x82100efc = 70deg)
    float ground_clamp   = 1.0f;   // GROUNDED-ish: MoveClampToGround probe threshold (0x82099490)
    float gravity        = 9.81f;  // ENGINEERING: no retail constant
    float step_height    = 0.5f;   // ENGINEERING
};

// Static collision world built once from a cooked NativeScene: per-instance world
// AABBs (walls) + a terrain heightfield (ground). Mirrors the two queries retail
// movement makes — collide-and-slide and GetGroundPositionAtPosition.
class NativeCollisionWorld {
public:
    void build_from_scene(const NativeScene& scene);

    // Ground height under (x,z) at/below reference_y (+ small tolerance), or NaN if
    // no terrain there. Mirrors GetGroundPositionAtPosition @0x821E1160.
    [[nodiscard]] float sample_ground(float x, float z, float reference_y) const;
    [[nodiscard]] bool has_terrain() const noexcept { return grid_dim_ > 0; }

    // Resolve a horizontal move for a vertical capsule of `radius` at `pos`, sliding
    // along static AABBs it would penetrate (single-pass axis pushout = collide-and-slide).
    [[nodiscard]] std::array<float, 3> slide_move(const std::array<float, 3>& pos,
                                                  const std::array<float, 3>& delta,
                                                  float radius) const;

    [[nodiscard]] const std::vector<Aabb>& static_boxes() const noexcept { return boxes_; }

private:
    std::vector<Aabb> boxes_;
    // The terrain instance's box index (ground, resolved by the heightfield) — excluded
    // from wall collision so a spawn on the terrain isn't ejected by its huge AABB.
    int terrain_box_ = -1;

    // Uniform heightfield grid over the terrain XZ bounds (max terrain Y per cell).
    std::vector<float> grid_;   // grid_dim_ * grid_dim_, NaN = empty
    int grid_dim_ = 0;
    float grid_min_x_ = 0.0f, grid_min_z_ = 0.0f, grid_cell_ = 1.0f;

    [[nodiscard]] float grid_at(int cx, int cz) const;
};

// A capsule character driven by a desired horizontal velocity. Mirrors the retail
// input->SetVelocity->step->transform-write path (physics_collision_system.txt E/G.2).
class CharacterController {
public:
    CharacterControllerConfig config;
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> velocity{0.0f, 0.0f, 0.0f};  // y = fall speed
    bool on_ground = false;

    // Integrate one step: apply desired planar velocity, collide-and-slide against
    // static boxes, then ground-clamp (snap to terrain within ground_clamp) or fall.
    void move(const NativeCollisionWorld& world,
              const std::array<float, 2>& desired_planar_velocity, float dt);
};

}  // namespace f2
