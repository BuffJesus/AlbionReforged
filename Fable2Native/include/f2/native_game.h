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
#include "native_script.h"
#include "native_bnk.h"
#include "native_message.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    // When true, tick() does NOT sample devices — a headless driver (tests/tools) sets
    // `input` directly each frame. Default false = the app samples real devices.
    bool external_input = false;

    // Master gameplay tick driver (Quest -> General -> AI, retail-recovered order).
    ScriptSystems script_systems;

    // Embedded Lua 5.1 VM (null until enable_scripting()). When enabled, the three
    // script managers resume their Lua-side Update each InWorld tick.
    std::unique_ptr<NativeScriptVM> script_vm;
    // The game's script BNK (null until boot_game_scripts). RunScript pulls chunks from it.
    std::unique_ptr<BnkReader> script_bnk;
    std::vector<std::string> loaded_scripts;  // normalized names RunScript has loaded

    // Game-script substrate (populated by boot_game_scripts). The message-event bus quests
    // poll; the hero entity handle GetPlayerHero() returns; per-entity display names for
    // Debug.CreateEntityAt'd + hero entities; and the three manager Update callbacks the
    // game's managers register (SetGeneralScriptManager/SetQuestUpdateFunction/SetAIManager),
    // resumed each InWorld tick in the retail Quest->General->AI order.
    MessageBus messages;
    std::uint64_t hero_uid = 0;
    std::unordered_map<std::uint64_t, std::string> entity_names;
    std::unordered_set<std::uint64_t> killed_entities;  // Entity:Kill() marks; IsAlive() reads
    // SearchTools state: one pending name-filter per open search handle (1-based). A quest's
    // GetAllEntitiesWithName = StartNewSearch -> FilterWithName -> GetSearchResults, so we
    // record the filter on the handle and resolve it to named entities on GetSearchResults.
    std::vector<std::string> search_filters;
    int quest_update_ref = -2;    // luaL_ref sentinels (<0 = unset)
    int general_update_ref = -2;
    int ai_update_ref = -2;

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

    // Lines emitted by the Debug.Log native (observable output for scripts/tests).
    std::vector<std::string> script_log;

    double elapsed_seconds = 0.0;

    bool load_scene(const std::filesystem::path& path, std::string& error);
    // Set up the live world from the current `scene` (entities + collision + hero
    // placement). load_scene calls this; a headless driver can call it after setting
    // `scene` directly to enter the world without a file.
    void prepare_world();
    void tick(double delta_seconds);

    // Create the Lua VM, register the core natives, and wire the Quest/General/AI
    // managers to their Lua Update entry points (QuestUpdate/GeneralUpdate/AIUpdate).
    // Opt-in (the app/tools/tests call it); returns false if the VM failed to boot.
    bool enable_scripting();

    // Load every *.lua in `dir` (sorted, so load order is deterministic) into the VM —
    // the mod entry point. A broken mod is skipped (its error is left in the VM's
    // last_error), not fatal. Returns the number successfully loaded. No-op if scripting
    // isn't enabled or the dir is missing.
    int load_mods(const std::filesystem::path& dir);

    // Boot the game's OWN Lua scripts: open the script BNK under `data_root`, install a
    // manager boot shim + the auto-stub, then load generalsetupscript, whose RunScript
    // list pulls the ~160 gameplay scripts from the BNK. Requires enable_scripting() first.
    // Returns the number of scripts loaded, or -1 if the BNK could not be opened. Honest
    // scope: scripts LOAD and their boot coroutines are created; full gameplay needs the
    // backing systems the stubbed natives stand in for.
    int boot_game_scripts(const std::filesystem::path& data_root);

    // Load the quest bootstrap (quests/questsetupscript.lua) on top of a booted VM — pulls
    // QuestManager + the quest modules + gameflow from the BNK. Separate from boot_game_scripts
    // because retail loads quests on a later path (level/gameflow entry), not at general boot.
    // Requires boot_game_scripts() first. Returns scripts loaded, or -1 if unavailable.
    int load_quest_scripts();

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
