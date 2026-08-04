#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f2 {

struct NativeVideoFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t serial = 0;
    std::vector<std::uint8_t> rgba8;
};

bool start_native_video_runtime(std::string& error);
void stop_native_video_runtime();

class NativeVideoDecoder {
public:
    ~NativeVideoDecoder();

    bool open(const std::filesystem::path& path, std::string& error);
    bool read_next_frame(NativeVideoFrame& frame, std::string& error);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return reader_ != nullptr; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] double frame_duration_seconds() const noexcept { return frame_duration_seconds_; }

private:
    enum class PixelFormat : std::uint8_t {
        Argb32,
        Rgb32,
        Yuy2,
    };

    void* reader_ = nullptr;
    std::filesystem::path path_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::int32_t stride_ = 0;
    PixelFormat pixel_format_ = PixelFormat::Argb32;
    double frame_duration_seconds_ = 1.0 / 30.0;
    std::uint64_t serial_ = 0;
};

}  // namespace f2
