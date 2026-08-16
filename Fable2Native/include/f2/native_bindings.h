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

}  // namespace f2
