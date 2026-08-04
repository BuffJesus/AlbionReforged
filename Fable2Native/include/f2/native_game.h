#pragma once

#include "native_scene.h"
#include "native_frontend.h"

#include <array>
#include <filesystem>
#include <string>

namespace f2 {

struct NativeCamera {
    std::array<float, 3> position{0.0f, 2.0f, 6.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
};

struct NativeGame {
    NativeScene scene;
    NativeCamera camera;
    FrontendController frontend;
    double elapsed_seconds = 0.0;

    bool load_scene(const std::filesystem::path& path, std::string& error);
    void tick(double delta_seconds);

private:
    double simulation_accumulator_ = 0.0;
};

}  // namespace f2
