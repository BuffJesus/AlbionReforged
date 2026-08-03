# Fable II Studio — our own C++ modding environment

User directive (2026-07-18): *"I want our own modding environment written in C++ at some point, based
on all of our data — decomp, available programs, etc."* This doc is the architecture for that,
grounded in what actually exists in this repo today. Companion to
[MODDING_MASTER_PLAN.md](MODDING_MASTER_PLAN.md) (the modding strategy) — this is the **tool** plan.

---

## 1. What "the environment" is

One C++ application (working name **Fable II Studio**) that unifies the three things we have built
separately, on top of the data we have reverse-engineered:

1. **Authoring** — the AssetBrowser's readers/editors (models, textures, anims, levels, terrain,
   Lua, audio, GDB) grown into project-based editing.
2. **Knowledge** — the decomp databases as first-class, searchable tool data: the Lua native catalog
   (`ghidra_out/lua_natives5_catalog.tsv`, 3592 rows), item/object IDs (`tools/lua_mod/catalog/`),
   quest/interactable patterns (SYSTEMS_ANALYSIS.md), ~4600 Ghidra labels.
3. **The running game** — the recomp as a live target: launch, deploy mods, and (later) a live IPC
   bridge over the `register_native` Debug.Mod channel for edit-and-reload without restarts.

## 2. What already exists (the parts list, all working today)

| Piece | Where | State |
|---|---|---|
| BNK read/extract/repack | AssetBrowser `BNKCore/BnkWriter` + `tools/lua_mod/bnk_repack.py` | ✅ proven (C++ and Python) |
| Level editor backend+UI | `Fable2AssetBrowser .../Level/LevelEdit.cpp` | ✅ move/rotate/delete/place/chest/undo/save |
| **Durable edit sets** | `edited_levels/<level>.editset.txt` (2026-07-18) | ✅ NEW: journaled, crash-safe, rehydrates on load, BAKED history |
| **Mod packaging** | `LevelEdit::ExportMod` → `<assets>/mods/enabled/<name>/` (2026-07-18) | ✅ NEW: non-destructive, rides the runtime overlay |
| Runtime mod overlay | `Fable2Recomp/src/ModSupport.cpp` | ✅ name-ordered loose-file mods, staged at boot |
| Native API bridge | `Fable2Recomp/src/ModApi.cpp` (`Debug.Mod.*`) | ✅ host C++ callable from game Lua |
| Lua mod pipeline | `tools/lua_mod/` (apply_mod, luacheck, luadis51, script_index) | ✅ proven end-to-end (mod menu shipped) |
| GDB round-trip | AssetBrowser `GdbEdit` | ✅ data-level item/entity edits |
| Format decoders | MDL/textures/anims/terrain/particle_bank/XMA | ✅ read/export; encoders missing |
| Native catalog | `ghidra_out/lua_natives5_catalog.tsv` | ✅ enumeration complete |

## 3. Architecture

```
+---------------------------------------------------------------+
|  Fable II Studio (ImGui shell, C++)                           |
|  panels: Asset Browser | Level Editor | Script/Quest Editor   |
|          Catalog Search (natives/items) | Mod Manager | Log   |
+-------------------+-------------------------------------------+
                    |
        +-----------v-----------+       +------------------------+
        |  libf2 (static libs)  |       |  Game Link             |
        |  f2::bnk  f2::mdl     |       |  - launch recomp       |
        |  f2::tex  f2::gdb     |       |  - deploy mod overlay  |
        |  f2::level (editset)  |       |  - IPC via Debug.Mod   |
        |  f2::lua  f2::catalog |       |    (live reload, poke) |
        +-----------+-----------+       +-----------+------------+
                    |                               |
     game data / mods/enabled/            Fable2Native runtime
```

- **libf2**: extract the format/IO code out of the AssetBrowser UI into linkable libraries with no
  ImGui dependency. This is refactoring, not rewriting — the code exists; it is currently welded to
  the UI (e.g. `LevelEdit.cpp` includes `BNKCore.cpp` directly).
- **Studio shell**: starts as the AssetBrowser itself (it already has the panels); becomes "Studio"
  as project files (edit sets), catalogs, and game-link land.
- **Game Link**: phase 1 is just "launch + deploy" (both exist: overlay dir + launcher flags). Phase
  2 is an IPC channel: a `Debug.Mod.Poll()`-style native pumping a command file/socket, so the Studio
  can hot-reload scripts, teleport the hero to the edited spot, spawn a placed entity, etc. The
  ModApi mechanism for this is proven.
- **Decomp feeds the tool**: every subsystem decompiled (Track B) upgrades a Studio feature from
  "format-level editing" to "semantic editing" (e.g. Havok behaviors, spell defs). The behavioral
  oracle stays the recomp, per MODDING_MASTER_PLAN §2.

## 4. Data formats the environment owns

- **Edit set** (`.editset.txt`, v1, shipped 2026-07-18): TSV journal of a level's pending +
  baked edits (EDIT/ADD/CHEST records, bnk stamp for offset validity). The Studio "project file"
  seed. Format doc: see the header comment in `LevelEdit.cpp` (`editset` block).
- **Mod package** (`mods/enabled/<name>/`, shipped): game-relative file tree + `f2ab_mod.json`
  manifest + `_f2ab/*.editset.txt` provenance. Load order = folder-name sort (ModSupport.cpp).
- Future: quest module templates (Lua), encoder outputs (PNG→tex, glTF→MDL).

## 5. Phases

- **P0 (done 2026-07-18):** durable edit sets + mod export. Level edits are non-destructive and
  packageable; base game restorable via `.bak`/Restore Defaults.
- **P1 — libf2 extraction (STARTED 2026-07-18):** carve `bnk/gdb/level/editset` out of the
  AssetBrowser into libs + a CLI so mods can be built headlessly/CI'd. **`f2tool` shipped** (cmake
  target `f2tool`, `source/tools/F2Tool.cpp`): `list`/`extract`/`verify` over BNKs (streams the
  2.5 GB levels.bnk instantly; `verify` round-trips every entry — the RE_NEXT §2 wishlist item),
  `inject` for raw BNK entries via `BnkWriter` rebuild + one-time `.bak`, `hash [--lower]` for the
  FNV-1 resource/text-name hash used by the loaders, `editset` journal summary, and `mod` folder
  inspect — all smoke-tested on real game data. `BnkWriter` is now split
  from the UI behind `F2Host`; remaining P1 is chunk-compressed repack integration and true `libf2`
  targets.
- **Level-load RE result (2026-07-18):** `docs/LEVEL_LOAD_RE.md` proves the LS_* machines are the
  graphics streaming path (`LevelGraphicsFile` + `EngineResourceList`), not gameplay entity load.
  Editor-added Type-2 model placements enter the stock graphics parser, but their model/resource
  names must resolve through the mounted-bank hashed resource lookup; new model paths need matching
  assets and `engine_data` hashes.
- **P2 — Studio shell:** catalog search panels (natives/items — the TSVs just need a loader + fuzzy
  search), mod manager panel (list/enable/disable `mods/`), project = editset.
- **P3 — Game Link:** launch button + live IPC over Debug.Mod (hot-reload gamescripts hook, warp-to-
  edit, spawn-preview). Needs the ModApi command-pump native (mechanism proven, console gate exists).
- **P4 — Quest/node editor:** visual editor emitting Lua quest modules + placements + OnActionUse
  bindings (the RE in `fable2-quest-interactable-system` is the spec).
- **P5 — Encoders:** PNG→Lh/DDS, glTF→MDL, particle bank writer, XMA pack — unlocks new-asset mods
  (the current Tier-2 blocker set).

## 6. Principles

- **Don't rewrite what runs.** AssetBrowser panels and the Python lua_mod pipeline stay canonical
  until a C++ replacement is *better*, not merely newer. Python tools become `f2tool` subcommands
  over time.
- **Every feature is evidence-backed** — a Studio feature ships only over a decoded format or an
  RE'd system (per SYSTEMS_ANALYSIS.md verdicts).
- **Non-destructive by default** — edits live in edit sets / mod folders; base data restorable.
# Native-port update

The Studio and `Fable2Native` share one clean C++23 data/API vocabulary. `Fable2Native` is the
primary launch/deploy target; the recompilation is an optional parity target. New authoring,
mod-package, menu, script, and runtime-inspection features should target the native runtime first.
