#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace f2 {

using InstallProgress = std::function<bool(std::uint64_t copied_bytes,
                                           std::uint64_t total_bytes)>;

// Minimal Xbox 360 XDVDFS reader used only by the user-data installer. The
// native game runtime never mounts or interprets an ISO after installation.
bool extract_xbox360_iso(const std::filesystem::path& iso_path,
                         const std::filesystem::path& output_root,
                         const InstallProgress& progress,
                         std::string& error);

}  // namespace f2
