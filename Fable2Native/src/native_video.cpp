#include "f2/native_video.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace f2 {

bool NativeVideoPlayer::set_sequence(std::vector<VideoClip> clips) {
    if (clips.empty() || std::ranges::any_of(clips, [](const VideoClip& clip) {
            return clip.id.empty() || clip.asset.empty() || clip.duration_seconds <= 0.0;
        })) {
        return false;
    }
    clips_ = std::move(clips);
    reset();
    return true;
}

void NativeVideoPlayer::reset() {
    current_index_ = 0;
    current_time_ = 0.0;
    state_ = clips_.empty() ? VideoPlaybackState::Stopped : VideoPlaybackState::Playing;
}

void NativeVideoPlayer::tick(double delta_seconds) {
    if (state_ != VideoPlaybackState::Playing) return;
    current_time_ += std::clamp(delta_seconds, 0.0, 0.25);
    while (state_ == VideoPlaybackState::Playing && current_time_ >= clips_[current_index_].duration_seconds) {
        current_time_ -= clips_[current_index_].duration_seconds;
        advance();
    }
}

void NativeVideoPlayer::skip_current() {
    if (state_ == VideoPlaybackState::Playing && clips_[current_index_].skippable) {
        advance();
    }
}

void NativeVideoPlayer::skip_all() {
    if (state_ == VideoPlaybackState::Playing) {
        current_index_ = clips_.size();
        current_time_ = 0.0;
        state_ = VideoPlaybackState::Finished;
    }
}

const VideoClip* NativeVideoPlayer::current_clip() const noexcept {
    return state_ == VideoPlaybackState::Playing && current_index_ < clips_.size()
               ? &clips_[current_index_]
               : nullptr;
}

void NativeVideoPlayer::advance() {
    ++current_index_;
    current_time_ = 0.0;
    if (current_index_ >= clips_.size()) state_ = VideoPlaybackState::Finished;
}

}  // namespace f2
