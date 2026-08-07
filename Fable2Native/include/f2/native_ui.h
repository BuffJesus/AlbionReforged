#pragma once

#include "f2/native_texture.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace f2 {

enum class NativeUiAsset : std::uint8_t {
    TitleBackground,
    MainBackground,
    Logo,
    Accept,
    Back,
    MenuSurface,
    Sparkle1,
    Sparkle2,
    Sparkle3,
    Sparkle4,
    Sparkle5,
    Sparkle6,
    Sparkle7,
    Sparkle8,
    AmbientAtlas,
    AmbientBaseline,
    AmbientDetail,
    FrameElements,
    AbilityElements,
    MenuFrameOverlay,
    Frames04,
    CardBoy,
    CardGirl,
    CardBack,
    SideRailAtlas,
    MenuFrameLeftUpper,
    MenuFrameLeftLower,
    MenuFrameRightUpper,
    MenuFrameRightLower,
    FramesPage,
    FramesPageTexture,
    SliderFrame,
    Motifs,
    CalibrationImage,
    GoldCoin,
    Count,
};

class NativeUiAssets {
public:
    bool load(const std::filesystem::path& root, std::string& error);

    [[nodiscard]] const NativeTexture* texture(NativeUiAsset asset) const noexcept;
    [[nodiscard]] bool has(NativeUiAsset asset) const noexcept;
    [[nodiscard]] std::size_t ambient_detail_frame_count() const noexcept {
        return ambient_detail_frames_.size();
    }
    [[nodiscard]] const NativeTexture* ambient_detail_frame(std::size_t index) const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] const std::filesystem::path& title_font_path() const noexcept {
        return title_font_path_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path title_font_path_;
    std::array<NativeTexture, static_cast<std::size_t>(NativeUiAsset::Count)> textures_{};
    std::array<bool, static_cast<std::size_t>(NativeUiAsset::Count)> loaded_{};
    std::vector<NativeTexture> ambient_detail_frames_;
};

// Applies the recovered title pixel material:
// out.rgb = detail.rgb + detail.a * main.rgb, out.a = main.a * detail.a.
// The captured representative draw has c47.x = 1.
bool compose_native_ambient_atlas(const NativeTexture& main,
                                  const NativeTexture& detail,
                                  NativeTexture& output);

}  // namespace f2
