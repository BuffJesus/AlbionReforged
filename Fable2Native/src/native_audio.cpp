#include "f2/native_audio.h"

#ifdef _WIN32
#include <windows.h>
#include <xaudio2.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <ranges>

namespace f2 {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0] | (bytes[1] << 8) |
                                      (bytes[2] << 16) | (bytes[3] << 24));
}

}  // namespace

NativeFrontendAudio::~NativeFrontendAudio() { shutdown(); }

std::shared_ptr<NativeFrontendAudio::Clip>
NativeFrontendAudio::load_clip(const std::filesystem::path& path, std::string& error) {
#ifdef _WIN32
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    auto clip = std::make_shared<Clip>();
    clip->bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (clip->bytes.size() < 12 || std::memcmp(clip->bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(clip->bytes.data() + 8, "WAVE", 4) != 0) {
        error = "audio is not a RIFF/WAVE file: " + path.string();
        return {};
    }
    const std::uint8_t* format = nullptr;
    const std::uint8_t* data = nullptr;
    std::uint32_t data_size = 0;
    std::size_t cursor = 12;
    while (cursor + 8 <= clip->bytes.size()) {
        const auto* chunk = clip->bytes.data() + cursor;
        const auto size = read_u32(chunk + 4);
        const auto payload = cursor + 8;
        if (payload > clip->bytes.size()) break;
        const auto available = std::min<std::size_t>(size, clip->bytes.size() - payload);
        if (std::memcmp(chunk, "fmt ", 4) == 0 && available >= 16) format = chunk + 8;
        if (std::memcmp(chunk, "data", 4) == 0) {
            data = chunk + 8;
            data_size = static_cast<std::uint32_t>(available);
        }
        cursor = payload + size + (size & 1u);
    }
    if (!format || !data || read_u16(format) != 1 || read_u16(format + 2) == 0 ||
        read_u16(format + 14) != 16) {
        error = "audio must be uncompressed 16-bit PCM: " + path.string();
        return {};
    }
    clip->channels = read_u16(format + 2);
    clip->sample_rate = read_u32(format + 4);
    clip->bits_per_sample = read_u16(format + 14);
    std::vector<std::uint8_t> pcm(data, data + data_size);
    clip->bytes = std::move(pcm);
    return clip;
#else
    (void)path;
    error = "native frontend audio is only implemented on Windows";
    return {};
#endif
}

bool NativeFrontendAudio::initialise(const std::filesystem::path& root, std::string& error) {
#ifdef _WIN32
    shutdown();
    root_ = root;
    if (root_.empty() || !std::filesystem::is_directory(root_)) return true;
    if (FAILED(XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !engine_ ||
        FAILED(engine_->CreateMasteringVoice(&mastering_voice_))) {
        error = "XAudio2 could not create a mastering voice";
        shutdown();
        return false;
    }

    // Audio identities are keyed to the retail SE_GUI event names (native_audio.h). The runtime
    // NEVER ships a sound: it consumes a user-local cooked audio package produced from the user's
    // own game data (data/audio/gui.bnk + gui.adb → XMA → WAV). Each clip is resolved from
    // audio_manifest.ini in the cooked root, keyed by the SE_GUI event name (or a friendly alias).
    // Anything missing simply stays silent — there is no bundled fallback asset.
    constexpr std::size_t kCount = static_cast<std::size_t>(NativeFrontendSound::Count);
    std::array<std::filesystem::path, kCount> paths{};
    paths[static_cast<std::size_t>(NativeFrontendSound::Music)] = "menu_interlude.wav";
    paths[static_cast<std::size_t>(NativeFrontendSound::NavigateUp)] = "SE_GUI_SLIDE_MENU_UP.wav";
    paths[static_cast<std::size_t>(NativeFrontendSound::NavigateDown)] = "SE_GUI_SLIDE_MENU_DOWN.wav";
    paths[static_cast<std::size_t>(NativeFrontendSound::SelectionLeft)] = "SE_GUI_SELECTION_LEFT.wav";
    paths[static_cast<std::size_t>(NativeFrontendSound::SelectionRight)] = "SE_GUI_SELECTION_RIGHT.wav";
    paths[static_cast<std::size_t>(NativeFrontendSound::Accept)] = "SE_GUI_MENU_BOX_SELECT.wav";
    paths[static_cast<std::size_t>(NativeFrontendSound::Back)] = "SE_GUI_MENU_BOX_CANCEL.wav";

    // Map a manifest key (lowercased) to a clip slot. Accepts the canonical SE_GUI event name, a
    // friendly alias, and legacy category keys (navigate/scroll → both up & down, etc.).
    const auto slot_for_key = [](const std::string& key) -> std::vector<std::size_t> {
        using S = NativeFrontendSound;
        auto idx = [](S s) { return static_cast<std::size_t>(s); };
        if (key == "music") return {idx(S::Music)};
        if (key == "navigate_up" || key == "se_gui_slide_menu_up") return {idx(S::NavigateUp)};
        if (key == "navigate_down" || key == "se_gui_slide_menu_down") return {idx(S::NavigateDown)};
        if (key == "selection_left" || key == "se_gui_selection_left") return {idx(S::SelectionLeft)};
        if (key == "selection_right" || key == "se_gui_selection_right") return {idx(S::SelectionRight)};
        if (key == "accept" || key == "select" || key == "se_gui_menu_box_select") return {idx(S::Accept)};
        if (key == "back" || key == "exit" || key == "cancel" || key == "se_gui_menu_box_cancel")
            return {idx(S::Back)};
        if (key == "navigate" || key == "scroll") return {idx(S::NavigateUp), idx(S::NavigateDown)};
        if (key == "selection") return {idx(S::SelectionLeft), idx(S::SelectionRight)};
        return {};
    };

    const auto manifest = root_ / "audio_manifest.ini";
    std::ifstream input(manifest);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = lower(trim(line.substr(0, separator)));
        const auto value = trim(line.substr(separator + 1));
        for (const auto index : slot_for_key(key)) paths[index] = value;
    }
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (paths[index].empty()) continue;
        const auto path = paths[index].is_absolute() ? paths[index] : root_ / paths[index].filename();
        if (!std::filesystem::is_regular_file(path)) continue;
        clips_[index] = load_clip(path, error);
        if (!clips_[index] && !error.empty()) {
            error.clear();
        }
    }
    return true;
#else
    (void)root;
    error = "native frontend audio is only implemented on Windows";
    return false;
#endif
}

void NativeFrontendAudio::start_music() {
#ifdef _WIN32
    if (!music_enabled_ || !clips_[0] || !engine_) return;
    for (const auto& voice : voices_) if (voice.music) return;
    const auto& clip = clips_[0];
    WAVEFORMATEX format{WAVE_FORMAT_PCM, clip->channels, clip->sample_rate,
                        static_cast<DWORD>(clip->sample_rate * clip->channels * 2),
                        static_cast<WORD>(clip->channels * 2), 16, 0};
    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(engine_->CreateSourceVoice(&voice, &format))) return;
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(clip->bytes.size());
    buffer.pAudioData = clip->bytes.data();
    buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
        voice->DestroyVoice();
        return;
    }
    voices_.push_back({voice, clip, true});
#endif
}

void NativeFrontendAudio::stop_music() {
#ifdef _WIN32
    for (auto it = voices_.begin(); it != voices_.end();) {
        if (!it->music) {
            ++it;
            continue;
        }
        it->voice->Stop();
        it->voice->DestroyVoice();
        it = voices_.erase(it);
    }
#endif
}

void NativeFrontendAudio::set_music_enabled(bool enabled) {
    music_enabled_ = enabled;
    if (music_enabled_) start_music();
    else stop_music();
}

void NativeFrontendAudio::play(NativeFrontendSound sound) {
#ifdef _WIN32
    if (sound == NativeFrontendSound::Music) {
        start_music();
        return;
    }
    const auto index = static_cast<std::size_t>(sound);
    if (index >= clips_.size() || !clips_[index] || !engine_) return;
    const auto& clip = clips_[index];
    WAVEFORMATEX format{WAVE_FORMAT_PCM, clip->channels, clip->sample_rate,
                        static_cast<DWORD>(clip->sample_rate * clip->channels * 2),
                        static_cast<WORD>(clip->channels * 2), 16, 0};
    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(engine_->CreateSourceVoice(&voice, &format))) return;
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(clip->bytes.size());
    buffer.pAudioData = clip->bytes.data();
    if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
        voice->DestroyVoice();
        return;
    }
    voices_.push_back({voice, clip, false});
#else
    (void)sound;
#endif
}

void NativeFrontendAudio::play_video_audio(const std::vector<std::uint8_t>& pcm,
                                           std::uint16_t channels, std::uint32_t sample_rate) {
#ifdef _WIN32
    stop_video_audio();
    if (!engine_ || pcm.empty() || channels == 0 || sample_rate == 0) return;
    // Own the PCM for the voice's lifetime — XAudio2 reads the buffer asynchronously.
    auto clip = std::make_shared<Clip>();
    clip->bytes = pcm;
    clip->channels = channels;
    clip->sample_rate = sample_rate;
    clip->bits_per_sample = 16;
    WAVEFORMATEX format{WAVE_FORMAT_PCM, channels, sample_rate,
                        static_cast<DWORD>(sample_rate * channels * 2),
                        static_cast<WORD>(channels * 2), 16, 0};
    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(engine_->CreateSourceVoice(&voice, &format))) return;
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(clip->bytes.size());
    buffer.pAudioData = clip->bytes.data();
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
        voice->DestroyVoice();
        return;
    }
    video_voice_ = voice;
    video_clip_ = std::move(clip);
#else
    (void)pcm;
    (void)channels;
    (void)sample_rate;
#endif
}

void NativeFrontendAudio::stop_video_audio() {
#ifdef _WIN32
    if (video_voice_) {
        video_voice_->Stop();
        video_voice_->FlushSourceBuffers();
        video_voice_->DestroyVoice();
        video_voice_ = nullptr;
    }
    video_clip_.reset();
#endif
}

void NativeFrontendAudio::tick() {
#ifdef _WIN32
    for (auto it = voices_.begin(); it != voices_.end();) {
        if (it->music) {
            ++it;
            continue;
        }
        XAUDIO2_VOICE_STATE state{};
        it->voice->GetState(&state);
        if (state.BuffersQueued != 0) {
            ++it;
            continue;
        }
        it->voice->DestroyVoice();
        it = voices_.erase(it);
    }
#endif
}

void NativeFrontendAudio::shutdown() {
#ifdef _WIN32
    stop_video_audio();
    for (auto& voice : voices_) {
        if (voice.voice) {
            voice.voice->Stop();
            voice.voice->DestroyVoice();
        }
    }
    voices_.clear();
    if (mastering_voice_) mastering_voice_->DestroyVoice();
    mastering_voice_ = nullptr;
    if (engine_) engine_->Release();
    engine_ = nullptr;
#endif
    clips_ = {};
}

}  // namespace f2
