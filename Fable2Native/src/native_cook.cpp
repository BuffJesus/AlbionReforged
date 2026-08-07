#include "f2/native_cook.h"

#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace f2 {
namespace {

#ifdef _WIN32
std::wstring quote(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

// Run a child process, capturing merged stdout+stderr into `output`. Returns the exit code,
// or -1 if the process could not be launched.
int run_process(const std::wstring& command_line, const std::filesystem::path& working_dir,
                std::string& output) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return -1;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutable_command = command_line;  // CreateProcessW may modify the buffer.
    const std::wstring cwd = working_dir.empty() ? std::wstring() : working_dir.wstring();
    const BOOL created = CreateProcessW(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(write_pipe);  // parent keeps only the read end
    if (!created) {
        CloseHandle(read_pipe);
        return -1;
    }

    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) && read != 0) {
        output.append(buffer, read);
    }
    CloseHandle(read_pipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code);
}
#endif  // _WIN32

}  // namespace

std::filesystem::path default_tools_dir() {
#ifdef F2NATIVE_TOOLS_DIR
    std::error_code ec;
    if (std::filesystem::is_directory(F2NATIVE_TOOLS_DIR, ec)) return F2NATIVE_TOOLS_DIR;
#endif
#ifdef _WIN32
    wchar_t module_path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (length != 0 && length < MAX_PATH) {
        return std::filesystem::path(module_path).parent_path() / "tools";
    }
#endif
    return std::filesystem::current_path() / "tools";
}

bool cook_native_package(const GameSource& source, const std::filesystem::path& package_dir,
                         const CookOptions& options, CookReport& report) {
    report = CookReport{};
#ifndef _WIN32
    (void)source;
    (void)package_dir;
    (void)options;
    report.log = "install-time cooking is only implemented on Windows";
    return false;
#else
    std::error_code ec;
    std::filesystem::create_directories(package_dir, ec);
    const std::filesystem::path tools =
        options.tools_dir.empty() ? default_tools_dir() : options.tools_dir;

    const std::filesystem::path cook_videos = tools / "cook_videos.py";
    const std::filesystem::path cook_gui_audio = tools / "cook_gui_audio.py";
    const std::filesystem::path cook_gui_textures = tools / "cook_gui_textures.py";
    const std::filesystem::path video_input = source.data_root / "art" / "videos";

    // The LhTex .tex->DDS decoder ships next to this executable (built as f2native_cook_lh_tex).
    std::filesystem::path lh_tex = options.cook_lh_tex;
    if (lh_tex.empty()) {
        wchar_t module_path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
        if (length != 0 && length < MAX_PATH) {
            lh_tex = std::filesystem::path(module_path).parent_path() / "f2native_cook_lh_tex.exe";
        }
    }

    const auto run_cooker = [&](const std::filesystem::path& script, const std::wstring& args,
                                const char* label) -> bool {
        if (!std::filesystem::is_regular_file(script)) {
            report.log += std::string("[") + label + "] cooker not found: " + script.string() + "\n";
            return false;
        }
        std::wstring command = quote(options.python) + L" " + quote(script) + L" " + args;
        std::string output;
        report.log += std::string("[") + label + "] running\n";
        const int code = run_process(command, tools, output);
        report.log += output;
        if (code != 0) {
            report.log += std::string("[") + label + "] FAILED (exit " + std::to_string(code) + ")\n";
            return false;
        }
        report.log += std::string("[") + label + "] ok\n";
        return true;
    };

    if (options.cook_videos) {
        const std::wstring args = quote(video_input) + L" " + quote(package_dir) +
                                  L" --ffmpeg " + quote(options.ffmpeg) +
                                  L" --ffprobe " + quote(options.ffmpeg.parent_path().empty()
                                                             ? std::filesystem::path("ffprobe")
                                                             : options.ffmpeg.parent_path() / "ffprobe");
        report.videos_ok = run_cooker(cook_videos, args, "videos");
    } else {
        report.videos_ok = true;  // not requested → not a failure
    }

    if (options.cook_gui_audio) {
        const std::wstring args = quote(source.data_root) + L" " + quote(package_dir) +
                                  L" --ffmpeg " + quote(options.ffmpeg);
        report.gui_audio_ok = run_cooker(cook_gui_audio, args, "gui_audio");
    } else {
        report.gui_audio_ok = true;
    }

    if (options.cook_gui_textures) {
        if (!std::filesystem::is_regular_file(lh_tex)) {
            report.log += "[textures] LhTex cooker not found: " + lh_tex.string() + "\n";
            report.textures_ok = false;
        } else {
            const std::wstring args = quote(source.data_root) + L" " + quote(package_dir) +
                                      L" --cooker " + quote(lh_tex);
            report.textures_ok = run_cooker(cook_gui_textures, args, "textures");
        }
    } else {
        report.textures_ok = true;
    }

    return report.all_ok();
#endif
}

}  // namespace f2
