#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace f2 {

// Frontend sound identities are keyed to the RETAIL sound-event names recovered from the
// XEX (docs/RETAIL_FRONTEND_SPEC.md §8; ghidra_out/frontend_sound_names.txt). The retail
// frontend fires these via 82371E80(out, sound-mgr, &SE_GUI_* lh_string). Each entry below
// maps 1:1 to the proven retail event; se_gui_event_name() returns that event string so the
// audio manifest and any future soundbank bridge can resolve by the real name.
enum class NativeFrontendSound : std::uint8_t {
    Music,           // frontend music bed (retail plays a menu ambience; native: menu_interlude)
    NavigateUp,      // SE_GUI_SLIDE_MENU_UP   — stick/keys move selection up
    NavigateDown,    // SE_GUI_SLIDE_MENU_DOWN — stick/keys move selection down
    SelectionLeft,   // SE_GUI_SELECTION_LEFT  — value/option decreased (options pages)
    SelectionRight,  // SE_GUI_SELECTION_RIGHT — value/option increased (options pages)
    Accept,          // SE_GUI_MENU_BOX_SELECT — A / confirm
    Back,            // SE_GUI_MENU_BOX_CANCEL — B / cancel
    Count,
};

// The retail SE_GUI_* event name each native sound stands in for (empty for Music, which is a
// bespoke ambience rather than a fired SE_GUI event).
[[nodiscard]] constexpr std::string_view se_gui_event_name(NativeFrontendSound sound) noexcept {
    switch (sound) {
        case NativeFrontendSound::NavigateUp: return "SE_GUI_SLIDE_MENU_UP";
        case NativeFrontendSound::NavigateDown: return "SE_GUI_SLIDE_MENU_DOWN";
        case NativeFrontendSound::SelectionLeft: return "SE_GUI_SELECTION_LEFT";
        case NativeFrontendSound::SelectionRight: return "SE_GUI_SELECTION_RIGHT";
        case NativeFrontendSound::Accept: return "SE_GUI_MENU_BOX_SELECT";
        case NativeFrontendSound::Back: return "SE_GUI_MENU_BOX_CANCEL";
        default: return {};
    }
}

class NativeFrontendAudio {
public:
    NativeFrontendAudio() = default;
    ~NativeFrontendAudio();

    bool initialise(const std::filesystem::path& root, std::string& error);
    void shutdown();
    void tick();
    void play(NativeFrontendSound sound);
    void set_music_enabled(bool enabled);

    // Play the soundtrack of an intro/attract movie (interleaved 16-bit PCM decoded by
    // NativeVideoDecoder). Replaces any movie audio already playing; stop_video_audio() ends it
    // when the movie is skipped or finishes. Independent of the SE_GUI one-shot/music voices.
    void play_video_audio(const std::vector<std::uint8_t>& pcm, std::uint16_t channels,
                          std::uint32_t sample_rate);
    void stop_video_audio();

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
    IXAudio2SourceVoice* video_voice_ = nullptr;
    std::shared_ptr<Clip> video_clip_;
    std::array<std::shared_ptr<Clip>, static_cast<std::size_t>(NativeFrontendSound::Count)> clips_{};
    std::vector<Voice> voices_;
    std::filesystem::path root_;
    bool music_enabled_ = true;
};

}  // namespace f2
