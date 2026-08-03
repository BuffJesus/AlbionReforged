#include "f2/native_game.h"

#include <chrono>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: f2native_probe <scene.f2scene>\n";
        return 2;
    }

    f2::NativeGame game;
    std::string error;
    if (!game.load_scene(argv[1], error)) {
        std::cerr << "f2native: " << error << "\n";
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    game.tick(1.0 / 60.0);
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "Fable2Native scene loaded\n"
              << "  meshes: " << game.scene.meshes.size() << "\n"
              << "  instances: " << game.scene.instances.size() << "\n"
              << "  simulation_seconds: " << game.elapsed_seconds << "\n"
              << "  load_tick_seconds: " << elapsed << "\n";
    return 0;
}
