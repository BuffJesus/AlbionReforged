#pragma once

// Minimal Havok-free collision layer for the native port.
//
// Retail uses Havok (CECPhysicsSimulationCharacterControlled @0x82630B30; controller
// entity->comp+0x20->+0x38; ground probe GetGroundPositionAtPosition @0x821E1160). We
// do NOT ship Havok — this reproduces the two queries movement makes:
//   * GROUND = a terrain heightfield (walkable floor, rasterized from the level mesh),
//   * WALLS  = the actual steep (near-vertical) triangles of the props/buildings,
//     resolved as vertical prisms so the hero can walk up to a wall, round a corner,
//     and through a doorway gap (a whole-mesh AABB could do none of these).
//
// Grounded constants (ghidra_out/p2_feel_constants.txt): capsule_radius 1.0
// (globals.gdb char-controlled Radius 0xd422bc6f), max-slope 70deg (0x82100efc),
// ground-clamp 1.0 (0x82099490). Height/gravity/step are flagged engineering defaults.

#include "native_scene.h"

#include <array>
#include <cstdint>
#include <vector>

namespace f2 {

struct CharacterControllerConfig {
    float capsule_radius = 1.0f;   // GROUNDED: char-controlled physics Radius (globals.gdb rec 0x2dbe4141)
    float capsule_height = 2.0f;   // ENGINEERING: Havok Cinfo default not RE'd
    float cos_max_slope  = 0.342020143f;  // cos(70deg) — GROUNDED max-slope default (0x82100efc)
    float ground_clamp   = 1.0f;   // GROUNDED-ish: MoveClampToGround probe threshold (0x82099490)
    float gravity        = 9.81f;  // ENGINEERING
    float step_height    = 0.5f;   // ENGINEERING
};

// A wall triangle (world space) kept for narrowphase. Only near-vertical triangles are
// stored (floors/ceilings are the heightfield's job). ymin/ymax gate vertical overlap.
struct WallTri {
    std::array<float, 3> a{}, b{}, c{};
    float ymin = 0.0f, ymax = 0.0f;
};

class NativeCollisionWorld {
public:
    void build_from_scene(const NativeScene& scene);

    // Ground height under (x,z) at/below reference_y (+ tolerance), or NaN if none.
    [[nodiscard]] float sample_ground(float x, float z, float reference_y) const;
    [[nodiscard]] bool has_terrain() const noexcept { return grid_dim_ > 0; }

    // Resolve a horizontal move for a vertical capsule (radius, height) at `pos` (feet),
    // sliding along wall triangles it would penetrate. Several relaxation passes so a
    // corner (two walls) settles. delta is the intended feet displacement this step.
    [[nodiscard]] std::array<float, 3> slide_move(const std::array<float, 3>& pos,
                                                  const std::array<float, 3>& delta,
                                                  float radius, float height) const;

    // Distance to the nearest wall triangle along origin + dir*[0, max_dist] (dir unit),
    // or max_dist if none. Camera collision uses this to pull the eye in on a hit
    // (retail DoesCameraRayIntersect @0x822B77E8, the physics ray focus->eye, §2).
    [[nodiscard]] float raycast_walls(const std::array<float, 3>& origin,
                                      const std::array<float, 3>& dir, float max_dist) const;

    // True if no wall blocks the segment from `a` to `b` (a physics ray between two
    // points). Backs NPC sight + player targeting — retail IsLineOfSight @0x82497830 /
    // CECPerception CanDirectlySee @0x828BE650, which cast the same physics ray
    // (npc_ai_brain_system.txt). Endpoints assumed already at eye/target height.
    [[nodiscard]] bool line_of_sight(const std::array<float, 3>& a,
                                     const std::array<float, 3>& b) const;

    [[nodiscard]] std::size_t wall_triangle_count() const noexcept { return walls_.size(); }

private:
    // Terrain heightfield.
    std::vector<float> grid_;   // grid_dim_ * grid_dim_, NaN = empty
    int grid_dim_ = 0;
    float grid_min_x_ = 0.0f, grid_min_z_ = 0.0f, grid_cell_ = 1.0f;
    [[nodiscard]] float grid_at(int cx, int cz) const;

    // Wall triangles + a uniform XZ bucket grid over the scene bounds for broadphase.
    std::vector<WallTri> walls_;
    std::vector<std::vector<std::uint32_t>> wall_cells_;  // wcells_dim_^2 buckets
    int wcells_dim_ = 0;
    float wcell_min_x_ = 0.0f, wcell_min_z_ = 0.0f, wcell_size_ = 1.0f;

    // Push a circle (centre c, radius r) out of any nearby wall triangle overlapping
    // [y0,y1]. Returns the corrected XZ centre. One relaxation pass.
    [[nodiscard]] std::array<float, 2> resolve_walls(std::array<float, 2> c, float r,
                                                     float y0, float y1) const;
};

// A capsule character driven by a desired horizontal velocity.
class CharacterController {
public:
    CharacterControllerConfig config;
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> velocity{0.0f, 0.0f, 0.0f};
    bool on_ground = false;

    void move(const NativeCollisionWorld& world,
              const std::array<float, 2>& desired_planar_velocity, float dt);
};

}  // namespace f2
