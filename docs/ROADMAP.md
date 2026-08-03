# Roadmap

> Native-port directive: the product is a faithful, moddable C++23 PC game. ReXGlue is a
> behavioral oracle only; D3D12 is the primary native renderer.

Four tracks, sequenced by dependency and ROI. The **north star** is a moddable — eventually
multiplayer — Fable II PC port with a Creation-Kit-style authoring tool.

## Track A — Recompilation (behavioral oracle)  ·  FROZEN
Keep the recompiled game available as a reference implementation only.
- ✅ Correct (TU1) executable recompiles, builds, boots deep into init.
- ⛔ **Blocked** at an upstream ReXGlue **core heap/threading** crash (guest CRT exit / main thread
  returns without spawning game threads). This is maintainer WIP (only the child section works even
  in their build).
- **Plan:** no further renderer/runtime debugging as a product path. Preserve the last known-good
  build for captures, deterministic behavior checks, and subsystem parity tests.

## Track A2 — Native PC port  ·  ACTIVE
Build a standalone x64 game runtime that consumes cooked native scene/entity data. The target is
documented in [NATIVE_PORT_PLAN.md](NATIVE_PORT_PLAN.md). It must not depend on XEX/PPC translation,
Xbox kernel calls, guest memory, PM4, EDRAM, or Xenos shader translation.

- **First slice:** cooked level + terrain/static props + native camera/input + native save/load.
- **Legacy data:** BNK/MDL/TEX/EHF/GDB/Lua readers remain offline cooker code only.
- **Oracle:** the frozen recompilation is used for screenshots, traces, saves, and behavior checks.
- **End state:** ReXGlue is removed once the native game systems cover the needed behavior.

## Track B — Decompilation (understand the engine)  ·  ACTIVE
Turn the machine translation into *understanding* — the prerequisite for deep modding + MP.
- ✅ Ghidra 12.1 + XEXLoaderWV analysis of the TU1 exe complete (`Fable2_TU1`); GhidraMCP ready.
- **Next:**
  1. Seed known labels (Lua VM, resource system, heap) from the recomp into Ghidra.
  2. Map the subsystems we want to touch: **item/appearance**, **quest Lua**, **level/terrain
     loaders**, **networking/session** (for MP).
  3. Do *functional* (not byte-matching) decompilation of those subsystems into clean C++, verified
     against the recomp at runtime (the recomp is a behavioral oracle). Optionally use the
     community's "custom xex with known output" as a regression oracle.
  4. Use auto-re-agent / GhidraMCP to scale up function labeling where useful.
- This is incremental: the recomp keeps running while we decompile subsystem-by-subsystem.

## Track C — Modding toolchain (toward a Creation Kit)  ·  ACTIVE
Author custom content — models, textures, armours, quests, and eventually **landscapes/terrain**.
- **Foundation:** Fable2AssetBrowser (decodes models/textures/anims/levels/terrain/Lua) — building.
- **Layers** (see MODDING.md):
  1. *Read* — decode BNK/Lua/level/terrain (mostly done in AssetBrowser).
  2. *Overlay* — recomp VFS loose-file override (`Fable2Recomp/src/ModSupport.cpp`) + BNK injection
     (`BnkWriter`), so mods load without touching originals.
  3. *Author* — grow AssetBrowser into an editor: level/terrain editing, item/appearance, quest/Lua.
  4. *Engine bridge* (needs Track B) — extend the decompiled loaders to accept modern formats
     (glTF/PNG/heightmaps) so the CK authors in convenient formats.
- **Milestone ladder:** whole-file asset swaps → per-entry BNK injection → new models/armours →
  custom textures → quest/Lua edits → **custom terrain/landscape** → full CK-style editor.

## Track D — Multiplayer  ·  DESIGN
Co-op / party play, enabled by the decomp.
- Recomp/decomp means **all players run the same x86 code** → the cross-architecture floating-point
  desync that plagues emulator PvP largely disappears (a real advantage).
- Fable II already shipped henchman co-op → existing networking/session code to build on (decompile
  it in Track B first).
- Approach TBD (lockstep vs state-sync); design once the netcode subsystem is understood.

## Near-term focus
1. **Native port (A2):** create `Fable2Native`, define the native scene package, and cook one real level.
2. **Data layer:** extract AssetBrowser readers into headless `libf2data` targets.
3. **Gameplay:** use Ghidra/auto-re-agent only for the next native subsystem's behavioral specification.
4. **Oracle (A):** keep the frozen recompilation runnable for parity tests; do not expand its renderer.

## Native runtime rules

- New runtime code is clean, human-readable C++23.
- D3D12 is the primary renderer; Vulkan is a later backend behind the same RHI.
- Native code uses ordinary PC memory, handles, jobs, streaming, and serialization.
- Legacy BNK/MDL/TEX/EHF/GDB/Bink formats are offline cooker inputs, not the native runtime object model.
- Fidelity is measured against captures, input traces, save states, asset identities, and behavior from
  the frozen oracle—not by preserving 360 implementation accidents.

## Native frontend milestone

The first native slice is boot -> intro video -> title screen -> main menu -> options/mod manager ->
cooked level -> native camera/input/save. The frontend is not delegated to the old GUI VM.

The startup video sequence is now an explicit native package contract:
`microsoft_logo -> lionhead_logo -> middlewarelogos -> intro`. Bink is an offline input; the
runtime consumes cooked, replaceable video assets and exposes stable clip IDs to mods.

Distribution rule: the repository contains no shipped game assets. The native executable requires
a user-selected extracted game directory (or an ISO passed to the native installer) and stores
only the user's source-path preference.

The first installation implementation is `f2native_installer`, sharing the native XDVDFS reader
with the future graphical setup wizard. It extracts user-owned ISO contents into a user-selected
install directory and validates the result before launch; it never adds source assets to the repo.

Vertical-slice status: source validation, startup state machine, title/main-menu input, loading,
and a real native D3D12 world draw are now connected. `--scene` supplies a user-cooked package;
the asset-free procedural world is only a bring-up fallback.
