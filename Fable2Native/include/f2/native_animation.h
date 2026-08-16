#pragma once

// Runtime skeletal animation for the native port.
//
// Grounded in ghidra_out/anim_runtime_sampler_re.txt (the per-frame contract §A.5) +
// anim_pose_re.txt (the VALIDATED, bit-exact LBS skin math §3/§6). The clip codec,
// retarget, and pose->skin build are already validated in fable_pose.py; the cook
// runs them ONCE per clip to BAKE per-frame per-bone skin matrices, so the only new
// runtime code is what the spec flagged: advance time, pick/interpolate the frame,
// and LBS-skin the bind vertices. No guest sampler address and no codec port needed.
//
// Measured clips (child hero, 143 bones, 30 fps; net root translation metric):
//   idle id_1B78A889 (0.00 wu/s), walk id_02EE1AA7 (0.77), run id_8C7D7F7E (4.20).

#include <array>
#include <cstdint>
#include <vector>

namespace f2 {

// A skinned bind-pose vertex: model-space position + up to 4 bone influences.
struct SkinnedVertex {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};  // bind pose (model space)
    std::array<std::uint16_t, 4> bones{0, 0, 0, 0};
    std::array<float, 4> weights{1.0f, 0.0f, 0.0f, 0.0f};  // should sum to 1
};

// A baked clip: per-frame, per-bone SKIN matrix (invBind_i . world_i, 3x4 row-major),
// precomputed by the cook via the validated codec+LBS. Playing = pick the frame(s) by
// time and LBS-skin. skin[frame*bone_count + bone].
struct AnimClip {
    std::uint32_t hash = 0;
    int bone_count = 0;
    int frame_count = 0;
    float fps = 30.0f;
    std::vector<std::array<float, 12>> skin;  // frame_count * bone_count, 3x4 row-major

    [[nodiscard]] float duration() const noexcept {
        return (fps > 0.0f && frame_count > 0) ? static_cast<float>(frame_count) / fps : 0.0f;
    }
};

// Plays one clip: advances time, interpolates the per-bone skin matrices between the
// two nearest frames, and LBS-skins bind vertices. Single-clip (no cross-fade yet —
// clip selection/blend is an engineering choice per the spec §C).
class AnimationPlayer {
public:
    void set_clip(const AnimClip* clip);
    [[nodiscard]] const AnimClip* clip() const noexcept { return clip_; }
    [[nodiscard]] float time() const noexcept { return time_; }

    // Advance time (loops on the clip duration) and rebuild the interpolated pose.
    void update(float dt);

    // The interpolated per-bone skin matrices for the current time (bone_count entries).
    [[nodiscard]] const std::vector<std::array<float, 12>>& pose() const noexcept { return pose_; }

    // LBS-skin `base` into `out_positions` using the current pose (out is resized).
    void skin(const std::vector<SkinnedVertex>& base,
              std::vector<std::array<float, 3>>& out_positions) const;

    // Apply a 3x4 row-major skin matrix to a point (exposed for tests/reuse).
    static std::array<float, 3> transform_point(const std::array<float, 12>& m,
                                                const std::array<float, 3>& p) noexcept;

private:
    void rebuild_pose();
    const AnimClip* clip_ = nullptr;
    float time_ = 0.0f;
    std::vector<std::array<float, 12>> pose_;
};

// A locomotion clip tagged with its intrinsic root speed (measured, wu/s):
// idle id_1B78A889 = 0.00, walk id_02EE1AA7 = 0.77, run id_8C7D7F7E = 4.20
// (anim_runtime_sampler_re.txt, net-root-translation metric).
struct LocomotionClip {
    const AnimClip* clip = nullptr;
    float root_speed = 0.0f;
};

// Pick the locomotion clip whose intrinsic root speed best matches the character's
// planar `speed` (wu/s) — nearest-speed match so the foot plant tracks ground motion
// (minimises sliding). The clip SPEEDS are measured data; this nearest-match SELECTION
// POLICY is the engineering choice the spec flags (retail selection is engine/AI-side,
// anim_runtime_sampler_re §C). Returns nullptr if the set is empty.
[[nodiscard]] const AnimClip* select_locomotion_clip(float speed,
                                                     const std::vector<LocomotionClip>& set);

}  // namespace f2
