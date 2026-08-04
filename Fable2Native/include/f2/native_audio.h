#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace f2 {

enum class NativeFrontendSound : std::uint8_t {
    Music,
    Navigate,
    Accept,
    Back,
};

class NativeFrontendAudio {
public:
    NativeFrontendAudio() = default;
    ~NativeFrontendAudio();

    bool initialise(const std::filesystem::path& root, std::string& error);
    void shutdown();
    void tick();
    void play(NativeFrontendSound sound);
    void set_music_enabled(bool enabled);

    [[nodiscard]] bool ready() const noexcept { return engine_ != nullptr; }

private:
    struct Clip {
        std::vector<std::uint8_t> bytes;
        std::uint16_t channels = 0;
        std::uint32_t sample_rate = 0;
        std::uint16_t bits_per_sample = 0;
    };

    struct Voice {
        IXAudio2SourceVoice* voice = nullptr;
        std::shared_ptr<Clip> clip;
        bool music = false;
    };

    std::shared_ptr<Clip> load_clip(const std::filesystem::path& path, std::string& error);
    void start_music();
    void stop_music();

    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* mastering_voice_ = nullptr;
    std::array<std::shared_ptr<Clip>, 4> clips_{};
    std::vector<Voice> voices_;
    std::filesystem::path root_;
    bool music_enabled_ = true;
};

}  // namespace f2
