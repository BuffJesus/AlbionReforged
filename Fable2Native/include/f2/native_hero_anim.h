#pragma once

// Loader for the hero skeletal-animation package (.heroanim) baked by tools/cook_hero_anim.py.
// The package carries the hero's per-geom bind-pose SkinnedVertex data + the baked locomotion
// clips (per-frame per-bone 3x4 skin matrices, already in the runtime column-vector convention)
// + a reference idle pose for a bit-exact runtime-convention self-test. Ships nothing: it is
// cooked from the user's own game data (docs/NATIVE_PORT_PLAN.md).

#include "native_animation.h"

#include <array>
#include <string>
#include <vector>

namespace f2 {

struct HeroAnimData {
    bool ok = false;
    int bone_count = 0;
    std::vector<AnimClip> clips;                              // baked locomotion clips
    std::vector<float> root_speeds;                          // parallel to clips (wu/s)
    std::vector<std::vector<SkinnedVertex>> geom_bind;       // one bind-vertex list per hero geom
    std::vector<std::vector<std::array<float, 3>>> ref_pose; // idle@0 posed positions per geom (self-test)
};

// Parse a .heroanim file. Returns ok=false on a bad magic / version / truncation. Does NOT build
// LocomotionClip pointers (they would dangle if `clips` is later moved) — the caller pairs
// clips[i] with root_speeds[i] after taking ownership.
HeroAnimData load_hero_anim(const std::string& path);

// Compute the per-geom render-space hero pose to feed set_character_pose (shared by BOTH backends
// so the two apps can't drift). For each geom: skin its bind vertices with `player`'s current pose,
// swap MDL {x,y,z} -> render {x,z,y} (the cook_levels F2SCENE emit convention), then Y-rotate by
// `delta_yaw` using the renderer's place_vertex convention (x'=x*cy+z*sy, z'=-x*sy+z*cy) so the
// renderer's baked hero_yaw composes to the live heading (delta_yaw = facing_yaw - baked hero_yaw).
// out[gi] aligns with character mesh gi; `out` is resized.
void compute_hero_pose(const AnimationPlayer& player,
                       const std::vector<std::vector<SkinnedVertex>>& geom_bind, float delta_yaw,
                       std::vector<std::vector<std::array<float, 3>>>& out);

}  // namespace f2
