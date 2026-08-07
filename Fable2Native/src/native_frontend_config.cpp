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
std::filesystem::path renderer_config_path() {
    wchar_t local_app_data[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(local_app_data) / "Fable2Native" / "renderer.txt";
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

}  // namespace f2
