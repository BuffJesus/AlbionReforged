#include "f2/native_game.h"

#include <algorithm>

namespace f2 {

bool NativeGame::load_scene(const std::filesystem::path& path, std::string& error) {
    elapsed_seconds = 0.0;
    camera = NativeCamera{};
    frontend.reset();
    return load_native_scene(path, scene, error);
}

void NativeGame::tick(double delta_seconds) {
    delta_seconds = std::clamp(delta_seconds, 0.0, 0.25);
    elapsed_seconds += delta_seconds;
    frontend.tick(delta_seconds);
}

}  // namespace f2
