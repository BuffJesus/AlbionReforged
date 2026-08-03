#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace f2 {

enum class GameSourceKind {
    ExtractedDirectory,
    IsoImage,
};

struct GameSource {
    GameSourceKind kind = GameSourceKind::ExtractedDirectory;
    std::filesystem::path root;
    std::filesystem::path data_root;
};

// The native executable never ships or discovers project-local game assets.
// It accepts an extracted game directory and validates the files needed by the
// offline cooker. ISO files are recognized as acquisition input, but must be
// mounted or extracted before the runtime can consume them.
[[nodiscard]] std::optional<GameSource> detect_game_source(
    const std::filesystem::path& path, std::string& error);

}  // namespace f2
