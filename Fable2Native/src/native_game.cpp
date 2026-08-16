#include "f2/native_game.h"

#include <algorithm>
#include <cmath>

namespace f2 {

namespace {
// Own-format save magic + version gate ("F2SV"). Bumped when the format changes.
constexpr std::uint32_t kSaveMagic = 0x46325356u;
}  // namespace

std::vector<std::uint8_t> NativeGame::save_state() {
    WorldArchive ar(ArchiveMode::Write);
    std::uint32_t magic = kSaveMagic;
    ar.visit(magic);
    game_state.hero_position = player.position();
    game_state.serialize(ar);   // header + quest bitfield + hero position
    world.serialize(ar);        // live entity delta
    return ar.take();
}

bool NativeGame::load_state(const std::vector<std::uint8_t>& data) {
    WorldArchive ar(data);
    std::uint32_t magic = 0;
    ar.visit(magic);
    if (magic != kSaveMagic) return false;
    game_state.serialize(ar);
    if (game_state.header.version != 1) return false;  // future-version gate
    player.set_position(game_state.hero_position);
    world.serialize(ar);  // overlay the delta onto the current (rebuilt) baseline
    return ar.ok();
}

bool NativeGame::load_scene(const std::filesystem::path& path, std::string& error) {
    elapsed_seconds = 0.0;
    simulation_accumulator_ = 0.0;
    camera = NativeCamera{};
    frontend.reset();
    if (!load_native_scene(path, scene, error)) {
        return false;
    }
    // Seed the live entity graph from the cooked baseline (one entity per instance,
    // Transform + GraphicAppearanceStaticMesh). Sim only advances it when InWorld.
    world.spawn_from_scene(scene);

    // Build static collision (AABBs + terrain heightfield) and place the hero at the
    // RE'd PlayerStart (SimpleTransformComponent). Camera starts behind the hero's yaw.
    collision.build_from_scene(scene);
    std::array<float, 3> start = scene.has_hero_start ? scene.hero_start
                                                      : std::array<float, 3>{0.0f, 0.0f, 0.0f};
    if (scene.has_hero_start) {
        const float gy = collision.sample_ground(start[0], start[2], start[1] + 2.0f);
        if (std::isfinite(gy)) start[1] = gy;
    }
    player = NativePlayer{};
    player.set_position(start);
    camera_controller.yaw = scene.hero_yaw;
    camera_controller.pitch = -0.3f;
    return true;
}

void NativeGame::tick(double delta_seconds) {
    delta_seconds = std::clamp(delta_seconds, 0.0, 0.25);

    // Input is sampled ONCE per frame, before the fixed-step accumulator: retail
    // input_pad_poll (@0x822B3A10) polls per frame, not per simulation sub-step.
    input_sampler.tick(input);

    constexpr double simulation_step = 1.0 / 60.0;
    simulation_accumulator_ += delta_seconds;
    while (simulation_accumulator_ >= simulation_step) {
        simulation_accumulator_ -= simulation_step;
        elapsed_seconds += simulation_step;

        // Master gameplay tick order. Only the script sub-order (Quest->General
        // ->AI, Function_82281FE0) is decomp-confirmed today; movement/camera
        // steps join in P2. The front-end always ticks (menus/loading overlays).
        frontend.tick(simulation_step);
        if (mode == GameMode::InWorld) {
            script_systems.tick(simulation_step);

            // Movement: camera-relative input -> desired velocity -> collide-and-slide.
            player.update(collision, input, camera_controller.yaw,
                          static_cast<float>(simulation_step));

            // Camera follows the moved hero (look from right-stick or mouse).
            if (camera_controller.mode == CameraMode::Follow) {
                const bool look_is_mouse = input.last_active_device == InputDevice::KeyboardMouse;
                camera_controller.update(player.position(), input.look, look_is_mouse,
                                         static_cast<float>(simulation_step), &collision);
                // Feed the follow pose into NativeCamera (the renderer reads pos/yaw/pitch).
                camera.position = camera_controller.position;
                camera.yaw = camera_controller.yaw;
                camera.pitch = camera_controller.pitch;
            }

            // Final step: push live entity transforms into the render scene.
            world.sync_to_scene(scene);
        }
    }
}

}  // namespace f2
