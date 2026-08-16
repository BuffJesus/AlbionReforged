#include "f2/native_animation.h"

#include <cmath>

namespace f2 {

std::array<float, 3> AnimationPlayer::transform_point(const std::array<float, 12>& m,
                                                      const std::array<float, 3>& p) noexcept {
    // 3x4 row-major: [m0 m1 m2 m3 ; m4 m5 m6 m7 ; m8 m9 m10 m11], last row implied (0,0,0,1).
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
            m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

void AnimationPlayer::set_clip(const AnimClip* clip) {
    clip_ = clip;
    time_ = 0.0f;
    rebuild_pose();
}

void AnimationPlayer::update(float dt) {
    if (!clip_ || clip_->frame_count <= 0) return;
    const float dur = clip_->duration();
    if (dur <= 0.0f) return;
    time_ += dt;
    // Loop.
    time_ -= std::floor(time_ / dur) * dur;
    rebuild_pose();
}

void AnimationPlayer::rebuild_pose() {
    if (!clip_ || clip_->bone_count <= 0 || clip_->frame_count <= 0) {
        pose_.clear();
        return;
    }
    const int bones = clip_->bone_count;
    const int frames = clip_->frame_count;
    pose_.assign(static_cast<std::size_t>(bones), std::array<float, 12>{});

    // Frame indices bracketing the current time; linear blend between them (frames are
    // dense at 30fps). The clip loops, so the last frame blends back to the first.
    const float fpos = time_ * clip_->fps;
    int f0 = static_cast<int>(std::floor(fpos));
    float alpha = fpos - static_cast<float>(f0);
    f0 %= frames;
    if (f0 < 0) f0 += frames;
    const int f1 = (f0 + 1) % frames;

    const auto* a = &clip_->skin[static_cast<std::size_t>(f0) * bones];
    const auto* b = &clip_->skin[static_cast<std::size_t>(f1) * bones];
    for (int i = 0; i < bones; ++i) {
        for (int k = 0; k < 12; ++k) {
            pose_[i][k] = a[i][k] * (1.0f - alpha) + b[i][k] * alpha;
        }
    }
}

const AnimClip* select_locomotion_clip(float speed, const std::vector<LocomotionClip>& set) {
    const AnimClip* best = nullptr;
    float best_diff = 0.0f;
    for (const LocomotionClip& lc : set) {
        if (!lc.clip) continue;
        const float diff = std::abs(lc.root_speed - speed);
        if (!best || diff < best_diff) {
            best = lc.clip;
            best_diff = diff;
        }
    }
    return best;
}

void AnimationPlayer::skin(const std::vector<SkinnedVertex>& base,
                           std::vector<std::array<float, 3>>& out_positions) const {
    out_positions.resize(base.size());
    const int bones = static_cast<int>(pose_.size());
    for (std::size_t v = 0; v < base.size(); ++v) {
        const SkinnedVertex& sv = base[v];
        std::array<float, 3> acc{0.0f, 0.0f, 0.0f};
        float wsum = 0.0f;
        for (int j = 0; j < 4; ++j) {
            const float w = sv.weights[j];
            if (w <= 0.0f) continue;
            const int bone = sv.bones[j];
            if (bone < 0 || bone >= bones) continue;
            const auto p = transform_point(pose_[static_cast<std::size_t>(bone)], sv.position);
            acc[0] += p[0] * w; acc[1] += p[1] * w; acc[2] += p[2] * w;
            wsum += w;
        }
        // Fall back to the bind position if the vertex had no valid influence.
        out_positions[v] = (wsum > 0.0f) ? acc : sv.position;
    }
}

}  // namespace f2
