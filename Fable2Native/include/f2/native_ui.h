#pragma once

#include "f2/native_texture.h"

#include <array>
#include <filesystem>
#include <string>

namespace f2 {

enum class NativeUiAsset : std::uint8_t {
    TitleBackground,
    MainBackground,
    Logo,
    Accept,
    Back,
    Count,
};

class NativeUiAssets {
public:
    bool load(const std::filesystem::path& root, std::string& error);

    [[nodiscard]] const NativeTexture* texture(NativeUiAsset asset) const noexcept;
    [[nodiscard]] bool has(NativeUiAsset asset) const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
    std::array<NativeTexture, static_cast<std::size_t>(NativeUiAsset::Count)> textures_{};
    std::array<bool, static_cast<std::size_t>(NativeUiAsset::Count)> loaded_{};
};

}  // namespace f2
