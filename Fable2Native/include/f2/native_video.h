#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace f2 {

struct VideoClip {
    std::string id;
    std::filesystem::path asset;
    double duration_seconds = 0.0;
    bool skippable = true;
};

enum class VideoPlaybackState {
    Stopped,
    Playing,
    Finished,
};

// Backend-neutral sequencing contract. A decoder presents frames through this
// contract; the frontend never needs to know whether the source was MP4,
// FFmpeg, Media Foundation, or a mod-provided replacement.
class NativeVideoPlayer {
public:
    bool set_sequence(std::vector<VideoClip> clips);
    void reset();
    void tick(double delta_seconds);
    void skip_current();
    void skip_all();

    [[nodiscard]] VideoPlaybackState state() const noexcept { return state_; }
    [[nodiscard]] bool finished() const noexcept {
        return state_ == VideoPlaybackState::Finished;
    }
    [[nodiscard]] std::size_t current_index() const noexcept { return current_index_; }
    [[nodiscard]] double current_time() const noexcept { return current_time_; }
    [[nodiscard]] const VideoClip* current_clip() const noexcept;
    [[nodiscard]] const std::vector<VideoClip>& sequence() const noexcept { return clips_; }

private:
    void advance();

    std::vector<VideoClip> clips_;
    std::size_t current_index_ = 0;
    double current_time_ = 0.0;
    VideoPlaybackState state_ = VideoPlaybackState::Stopped;
};

}  // namespace f2
