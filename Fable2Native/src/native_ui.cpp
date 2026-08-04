#include "f2/native_ui.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <ranges>
#include <string_view>

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

std::optional<NativeUiAsset> asset_from_key(std::string_view key) {
    if (key == "title_background") return NativeUiAsset::TitleBackground;
    if (key == "main_background") return NativeUiAsset::MainBackground;
    if (key == "logo") return NativeUiAsset::Logo;
    if (key == "accept" || key == "button_accept") return NativeUiAsset::Accept;
    if (key == "back" || key == "button_back") return NativeUiAsset::Back;
    if (key == "menu_surface" || key == "menu_texture") return NativeUiAsset::MenuSurface;
    if (key == "sparkle_1" || key == "sparkle1") return NativeUiAsset::Sparkle1;
    if (key == "sparkle_2" || key == "sparkle2") return NativeUiAsset::Sparkle2;
    if (key == "sparkle_3" || key == "sparkle3") return NativeUiAsset::Sparkle3;
    if (key == "sparkle_4" || key == "sparkle4") return NativeUiAsset::Sparkle4;
    if (key == "sparkle_5" || key == "sparkle5") return NativeUiAsset::Sparkle5;
    if (key == "sparkle_6" || key == "sparkle6") return NativeUiAsset::Sparkle6;
    if (key == "sparkle_7" || key == "sparkle7") return NativeUiAsset::Sparkle7;
    if (key == "sparkle_8" || key == "sparkle8") return NativeUiAsset::Sparkle8;
    return std::nullopt;
}

}  // namespace

bool NativeUiAssets::load(const std::filesystem::path& root, std::string& error) {
    root_ = root;
    loaded_.fill(false);
    textures_ = {};
    if (root_.empty() || !std::filesystem::is_directory(root_)) return true;

    std::array<std::filesystem::path, static_cast<std::size_t>(NativeUiAsset::Count)> paths{};
    const auto manifest = root_ / "ui_manifest.ini";
    std::ifstream input(manifest);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = lower(trim(line.substr(0, separator)));
        const auto value = trim(line.substr(separator + 1));
        if (const auto asset = asset_from_key(key)) paths[static_cast<std::size_t>(*asset)] = value;
    }

    const std::array<std::pair<NativeUiAsset, std::string_view>, 14> defaults = {{
        {NativeUiAsset::TitleBackground, "title_background.dds"},
        {NativeUiAsset::MainBackground, "main_background.dds"},
        {NativeUiAsset::Logo, "logo.dds"},
        {NativeUiAsset::Accept, "button_accept.dds"},
        {NativeUiAsset::Back, "button_back.dds"},
        {NativeUiAsset::MenuSurface, "menu_surface.dds"},
        {NativeUiAsset::Sparkle1, "sparkle_1.png"},
        {NativeUiAsset::Sparkle2, "sparkle_2.png"},
        {NativeUiAsset::Sparkle3, "sparkle_3.png"},
        {NativeUiAsset::Sparkle4, "sparkle_4.png"},
        {NativeUiAsset::Sparkle5, "sparkle_5.png"},
        {NativeUiAsset::Sparkle6, "sparkle_6.png"},
        {NativeUiAsset::Sparkle7, "sparkle_7.png"},
        {NativeUiAsset::Sparkle8, "sparkle_8.png"},
    }};
    for (const auto& [asset, filename] : defaults) {
        auto& path = paths[static_cast<std::size_t>(asset)];
        if (path.empty()) path = filename;
    }
    if (paths[static_cast<std::size_t>(NativeUiAsset::MainBackground)].filename() ==
        "main_background.dds" &&
        paths[static_cast<std::size_t>(NativeUiAsset::TitleBackground)].filename() !=
        "title_background.dds") {
        paths[static_cast<std::size_t>(NativeUiAsset::MainBackground)] =
            paths[static_cast<std::size_t>(NativeUiAsset::TitleBackground)];
    }

    std::string first_error;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        const auto path = paths[index].is_absolute() ? paths[index] : root_ / paths[index];
        if (!std::filesystem::is_regular_file(path)) continue;
        std::string asset_error;
        if (load_image_rgba8(path, textures_[index], asset_error)) {
            loaded_[index] = true;
        } else if (first_error.empty()) {
            first_error = std::move(asset_error);
        }
    }
    if (!first_error.empty()) {
        error = std::move(first_error);
        return false;
    }
    return true;
}

const NativeTexture* NativeUiAssets::texture(NativeUiAsset asset) const noexcept {
    const auto index = static_cast<std::size_t>(asset);
    return index < loaded_.size() && loaded_[index] ? &textures_[index] : nullptr;
}

bool NativeUiAssets::has(NativeUiAsset asset) const noexcept {
    return texture(asset) != nullptr;
}

}  // namespace f2
