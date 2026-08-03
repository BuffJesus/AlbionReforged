#include "f2/native_install.h"
#include "f2/native_iso.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::filesystem::path iso;
    std::filesystem::path output;
    for (int index = 1; index + 1 < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--iso") iso = argv[++index];
        else if (option == "--out") output = argv[++index];
    }
    if (iso.empty() || output.empty()) {
        std::cerr << "Usage: f2native_installer --iso game.iso --out install-directory\n";
        return 2;
    }

    std::string error;
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

    if (const auto source = f2::detect_game_source(output, error)) {
        std::cout << "Fable II source installed at " << source->root << '\n';
        return 0;
    }
    std::cerr << "Installation completed but validation failed: " << error << '\n';
    return 1;
}
