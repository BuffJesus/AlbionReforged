#include "f2/native_ui.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

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
    if (key == "ambient_atlas" || key == "ambientatlas") return NativeUiAsset::AmbientAtlas;
    if (key == "ambient_baseline" || key == "ambientbaseline") {
        return NativeUiAsset::AmbientBaseline;
    }
    if (key == "ambient_detail" || key == "ambientdetail") {
        return NativeUiAsset::AmbientDetail;
    }
    if (key == "frame_elements" || key == "frameelements" || key == "frames_elements") {
        return NativeUiAsset::FrameElements;
    }
    if (key == "ability_elements" || key == "abilityelements") {
        return NativeUiAsset::AbilityElements;
    }
    if (key == "menu_frame" || key == "menu_frame_overlay" || key == "menuframe") {
        return NativeUiAsset::MenuFrameOverlay;
    }
    if (key == "frames_04" || key == "frames04") return NativeUiAsset::Frames04;
    if (key == "card_boy" || key == "cardboy") return NativeUiAsset::CardBoy;
    if (key == "card_girl" || key == "cardgirl") return NativeUiAsset::CardGirl;
    if (key == "card_back" || key == "cardback") return NativeUiAsset::CardBack;
    if (key == "side_rail_atlas" || key == "siderailatlas") {
        return NativeUiAsset::SideRailAtlas;
    }
    if (key == "menu_frame_left_upper" || key == "menuframeleftupper") {
        return NativeUiAsset::MenuFrameLeftUpper;
    }
    if (key == "menu_frame_left_lower" || key == "menuframeleftlower") {
        return NativeUiAsset::MenuFrameLeftLower;
    }
    if (key == "menu_frame_right_upper" || key == "menuframerightupper") {
        return NativeUiAsset::MenuFrameRightUpper;
    }
    if (key == "menu_frame_right_lower" || key == "menuframerightlower") {
        return NativeUiAsset::MenuFrameRightLower;
    }
    if (key == "frames_page" || key == "framespage") return NativeUiAsset::FramesPage;
    if (key == "frames_page_texture" || key == "framespagetexture") {
        return NativeUiAsset::FramesPageTexture;
    }
    if (key == "slider_frame" || key == "sliderframe") return NativeUiAsset::SliderFrame;
    if (key == "motifs") return NativeUiAsset::Motifs;
    if (key == "calibration_image" || key == "calibrationimage") {
        return NativeUiAsset::CalibrationImage;
    }
    if (key == "gold_coin" || key == "goldcoin" || key == "icon_gold_coin") {
        return NativeUiAsset::GoldCoin;
    }
    return std::nullopt;
}

}  // namespace

bool NativeUiAssets::load(const std::filesystem::path& root, std::string& error) {
    root_ = root;
    title_font_path_.clear();
    loaded_.fill(false);
    textures_ = {};
    ambient_detail_frames_.clear();
    if (root_.empty() || !std::filesystem::is_directory(root_)) return true;

    std::array<std::filesystem::path, static_cast<std::size_t>(NativeUiAsset::Count)> paths{};
    std::filesystem::path title_font;
    std::string detail_frames_value;
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
        if (key == "title_font" || key == "titlefont") {
            title_font = value;
            continue;
        }
        if (key == "ambient_detail_frames" || key == "ambientdetailframes") {
            detail_frames_value = value;
            continue;
        }
        if (const auto asset = asset_from_key(key)) paths[static_cast<std::size_t>(*asset)] = value;
    }

    if (title_font.empty()) title_font = "title_font.ttf";
    const auto resolved_title_font = title_font.is_absolute() ? title_font : root_ / title_font;
    if (std::filesystem::is_regular_file(resolved_title_font)) {
        title_font_path_ = resolved_title_font;
    }

    const std::array<std::pair<NativeUiAsset, std::string_view>, 36> defaults = {{
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
        {NativeUiAsset::AmbientAtlas, "ambient_atlas.png"},
        {NativeUiAsset::AmbientBaseline, "ambient_baseline.png"},
        {NativeUiAsset::AmbientDetail, "ambient_detail.png"},
        {NativeUiAsset::FrameElements, "frames_elements.png"},
        {NativeUiAsset::AbilityElements, "ability_elements.png"},
        {NativeUiAsset::MenuFrameOverlay, "menu_frame_overlay.png"},
        {NativeUiAsset::Frames04, "frames_04.dds"},
        {NativeUiAsset::CardBoy, "card_boy.png"},
        {NativeUiAsset::CardGirl, "card_girl.png"},
        {NativeUiAsset::CardBack, "card_back.png"},
        {NativeUiAsset::SideRailAtlas, "side_rail_atlas.png"},
        {NativeUiAsset::MenuFrameLeftUpper, "menu_frame_left_upper.dds"},
        {NativeUiAsset::MenuFrameLeftLower, "menu_frame_left_lower.dds"},
        {NativeUiAsset::MenuFrameRightUpper, "menu_frame_right_upper.dds"},
        {NativeUiAsset::MenuFrameRightLower, "menu_frame_right_lower.dds"},
        {NativeUiAsset::FramesPage, "frames_page.png"},
        {NativeUiAsset::FramesPageTexture, "frames_page_texture.png"},
        {NativeUiAsset::SliderFrame, "sliderframe.png"},
        {NativeUiAsset::Motifs, "motifs.png"},
        {NativeUiAsset::LogoFlare, "fe_logo_ambient.png"},
        {NativeUiAsset::CalibrationImage, "CalibrationImage.dds"},
        {NativeUiAsset::GoldCoin, "icon_gold_coin.dds"},
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

    if (!detail_frames_value.empty()) {
        std::size_t start = 0;
        while (start < detail_frames_value.size()) {
            const auto separator = detail_frames_value.find_first_of(",;", start);
            const auto token = trim(detail_frames_value.substr(
                start, separator == std::string::npos ? std::string::npos : separator - start));
            if (!token.empty()) {
                const auto path = std::filesystem::path(token).is_absolute()
                                      ? std::filesystem::path(token)
                                      : root_ / token;
                NativeTexture frame;
                std::string frame_error;
                if (load_image_rgba8(path, frame, frame_error)) {
                    ambient_detail_frames_.push_back(std::move(frame));
                } else if (first_error.empty()) {
                    first_error = std::move(frame_error);
                }
            }
            if (separator == std::string::npos) break;
            start = separator + 1;
        }
    }
    if (!first_error.empty()) {
        error = std::move(first_error);
        return false;
    }

    // The captured pixel shader combines the main atlas and detail sampler
    // as detail.rgb * c47.x + detail.a * main.rgb, with
    // out.a = main.a * detail.a. The representative capture has c47.x=1.
    // Fold an optional user-provided static detail snapshot into the atlas so
    // both native backends consume the same material result. The retail title
    // streams changing detail textures; animated replacement remains a later
    // step, while this path is exact for the supplied snapshot.
    const auto atlas_index = static_cast<std::size_t>(NativeUiAsset::AmbientAtlas);
    const auto detail_index = static_cast<std::size_t>(NativeUiAsset::AmbientDetail);
    if (ambient_detail_frames_.empty() && loaded_[atlas_index] && loaded_[detail_index]) {
        NativeTexture composed;
        if (compose_native_ambient_atlas(textures_[atlas_index], textures_[detail_index],
                                         composed)) {
            textures_[atlas_index] = std::move(composed);
        }
    }
    return true;
}

const NativeTexture* NativeUiAssets::texture(NativeUiAsset asset) const noexcept {
    const auto index = static_cast<std::size_t>(asset);
    return index < loaded_.size() && loaded_[index] ? &textures_[index] : nullptr;
}

const NativeTexture* NativeUiAssets::ambient_detail_frame(std::size_t index) const noexcept {
    return index < ambient_detail_frames_.size() ? &ambient_detail_frames_[index] : nullptr;
}

bool NativeUiAssets::has(NativeUiAsset asset) const noexcept {
    return texture(asset) != nullptr;
}

bool compose_native_ambient_atlas(const NativeTexture& main,
                                  const NativeTexture& detail,
                                  NativeTexture& output) {
    if (main.width == 0 || main.height == 0 || main.width != detail.width ||
        main.height != detail.height || main.rgba8.size() !=
            static_cast<std::size_t>(main.width) * main.height * 4 ||
        detail.rgba8.size() != static_cast<std::size_t>(detail.width) * detail.height * 4) {
        return false;
    }
    output = main;
    for (std::size_t offset = 0; offset + 3 < output.rgba8.size(); offset += 4) {
        const float detail_alpha = static_cast<float>(detail.rgba8[offset + 3]) / 255.0f;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float value = static_cast<float>(detail.rgba8[offset + channel]) +
                                detail_alpha * static_cast<float>(main.rgba8[offset + channel]);
            output.rgba8[offset + channel] = static_cast<std::uint8_t>(
                std::clamp(value, 0.0f, 255.0f));
        }
        output.rgba8[offset + 3] = static_cast<std::uint8_t>(std::clamp(
            detail_alpha * static_cast<float>(main.rgba8[offset + 3]), 0.0f, 255.0f));
    }
    return true;
}

}  // namespace f2
