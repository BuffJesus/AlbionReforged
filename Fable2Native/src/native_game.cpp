#include "f2/native_game.h"

#include "f2/native_bindings.h"

#include <algorithm>
#include <cmath>

namespace f2 {

namespace {
// Own-format save magic + version gate ("F2SV"). Bumped when the format changes.
constexpr std::uint32_t kSaveMagic = 0x46325356u;
}  // namespace

bool NativeGame::enable_scripting() {
    script_vm = std::make_unique<NativeScriptVM>();
    if (!script_vm->valid()) {
        script_vm.reset();
        return false;
    }
    script_vm->set_user_data(this);

    // The gameplay native API (Debug/Game/Player/World/Quest/Camera/Health...). Everything
    // the game's scripts call beyond this falls through to the auto-stub (boot path).
    register_native_api(*script_vm, *this);

    // Wire the three managers to their Lua Update entry points, in the retail-recovered
    // order (Quest -> General -> AI). The Lua side owns its coroutine scheduling; the
    // native tick just resumes each manager's Update each InWorld step. call_global is a
    // no-op (returns false) until a loaded manager script defines the function.
    // Drive each manager's Lua Update in the retail order. Retail resumes the manager
    // OBJECT's Update METHOD (Manager:Update(dt)); fall back to a free *Update global (used
    // by simple mods/tests). The Lua side owns coroutine scheduling.
    NativeScriptVM* vm = script_vm.get();
    script_systems.quest.enabled = true;
    script_systems.quest.update = [vm](double dt) {
        if (!vm->call_method("QuestManager", "Update", dt)) vm->call_global("QuestUpdate", dt);
    };
    script_systems.general.enabled = true;
    script_systems.general.update = [vm](double dt) {
        if (!vm->call_method("GeneralScriptManager", "Update", dt)) vm->call_global("GeneralUpdate", dt);
    };
    script_systems.ai.enabled = true;
    script_systems.ai.update = [vm](double dt) {
        if (!vm->call_method("AIManager", "Update", dt)) vm->call_global("AIUpdate", dt);
    };
    return true;
}

int NativeGame::boot_game_scripts(const std::filesystem::path& data_root) {
    if (!script_vm) return -1;
    script_bnk = std::make_unique<BnkReader>();
    const auto bnk_path = data_root / "data" / "gamescripts_r.bnk";
    if (!script_bnk->open(bnk_path.string())) {
        script_bnk.reset();
        return -1;
    }
    loaded_scripts.clear();

    // Substrate natives BEFORE the scripts run: the manager-registration + entity + message
    // + gameflow natives the game's own Lua calls (register_game_systems_api), and RunScript
    // (register_boot_api). The auto-stub, installed LAST, swallows the still-unimplemented
    // long tail so quest/gameflow Lua runs without every native.
    register_game_systems_api(*script_vm, *this);
    register_boot_api(*script_vm, *this);

    // Run the real boot: generalsetupscript loads GeneralScriptManager + every enum/helper/
    // interactable and self-registers via SetGeneralScriptManager. (RunScript skips the
    // project's own MyConsoleHook0 mod-menu hook — not stock game logic; see register_boot_api.)
    std::vector<std::uint8_t> boot =
        script_bnk->extract("miscellaneous/generalsetupscript.lua");
    if (!boot.empty()) {
        script_vm->run_bytecode(boot.data(), boot.size(), "=generalsetupscript");
    }

    // Define the Platform enum consistently with GetPlatform() (Win32=2) before the auto-stub
    // claims `Platform` as a stub table. Values are arbitrary but self-consistent.
    script_vm->run_source("Platform = { Xbox360 = 1, Win32 = 2, PS3 = 3, PC = 2 }", "=platform");

    script_vm->install_autostub();

    // Wire the registered manager Update callbacks into the tick, retail Quest->General->AI
    // order. The managers are pure Lua; the native tick just resumes each one's Update.
    NativeScriptVM* vm = script_vm.get();
    NativeGame* self = this;
    script_systems.quest.enabled = true;
    script_systems.quest.update = [vm, self](double) {
        if (self->quest_update_ref >= 0) vm->call_ref(self->quest_update_ref);
    };
    script_systems.general.enabled = true;
    script_systems.general.update = [vm, self](double) {
        if (self->general_update_ref >= 0) vm->call_ref(self->general_update_ref);
    };
    script_systems.ai.enabled = true;
    script_systems.ai.update = [vm, self](double) {
        if (self->ai_update_ref >= 0) vm->call_ref(self->ai_update_ref);
    };

    return static_cast<int>(loaded_scripts.size());
}

int NativeGame::load_quest_scripts() {
    if (!script_vm || !script_bnk) return -1;
    // The quest bootstrap (quests/questsetupscript.lua) RunScripts QuestManager.lua + the
    // quest modules + gameflow. QuestManager.lua self-registers via SetQuestUpdateFunction.
    std::vector<std::uint8_t> qb = script_bnk->extract("quests/questsetupscript.lua");
    if (qb.empty()) return -1;
    // The quest scripts store their managers in a `BaseObjects` table but reference them as
    // globals (BaseObjects.QuestManager = {...} then `QuestManager.NewQuestThread(...)`), so
    // BaseObjects must alias _G. The engine sets this up before the quests bank runs.
    script_vm->run_source("if not BaseObjects then BaseObjects = _G end", "=baseobjects");

    // The save/load system defines helpers the quest machinery calls at registration time
    // (AddFunctionsInTableToPermanentsTables, via QuestManager.AddQuestToPermanentsTables).
    // The engine loads it in C++ (no Lua RunScript references it), so we load it here too,
    // before the quests bank. Best-effort: a failure just leaves those helpers to the stub.
    for (const char* dep : {"miscellaneous/saveload/saveloadsystem.lua"}) {
        std::vector<std::uint8_t> b = script_bnk->extract(dep);
        if (!b.empty()) script_vm->run_bytecode(b.data(), b.size(), "=saveload");
    }

    const std::size_t before = loaded_scripts.size();
    script_vm->run_bytecode(qb.data(), qb.size(), "=questsetupscript");

    // FLAGGED stand-in: neutralize the quest save/permanents registration
    // (QuestManager.AddQuestToPermanentsTables reaches into PlutoPermanentsSaveTable, which is
    // initialized by the save subsystem we don't fully wire yet). Without this, NewQuestThread
    // aborts mid-registration. Quests still run their gameplay logic; only save-persistence of
    // quest state is deferred. Remove once the save/permanents subsystem is wired.
    script_vm->run_source(
        "if QuestManager and QuestManager.AddQuestToPermanentsTables then "
        "QuestManager.AddQuestToPermanentsTables = function() end end",
        "=permanents_shim");

    return static_cast<int>(loaded_scripts.size() - before);
}

int NativeGame::load_mods(const std::filesystem::path& dir) {
    if (!script_vm) return 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    std::vector<std::filesystem::path> mods;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".lua") mods.push_back(entry.path());
    }
    std::sort(mods.begin(), mods.end());  // deterministic load order
    int loaded = 0;
    for (const auto& mod : mods) {
        if (script_vm->run_file(mod.string().c_str())) ++loaded;
        // else: broken mod -> skip (its error is in script_vm->last_error()), not fatal
    }
    return loaded;
}

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
    world.resync_npcs_from_entities();  // agents resume at their restored transforms
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
    prepare_world();
    return true;
}

void NativeGame::prepare_world() {
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
}

void NativeGame::tick(double delta_seconds) {
    delta_seconds = std::clamp(delta_seconds, 0.0, 0.25);

    // Input is sampled ONCE per frame, before the fixed-step accumulator: retail
    // input_pad_poll (@0x822B3A10) polls per frame, not per simulation sub-step.
    // external_input lets a headless driver (tests/tools) set `input` directly.
    if (!external_input) input_sampler.tick(input);

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

            // Entity/brain: tick the live NPCs (ACT layer) against the hero as target/
            // LOD centre — before movement, so the follow camera later frames the moved
            // hero. Uses last frame's hero position (1-frame lag, consistent).
            world.update_npcs(collision, player.position(),
                              static_cast<float>(simulation_step));

            // Melee: X swings toward the aim (camera yaw). One swing per press (edge).
            if (input.pressed(PadButton::X)) {
                world.melee_attack(collision, player.position(), camera_controller.yaw);
            }

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
