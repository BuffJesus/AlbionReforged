# Native level cook → "start a new game" (executable plan, validated on real data)

Goal: after ChooseCard→Loading→World, Fable2Native renders the REAL childhood level. Data-driven from
the decomp + the game's real containers; cook-at-install; loose files at runtime; modder-friendly.
Every step below is grounded in verified facts — no guessing.

## Validated pipeline (2026-08-09, end-to-end on real data)
1. **Container → level file.** `data/levels.bnk` holds the real level tree. Verified via the built
   `Fable2AssetBrowser/source/build/f2tool.exe`:
   `f2tool list levels.bnk` shows `worlds\albion\bwsslums\chapter2slums\chapter2slums.{engine_level,
   engine_data,gdb,havok_scenario,save,lmp,texture_atlas}`, `_streaming.bnk`, `textures.bnk`,
   `level.vfsconfig`, `pathdata/*` — EXACTLY the 4-layer structure `ghidra_out/world_level_format.txt`
   described. `chapter2slums` = the childhood level (`scenarios.list` `level:'albion\bwsslums'` scenario
   `chapter2slums`; `ghidra_out/newgame_handoff.txt` SetInitialWorldName="Albion"/SetInitialLevelName).
2. **Extract.** `f2tool extract levels.bnk "worlds\albion\bwsslums\chapter2slums\chapter2slums.engine_level" out`
   → 189 KB file. Header = ASCII `LevelGraphicsFile\0` + u32 **version=12** + entry count `0xB6` + first
   record `worlds\albion\bwsslums\heightfields...`. Matches AssetBrowser `kEngineLevelMagic` +
   guest parser 0x82ABAEF0.
3. **Parse (byte-exact, no guessing).** `Level::ParseEngineLevel(bytes, EngineLevelInfo&)` (LevelLoader.h)
   → `EngineLevelInfo.prop_blocks[]`: each `PropBlock{ model_path, shadow/lod/extra_model_path,
   instances[] }`; each `PropInstance{ hash, values[20] (transform), has_full_transform, gdb_entity_hash }`.
   Terrain geometry (positions/indices) is produced directly by LevelLoader/HeightfieldLoader from the
   `.ehf`/`.ghf` (grid mesh). Prop models = MDL via `MDL/ModelParser`, resolved from the level's
   `textures.bnk`/`_streaming.bnk` per `level.vfsconfig`.
4. **Emit F2SCENE (the runtime's existing loose format).** `native_scene.cpp load_native_scene` reads
   text `F2SCENE 1` / `material <name> r g b a [albedo normal material]` / `mesh <name> vcount icount mat`
   / `vertex px py pz nx ny nz u v` / `index ...` / `instance <mesh> px py pz rx ry rz scale`. The cooker
   writes exactly this from prop meshes (ModelParser) + instances (PropInstance transforms) + terrain.
5. **Runtime loads loose file.** World state → `--scene <cooked.f2scene>` → `game_.load_scene` →
   NativeWorldRenderer (D3D12+Vulkan) renders it. Never touches containers at runtime.

## Build steps (each buildable + verifiable)
- **A. `f2tool cook-level <levels.bnk> <level-path> <out.f2scene>`** (new command in
  `Fable2AssetBrowser/source/tools/F2Tool.cpp`; it already links `Level/`, `MDL/ModelParser`, BNK reader):
  extract engine_level → ParseEngineLevel → for each PropBlock, resolve+parse its MDL (from the level's
  banks) → NativeMesh/Material; add terrain mesh; write F2SCENE + cooked DDS. Verify: run on chapter2slums,
  screenshot the World state via `--scene`.
- **B. Installer step.** Add `cook_levels` to `Fable2Native/src/native_cook.cpp` `cook_native_package()`
  (mirrors cook_videos/cook_gui_audio: `run_cooker(f2tool, args, "levels")`), driven by the childhood level
  list. Package layout: `<pkg>/levels/albion/bwsslums/chapter2slums.f2scene` (+ textures).
- **C. New-game wiring.** After ChooseCard→Loading, load the cooked childhood F2SCENE for World (the gender
  scr+0x18 → SetInitialHeroModelName selects the hero template — decomp `newgame_handoff.txt`).
- **D. Hero + camera + physics** (later): NativeScene gains an entity/hero from the `.save`/`.gdb`
  (gdb_instantiation) + the char controller (`physics_collision_system.txt` CECPhysicsSimulationCharacter
  Controlled) + a 3rd-person camera; input drives movement.

## Modder-friendly (baked in)
F2SCENE is human-readable text; cooked assets are loose files a mod overrides; stable asset IDs; ties to
the edit-set/export-as-mod tooling (docs/MODDING_ENVIRONMENT.md). The cooker + schemas ARE the modding
foundation — a mod supplies/edits an F2SCENE or the loose cooked assets, no recompile.

## Ground-truth references
`ghidra_out/world_level_format.txt`, `newgame_handoff.txt`, `gdb_instantiation_re.txt`,
`physics_collision_system.txt`, `hero_appearance_morph.txt`; `Fable2AssetBrowser/source/src/Level/
{LevelLoader.h,LevelLoader.cpp}` + `MDL/ModelParser`; `docs/NATIVE_PORT_PLAN.md` (install policy);
`Fable2Native/src/native_scene.cpp` (F2SCENE) + `native_cook.cpp` (cook orchestration).
