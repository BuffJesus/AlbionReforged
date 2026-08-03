# Fable II Native PC Port

This is the product plan for the actual PC port. The ReXGlue recompilation and the
`rexgpu-native` plugin are development oracles only; neither is part of the target
runtime.

## Non-negotiable boundary

The shipping executable must not depend on:

- XEX loading or PPC translation
- Xbox kernel APIs
- guest virtual addresses or a 512 MiB physical-memory model
- Xenos PM4 packets, EDRAM aliases, or Xenos shader translation
- Xbox-specific render-target, audio, video, or input behavior

The original game data is still useful, but it is an input to an offline cooker. The
runtime consumes normalized PC data:

```text
360 disc / extracted BNK data
        |
        v
offline cooker (existing AssetBrowser readers + new exporters)
        |
        v
native package: meshes, materials, textures, terrain, animation, entities, scripts
        |
        v
Fable2Native: x64 C++ game runtime + PC renderer + PC memory + PC input/audio
```

## First playable slice

The first slice starts at process launch and reaches a controllable native world. This
proves the product shell before gameplay reconstruction begins:

1. Boot the native runtime and show the correct startup state.
2. Play the intro-video sequence through a replaceable native video-player interface.
3. Show the title screen and accept input to open the main menu.
4. Navigate New Game, Load Game, Options, Mods, and Exit using stable command IDs.
5. Load a cooked level package.
6. Create a PC-native world and player-camera object.
7. Render terrain, static props, materials, and sky through the D3D12 renderer.
8. Stream/unload one neighboring world chunk using ordinary 64-bit allocations.
9. Save and reload native world/front-end state.

This deliberately excludes the translated game loop. Once this works, gameplay
systems can be ported into the same runtime one at a time.

## Runtime layers

### `libf2data`

Owns file formats and cooking only. It may understand BNK, MDL, TEX, EHF, GDB, Lua,
Havok packfiles, and other legacy formats, but those types must not leak into the game
runtime. Its output is versioned native package data.

### `f2core`

Owns native math, handles, jobs, memory arenas, resource lifetime, scene/entity
components, serialization, and diagnostics. It uses native pointers and PC-sized
budgets. Handles are used only where an asynchronous resource boundary needs stable
identity.

### `f2render`

Owns a PC render graph and RHI. Draw packets contain native mesh/material handles,
not PM4 or guest addresses. D3D12 is the primary backend from the beginning; Vulkan can
follow once the scene contract is stable. The renderer also owns presentation, HDR,
resolution scaling, frame pacing, and shader/material caches.

### `f2game`

Owns player, entities, animation, combat, quests, UI, streaming, and save semantics.
Each subsystem is rewritten from PPC/Ghidra evidence and compared against the
recompiled oracle in deterministic scenarios until parity is good enough.

## Migration rule

We do not port another Xbox subsystem into the runtime merely because it is already
available in generated C++. A subsystem enters the native runtime only when its data
model, ownership, allocator, and PC-facing API are defined. The recompiled game can
remain running in parallel as a reference process, but it is never a dependency of
the native executable.

## Immediate implementation order

1. Create the standalone `Fable2Native` runtime and native scene package contract.
2. Extract the AssetBrowser's proven readers behind a headless `libf2data` interface.
3. Cook one real level's terrain, static placements, meshes, and textures.
4. Render that package with the standalone PC renderer.
5. Add camera/input/save, then replace the camera with the native player controller.
6. Port streaming, animation, entities, Lua-facing quest behavior, and combat.

The current ReXGlue renderer work is frozen at its last known-good state. New work
belongs under `Fable2Native` or the offline data tools unless it directly improves the
behavioral oracle.

## Distribution and source ownership

`Fable2Native` ships code, schemas, cookers, and tools only. It does not ship an ISO, XEX,
BNK, video, texture, model, or other copyrighted game asset. At first launch the user selects
their own extracted game directory; an ISO can be passed to the native installer as the acquisition
source. The native runtime validates the installed source and the offline cookers produce
user-local native packages.

The installation boundary is implemented as `f2native_installer`: it accepts a user-selected
Xbox 360 ISO, extracts its XDVDFS game partition into a user-selected destination, excludes the
console system-update partition, and validates the resulting source before the frontend uses it.
The GUI wizard will wrap this same library once the D3D12 setup shell has its progress UI.

## Vertical-slice checkpoint

The native executable now traverses the shell through a real world-rendering boundary:

`source validation -> boot -> replaceable intro sequence -> title -> main menu -> loading -> D3D12 world`

`--scene` loads a user-cooked `F2SCENE` package into the native scene model and submits its meshes
through the standalone D3D12 pipeline. A procedural fallback keeps the handoff testable before the
first real level cooker is complete. The next fidelity step is replacing the fallback UI/media
surfaces with cooked Fable presentation assets, then adding native camera input and save state.

## Frontend fidelity

The native frontend is a first-class subsystem, not a placeholder. Its state machine and
stable command IDs cover boot, replaceable intro videos, title, main menu, save-card/load
flow, options, mod manager, and exit. Oracle captures and menu input traces provide the
behavioral reference; native UI assets are cooked into the same package system as world data.

The extracted startup sequence is `microsoft_logo -> lionhead_logo -> middlewarelogos -> intro`.
`Fable2Native/tools/cook_videos.py` converts those Bink inputs to a versioned MP4 manifest. The
native runtime owns clip order, durations, skip policy, and replacement IDs; decoding and D3D12
texture presentation remain behind that interface so a mod can replace a clip without changing
frontend code.
