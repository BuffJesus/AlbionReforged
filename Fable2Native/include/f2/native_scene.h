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
    // Optional per-instance baked ambient (DC term of the .lmp LightmapFile SH probe).
    // has_ambient=false -> the renderer falls back to the global hemisphere ambient.
    bool has_ambient = false;
    std::array<float, 3> ambient{};
};

// A local point light (lamp post, lantern, brazier, placeable accent) — cooked from
// the level's .save/.gdb light entities (ghidra_out/level_lights_effects_re.txt §1).
// position = render space (game {x,z,y}); color = linear-ish RGB (source 0..255 / 255);
// range = falloff radius in world units; intensity = brightness multiplier.
struct NativeLight {
    std::array<float, 3> position{};
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    float range = 0.0f;
    float intensity = 1.0f;
};

struct NativeScene {
    std::vector<NativeMaterial> materials;
    std::vector<NativeMesh> meshes;
    std::vector<NativeInstance> instances;
    std::vector<NativeLight> lights;
    std::array<float, 3> sun_direction{0.3f, -1.0f, 0.2f};
    std::array<float, 4> sky_color{0.35f, 0.48f, 0.68f, 1.0f};

    bool validate(std::string* error = nullptr) const;
};

bool load_native_scene(const std::filesystem::path& path,
                       NativeScene& scene,
                       std::string& error);

// Write a NativeScene as a text F2SCENE package (the output stage of the level cooker; the exact
// format load_native_scene reads). Names (material/mesh) must be whitespace-free — the reader tokenizes
// them with >>. Returns false + fills error on an invalid scene or write failure.
bool save_native_scene(const std::filesystem::path& path,
                       const NativeScene& scene,
                       std::string& error);

}  // namespace f2
