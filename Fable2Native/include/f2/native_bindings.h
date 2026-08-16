#pragma once

// The gameplay native API surface bound onto the embedded Lua VM.
//
// Registers the grounded subset of the retail Lua natives (lua_natives5_catalog.tsv)
// that map to the native port's EXISTING systems — Debug/Game/Player/World/Quest/Camera/
// Health/Entity. Everything else the game's scripts call falls through to the auto-stub
// (native_script.h install_autostub). Each binding reaches game state via the VM's
// user_data (-> NativeGame). Natives whose value has no retail source (dt, RNG) are
// flagged in the .cpp.

namespace f2 {

class NativeScriptVM;
struct NativeGame;

// Bind the gameplay natives onto `vm` (user_data must already point at `game`).
void register_native_api(NativeScriptVM& vm, NativeGame& game);

// Bind the boot natives (RunScript, which pulls a named LuaQ chunk from the game's
// script BNK via game.script_bnk and runs it). Used by NativeGame::boot_game_scripts.
void register_boot_api(NativeScriptVM& vm, NativeGame& game);

// Bind the game-script SUBSTRATE: the load-bearing natives the game's own quest/gameflow
// Lua calls so its coroutines can actually run and advance on the native systems — manager
// registration (SetGeneralScriptManager/SetQuestUpdateFunction/SetAIManager), the hero +
// entity object model (GetPlayerHero, Debug.CreateEntityAt, entity methods), the
// MessageEvents queue + event objects (the central quest poll), and a few grounded gameflow
// natives (IsToStartGameflow, Timing.*). Overrides `print` to capture into game.script_log.
// Grounded in ghidra_out + the decompiled questmanager.lua/gameflow.lua wait idioms; the
// long tail of unimplemented natives still falls through to the auto-stub.
void register_game_systems_api(NativeScriptVM& vm, NativeGame& game);

}  // namespace f2
