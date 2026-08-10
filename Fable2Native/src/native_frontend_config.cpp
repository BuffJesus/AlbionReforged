#include "f2/native_frontend_config.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace f2 {
namespace {

#ifdef _WIN32
std::filesystem::path config_dir() {
    // FABLE2NATIVE_CONFIG_DIR overrides the location (portable config; also isolates unit tests).
    wchar_t override_dir[MAX_PATH];
    const DWORD override_length =
        GetEnvironmentVariableW(L"FABLE2NATIVE_CONFIG_DIR", override_dir, MAX_PATH);
    if (override_length > 0 && override_length < MAX_PATH) {
        return std::filesystem::path(override_dir);
    }
    wchar_t local_app_data[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(local_app_data) / "Fable2Native";
}

std::filesystem::path renderer_config_path() {
    const auto dir = config_dir();
    return dir.empty() ? std::filesystem::path{} : dir / "renderer.txt";
}

std::filesystem::path options_config_path() {
    const auto dir = config_dir();
    return dir.empty() ? std::filesystem::path{} : dir / "options.ini";
}
#endif

RenderBackend backend_from_token(std::wstring token) {
    for (auto& c : token) c = static_cast<wchar_t>(std::towlower(c));
    if (token == L"vulkan" || token == L"vk") return RenderBackend::Vulkan;
    return RenderBackend::D3D12;
}

}  // namespace

const char* render_backend_name(RenderBackend backend) noexcept {
    return backend == RenderBackend::Vulkan ? "Vulkan" : "D3D12";
}

RenderBackend read_render_backend_preference() {
#ifdef _WIN32
    const auto path = renderer_config_path();
    if (path.empty()) return RenderBackend::D3D12;
    std::wifstream input(path);
    std::wstring value;
    if (input && std::getline(input, value)) {
        while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n' ||
                                  std::iswspace(value.back()))) {
            value.pop_back();
        }
        return backend_from_token(value);
    }
#endif
    return RenderBackend::D3D12;
}

void save_render_backend_preference(RenderBackend backend) {
#ifdef _WIN32
    const auto path = renderer_config_path();
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::wofstream output(path);
    if (output) output << (backend == RenderBackend::Vulkan ? L"vulkan" : L"d3d12") << L'\n';
#else
    (void)backend;
#endif
}

RenderBackend resolve_render_backend(const wchar_t* command_line) {
    if (command_line) {
        const std::wstring line(command_line);
        const std::wstring flag = L"--backend";
        auto pos = line.find(flag);
        if (pos != std::wstring::npos) {
            pos += flag.size();
            while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'=')) ++pos;
            std::wstring token;
            while (pos < line.size() && line[pos] != L' ') token.push_back(line[pos++]);
            if (!token.empty()) return backend_from_token(token);
        }
    }
    return read_render_backend_preference();
}

FrontendOptions read_options() {
    FrontendOptions options;
#ifdef _WIN32
    const auto path = options_config_path();
    if (path.empty()) return options;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = line.substr(0, separator);
        int value = 0;
        try {
            value = std::stoi(line.substr(separator + 1));
        } catch (...) {
            continue;
        }
        if (key == "subtitles") options.subtitles = value != 0;
        else if (key == "tutorials") options.tutorials = value != 0;
        else if (key == "multiplayer_orbs") options.multiplayer_orbs = value != 0;
        else if (key == "auto_joinable") options.auto_joinable = value != 0;
        else if (key == "invert_aim") options.invert_aim = value != 0;
        else if (key == "breadcrumb_size") options.breadcrumb_size = value;
        else if (key == "gamma_percent") options.gamma_percent = value;
        else if (key == "resolution_index") options.resolution_index = value;
        else if (key == "anti_aliasing_index") options.anti_aliasing_index = value;
        else if (key == "fps_display") options.fps_display = value != 0;
        else if (key == "sounds_volume") options.sounds_volume = value;
        else if (key == "music_volume") options.music_volume = value;
        else if (key == "voice_volume") options.voice_volume = value;
        else if (key == "speaker_mode") options.speaker_mode = value;
    }
#endif
    return options;
}

void save_options(const FrontendOptions& options) {
#ifdef _WIN32
    const auto path = options_config_path();
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path);
    if (!output) return;
    output << "subtitles=" << (options.subtitles ? 1 : 0) << '\n'
           << "tutorials=" << (options.tutorials ? 1 : 0) << '\n'
           << "multiplayer_orbs=" << (options.multiplayer_orbs ? 1 : 0) << '\n'
           << "auto_joinable=" << (options.auto_joinable ? 1 : 0) << '\n'
           << "invert_aim=" << (options.invert_aim ? 1 : 0) << '\n'
           << "breadcrumb_size=" << options.breadcrumb_size << '\n'
           << "gamma_percent=" << options.gamma_percent << '\n'
           << "resolution_index=" << options.resolution_index << '\n'
           << "anti_aliasing_index=" << options.anti_aliasing_index << '\n'
           << "fps_display=" << (options.fps_display ? 1 : 0) << '\n'
           << "sounds_volume=" << options.sounds_volume << '\n'
           << "music_volume=" << options.music_volume << '\n'
           << "voice_volume=" << options.voice_volume << '\n'
           << "speaker_mode=" << options.speaker_mode << '\n';
#else
    (void)options;
#endif
}

}  // namespace f2
