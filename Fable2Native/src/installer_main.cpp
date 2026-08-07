#include "f2/native_install.h"
#include "f2/native_cook.h"
#include "f2/native_iso.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cerr <<
        "Usage:\n"
        "  f2native_installer --iso <game.iso> --out <install-dir> [cook options]\n"
        "  f2native_installer --game <extracted-dir> [--package <dir>] [cook options]\n"
        "\n"
        "  --iso <file>      Xbox 360 ISO to extract into <install-dir>, then cook.\n"
        "  --game <dir>      Use an already-extracted game directory (skips ISO extraction).\n"
        "  --out <dir>       Install/extraction destination (also the default package dir).\n"
        "  --package <dir>   Native package destination (default: <out|game>/native).\n"
        "  --skip-cook       Extract/validate only; do not cook the native package.\n"
        "  --no-videos       Skip the video cooker.\n"
        "  --no-audio        Skip the GUI-audio cooker.\n"
        "  --no-textures     Skip the GUI-texture cooker.\n"
        "  --tools <dir>     Directory containing the cook_*.py cookers.\n"
        "  --python <exe>    Python launcher (default: python).\n"
        "  --ffmpeg <exe>    ffmpeg executable (default: ffmpeg).\n"
        "\n"
        "The native runtime is launched with --ui-root <package-dir>; videos/ and\n"
        "native_audio/ default beneath it. No copyrighted asset is ever shipped: the\n"
        "cookers read the user's own base-game data.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path iso;
    std::filesystem::path game;
    std::filesystem::path output;
    std::filesystem::path package;
    bool skip_cook = false;
    f2::CookOptions cook;

    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        const auto next = [&]() -> std::string {
            return (index + 1 < argc) ? argv[++index] : std::string();
        };
        if (option == "--iso") iso = next();
        else if (option == "--game") game = next();
        else if (option == "--out") output = next();
        else if (option == "--package") package = next();
        else if (option == "--skip-cook") skip_cook = true;
        else if (option == "--no-videos") cook.cook_videos = false;
        else if (option == "--no-audio") cook.cook_gui_audio = false;
        else if (option == "--no-textures") cook.cook_gui_textures = false;
        else if (option == "--tools") cook.tools_dir = next();
        else if (option == "--python") cook.python = next();
        else if (option == "--ffmpeg") cook.ffmpeg = next();
        else {
            std::cerr << "Unknown option: " << option << "\n\n";
            print_usage();
            return 2;
        }
    }

    if (iso.empty() == game.empty()) {  // exactly one of --iso / --game is required
        print_usage();
        return 2;
    }

    std::string error;

    // 1. Acquire an extracted game directory (extract the ISO, or use --game as-is).
    if (!iso.empty()) {
        if (output.empty()) {
            std::cerr << "--iso requires --out <install-dir>\n";
            return 2;
        }
        const auto progress = [](std::uint64_t copied, std::uint64_t total) {
            static std::uint64_t last_report = 0;
            if (total != 0 && (copied == total || copied - last_report >= 64 * 1024 * 1024)) {
                last_report = copied;
                std::cout << "installed " << copied / (1024 * 1024) << " / "
                          << total / (1024 * 1024) << " MiB\n";
            }
            return true;
        };
        if (!f2::extract_xbox360_iso(iso, output, progress, error)) {
            std::cerr << "Installation failed: " << error << '\n';
            return 1;
        }
    } else {
        output = game;
    }

    // 2. Validate the extracted source.
    const auto source = f2::detect_game_source(output, error);
    if (!source) {
        std::cerr << "Validation failed: " << error << '\n';
        return 1;
    }
    std::cout << "Fable II source validated at " << source->root << '\n';

    if (skip_cook) {
        std::cout << "Cooking skipped (--skip-cook).\n";
        return 0;
    }

    // 3. Cook the user's base-game data into a user-local native package.
    const std::filesystem::path package_dir = package.empty() ? (output / "native") : package;
    std::cout << "Cooking native package into " << package_dir << " ...\n";
    f2::CookReport report;
    const bool cooked = f2::cook_native_package(*source, package_dir, cook, report);
    std::cout << report.log;
    std::cout << "  videos: " << (report.videos_ok ? "ok" : "FAILED")
              << "  gui-audio: " << (report.gui_audio_ok ? "ok" : "FAILED")
              << "  gui-textures: " << (report.textures_ok ? "ok" : "FAILED") << '\n';
    if (!cooked) {
        std::cerr << "Cooking incomplete. The native package may be missing assets.\n";
        return 1;
    }
    std::cout << "Native package ready. Launch the frontend with --ui-root " << package_dir << "\n";
    return 0;
}
