#pragma once

#include "native_install.h"

#include <filesystem>
#include <string>

namespace f2 {

// Options for the install-time cook step. The cookers are the project's offline tools
// (tools/cook_*.py); they read the user's base-game data and write a user-local native
// package. Nothing copyrighted is ever shipped — only the cookers themselves.
struct CookOptions {
    std::filesystem::path tools_dir;          // dir with cook_*.py (default: F2NATIVE_TOOLS_DIR or <exe>/tools)
    std::filesystem::path python = "python";  // python launcher (PATH-resolved)
    std::filesystem::path ffmpeg = "ffmpeg";  // ffmpeg for video + xma2 audio (PATH-resolved)
    std::filesystem::path cook_lh_tex;        // f2native_cook_lh_tex.exe (default: next to this exe)
    bool cook_videos = true;
    bool cook_gui_audio = true;
    bool cook_gui_textures = true;
};

struct CookReport {
    bool videos_ok = false;
    bool gui_audio_ok = false;
    bool textures_ok = false;
    std::string log;  // combined cooker stdout/stderr, for diagnostics

    [[nodiscard]] bool all_ok() const noexcept { return videos_ok && gui_audio_ok && textures_ok; }
};

// Cook the user's base-game data (from `source`) into a user-local native package at
// `package_dir` (videos/ + native_audio/). Returns true if every requested cooker succeeded.
// The runtime is then launched with --ui-root <package_dir> (video/audio roots default under it).
bool cook_native_package(const GameSource& source,
                         const std::filesystem::path& package_dir,
                         const CookOptions& options,
                         CookReport& report);

// Default cooker directory: the F2NATIVE_TOOLS_DIR compile definition if set (dev builds),
// otherwise <executable-dir>/tools (a shipped installer bundles the cookers next to itself).
[[nodiscard]] std::filesystem::path default_tools_dir();

}  // namespace f2
