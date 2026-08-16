#pragma once

#include "native_scene.h"
#include "native_frontend.h"
#include "native_input_state.h"
#include "native_script_systems.h"
#include "native_world.h"
#include "native_physics.h"
#include "native_camera.h"
#include "native_player.h"
#include "native_save.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace f2 {

struct NativeCamera {
    std::array<float, 3> position{0.0f, 2.0f, 6.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
};

// Whether the sim is idling in the front-end (menus/loading) or running the
// live world. Gameplay systems only tick InWorld. (The follow-camera controller
// and player/collision land in P2; free-fly NativeCamera stays until then.)
enum class GameMode : std::uint8_t {
    Frontend,
    InWorld,
};

struct NativeGame {
    NativeScene scene;
    NativeCamera camera;
    FrontendController frontend;
    GameMode mode = GameMode::Frontend;

    // Gameplay input (analog + KB&M + last-active-device); sampled once per frame.
    InputState input;
    InputSampler input_sampler;

    // Master gameplay tick driver (Quest -> General -> AI, retail-recovered order).
    ScriptSystems script_systems;

    // Live entity graph seeded from the cooked scene; pushes transforms into
    // scene.instances[] each InWorld tick.
    NativeWorld world;

    // P2 gameplay: static collision built from the scene, the controllable hero, and
    // the follow camera. Active only InWorld (CameraMode::Free restores the free-fly cam).
    NativeCollisionWorld collision;
    NativePlayer player;
    CameraController camera_controller;

    // Persisted game-flow state (chapter header + 150-bit quest completion + hero pos).
    NativeGameState game_state;

    double elapsed_seconds = 0.0;

    bool load_scene(const std::filesystem::path& path, std::string& error);
    void tick(double delta_seconds);

    // Own-format save/restore (gamestate_save_restore.txt model; see native_save.h).
    // save_state serializes the game-flow state + the live entity delta into a byte blob.
    // load_state overlays a blob onto the CURRENT world — the scene must already be loaded
    // (load_scene rebuilds the baseline), then the delta is applied. Returns false on a
    // bad magic/version or a truncated blob.
    [[nodiscard]] std::vector<std::uint8_t> save_state();
    [[nodiscard]] bool load_state(const std::vector<std::uint8_t>& data);

private:
    double simulation_accumulator_ = 0.0;
};

}  // namespace f2
