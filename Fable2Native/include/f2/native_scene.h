#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f2 {

struct NativeVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
    std::array<float, 2> uv{};
};

struct NativeMaterial {
    std::string name;
    std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
    std::string albedo;
    std::string normal;
    std::string material;
};

struct NativeMesh {
    std::string name;
    std::vector<NativeVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t material = 0;
};

struct NativeInstance {
    std::uint32_t mesh = 0;
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
    float scale = 1.0f;
};

struct NativeScene {
    std::vector<NativeMaterial> materials;
    std::vector<NativeMesh> meshes;
    std::vector<NativeInstance> instances;
    std::array<float, 3> sun_direction{0.3f, -1.0f, 0.2f};
    std::array<float, 4> sky_color{0.35f, 0.48f, 0.68f, 1.0f};

    bool validate(std::string* error = nullptr) const;
};

bool load_native_scene(const std::filesystem::path& path,
                       NativeScene& scene,
                       std::string& error);

}  // namespace f2
