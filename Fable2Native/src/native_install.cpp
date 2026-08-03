#include "f2/native_install.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace f2 {

std::optional<GameSource> detect_game_source(const std::filesystem::path& path,
                                             std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "No game source was selected.";
        return std::nullopt;
    }

    std::error_code filesystem_error;
    const auto candidate = std::filesystem::weakly_canonical(path, filesystem_error);
    if (filesystem_error || !std::filesystem::exists(candidate)) {
        error = "The selected game source does not exist.";
        return std::nullopt;
    }

    if (std::filesystem::is_regular_file(candidate)) {
        auto extension = candidate.extension().string();
        std::ranges::transform(extension, extension.begin(),
                               [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        if (extension == ".iso") {
            error = "An ISO was selected. Mount or extract it, then select the extracted game directory.";
        } else {
            error = "Select the extracted game directory, not an individual file.";
        }
        return std::nullopt;
    }

    auto root = candidate;
    auto data_root = root / "data";
    if (!std::filesystem::is_directory(data_root) ||
        !std::filesystem::is_regular_file(data_root / "dir.manifest")) {
        if (std::filesystem::is_regular_file(root / "dir.manifest") &&
            std::filesystem::is_directory(root / "art")) {
            data_root = root;
            root = root.parent_path();
        } else {
            error = "This directory is not an extracted Fable II game source.";
            return std::nullopt;
        }
    }

    constexpr std::array required_files{
        "dir.manifest",
        "art/videos/microsoft_logo.bik",
        "art/videos/lionhead_logo.bik",
        "art/videos/intro.bik",
    };
    for (const auto* relative_path : required_files) {
        if (!std::filesystem::is_regular_file(data_root / relative_path)) {
            error = "The selected game source is missing data/" + std::string(relative_path) + ".";
            return std::nullopt;
        }
    }

    return GameSource{GameSourceKind::ExtractedDirectory, root, data_root};
}

}  // namespace f2
