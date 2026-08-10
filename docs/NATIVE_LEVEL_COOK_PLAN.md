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

## Cooker status + exact parse spec (2026-08-09)
- ✅ DONE: the F2SCENE **writer** (`save_native_scene`, Fable2Native/src/native_scene.cpp) + round-trip
  unit test. The cooker's output stage is complete.
- ✅ Per-model cook EXISTS: `Fable2Native/tools/cook_mdl.py` already parses one MDL (via the pure-Python
  `Fable2AssetBrowser/source/addons/fable_mdl_format.py` `parse()` → geoms{positions,indices,uvs,
  specular_tex}) and emits F2SCENE (materials mat_N + meshes). REUSE this per prop model.
- ✅ BNK read: `Fable2AssetBrowser/source/Archive/bnk_reader.py` `BNKReader.list_files()/extract_file()`.
- REMAINING (the cook-level build): parse the engine_level → prop instances, cook each prop MDL, merge
  into ONE F2SCENE with `instance` records from the prop transforms. Resolve model paths from the level's
  own `_streaming.bnk`/`textures.bnk` + global model banks per `level.vfsconfig`.

EXACT engine_level byte format (from AssetBrowser `LevelLoader.cpp` `ParseEngineLevel` — the byte-exact
authority; reuse it verbatim, do NOT re-derive). BeReader is BIG-ENDIAN (Xbox 360): u8=1B; u32=BE 4B;
u64=BE(hi<<32|lo); f32=BE u32 bit-cast; half=BE 16-bit float; cstr=NUL-terminated (≤4096). Layout:
- Header: 17-byte magic `LevelGraphicsFile` (skip) + u32 version (11 or 12) + u32 entry_count.
- Per entry: u32 type, then:
  - type 2 (static props): cstr model_path, cstr shadow, cstr lod, cstr extra; u32 instance_count;
    per inst: u8 flags[3], u64 hash, 20× f32 `values` (values[0..2]=pos, [6..7]=cos/sin yaw, [9..11]=scale).
  - type 4/5/32: cstr str_a; type 4 also skip 8.
  - type 21 (instanced scatter): cstr str_a, cstr str_b; skip 10 (u64+2×u8); u32 loop1_count; skip
    (7*4+12+4+24); loop1: v11 = 4× f32 (values[0..3], set [7]=1,[9..11]=1); v12 = f32 pos[3] + 5× half
    (qx,qy,qz,qw,scale) → values[0..2]=pos, [6]=2(qw*qz+qx*qy)/mag, [7]=(1-2(qy²+qz²))/mag, [9..11]=scale;
    then u32 loop2_count; loop2: per rec 2× f32 + 2× vec3 f32 + … (decorators; can skip for geometry).
  - default/other: bail (unknown type) — but types 2/4/5/21/32 cover the real levels.
Recommended cooker impl: a small C++ tool (`f2native_cook_level`) or f2tool `cook-level` command with a
VERBATIM copy of BeReader + ParseEngineLevel (self-contained; no UI deps) → prop_blocks → per prop:
resolve+cook MDL (fable_mdl_format) → NativeMesh + `instance` per PropInstance transform → save_native_scene.
Verify: run on the extracted `chapter2slums.engine_level`, then World `--scene` screenshot.

## Cooker progress (2026-08-09, verified on real data)
- ✅ PARSE STAGE DONE + VERIFIED: `Fable2Native/tools/cook_levels.py` (faithful port of ParseEngineLevel)
  parses the REAL `albion\bwsslums\chapter2slums.engine_level`: v12, all 182 entries zero-desync,
  177 prop blocks / 7011 instances (bs_townhouse/fairfaxcastle/bw_treelargeoak/glb_longgrass02 with real
  world positions). Commit ee1e28f.
- ✅ MODEL LOCATION FOUND: the prop `.mdl` models live in the LEVEL's OWN `<scenario>_streaming.bnk`
  (chapter2slums_streaming.bnk = 499 `.mdl.gmd` + `.hkx` collision). Extract via f2tool. The referenced
  `bs_townhouse_v1_facade_mid.mdl` = bnk entry `...BS_TownHouse_V1_Facade_Mid.mdl.gmd`.
- ⚠ GEOMETRY STAGE (next, the deep layer): a `.mdl.gmd` is a 60-byte "GameMesh" v3 DESCRIPTOR, not the
  mesh — the real vertex/index buffers are reassembled by the GLUE system from the level's `.lmp`
  (level-mesh-pool, ~3MB) + streamed buffers. `cook_mdl.py`/`fable_mdl_format.parse` expect a GLUED (already
  reassembled) MDL. So the geometry stage = implement/reuse the .gmd→.lmp glue (AssetBrowser's LevelLoader/
  ModelParser do this for display; UI-tangled → needs a headless glue extraction) → glued MDL →
  fable_mdl_format → NativeMesh → merge with cook_levels instances (type-2 = 20-float transform,
  type-21 = normalized pos/yaw/scale) → save_native_scene. Then World `--scene` renders the real level.
- ✅ MANIFEST VALIDATED (`cook_levels.py --json`): chapter2slums = **32 DISTINCT models → 7011 instances**
  across 177 prop blocks. Key simplification for stage 2: cook each of the **32 unique MDLs ONCE**, then emit
  the 7011 placements as cheap `instance` records — the merge is 32 mesh cooks + 7011 transforms, not 7011
  mesh cooks. JSON manifest (`prop_blocks[].model` + `.instances[].pos/values|yaw_sin_cos|scale`) is the
  stage-2 input.
- ✅ INSTANCE TRANSFORM DECODE PROVEN (measured on real chapter2slums type-2 instances, not guessed):
  `values[0..2]`=world pos; `values[6..7]`=(cos,sin) yaw with **|cos,sin|==1.000 exactly** on EVERY instance
  (confirms a unit 2D rotation, not a scaled matrix column); `values[9..11]`=scale==[1,1,1] (uniform unit).
  So F2SCENE `instance <mesh> px py pz rx ry rz scale` = `pos=values[0:3], rx=rz=0,
  ry=atan2(values[7],values[6]), scale=values[9]`. type-21 is already normalized (pos + yaw_sin_cos + scale)
  → same emit (ry=atan2(sin,cos)). Both paths land on the SAME instance-record recipe — stage 2 has no
  transform ambiguity left; only the mesh geometry (the glue) remains.

## ✅ STAGE 2 SHIPPED + RENDERS (2026-08-09) — the cooked level draws in the native port
- `cook_levels.py --cook out.f2scene --header-bnk globals_model_headers.bnk --body-bnk <level>_models.bnk`
  glues each distinct prop model (MeshFile header ++ polymsh body via f2tool, per the RE'd glue) and
  merges instances into ONE F2SCENE. chapter2slums type-2 → 43 meshes / 21,584 verts / 14 building
  instances (townhouses, market walls, structures) with real material texture paths. Foliage/LOD trees
  skip with MdlParseError (fable_mdl_format doesn't decode those strides yet) — expected; buildings are
  the landmarks.
- Runtime: `f2native_frontend --start-world --scene <cooked.f2scene> --game-dir <data>` renders it
  (new `--start-world` test flag + `NativeFrontend::debug_jump_to`). VERIFIED via PrintWindow screenshot.
- ⚠ THE WORLD RENDERER WAS NEVER EXERCISED BEFORE — reaching World surfaced 3 real D3D12 bugs, all found
  by DATA (D3D12 debug layer, gated by env `FABLE2NATIVE_D3D_DEBUG=1` → `d3d_debug.log`), not guessing:
  1. **Missing viewport/scissor** in `NativeWorldRenderer::render` — an unset 0x0 viewport rasterises
     NOTHING with zero validation error (THE primary black-screen cause). Now sets RSSetViewports/Scissor.
  2. **`pipeline.SampleMask = 0`** (zero-init default) — "preventing blend operations for all samples";
     writes no samples. Set to 0xFFFFFFFF. (Debug layer caught this one explicitly.)
  3. Descriptor heap must be bound (SetDescriptorHeaps) BEFORE the world draw's root descriptor table.
  Also: instance rotation was dropped (added yaw via `place_vertex` Rz*Ry*Rx in both renderers), camera
  now fits the scene AABB (`scene_center_`/`scene_radius_`), CULL_NONE (strip winding), row_major cbuffer.
- ✅ AXIS FIX (user-caught "everything laying sideways"): model verts are game-space Z-up; the engine
  renders Y-up via game_vec_to_xform_axes(x,y,z)={x,z,y}. The cooker now swaps vertex+normal axes the SAME
  way it already swapped instance positions → buildings stand UPRIGHT (peaked roofs/gables/spires visible).
- REMAINING for a good-looking render (NEXT SESSION, in priority order): (a) DEPTH BUFFER — currently no
  depth → buildings read as a flat merged grey silhouette (overdraw, no occlusion); add a D32 depth
  texture+DSV, enable depth in the world pipeline, clear+bind it. THIS is the biggest visual win. (b)
  `.tex` texture decode/cook → currently grey vertex-color fallback (albedo.Sample returns white); (c)
  lighting (the sun dir is in the F2SCENE but the PS ignores normals); (d) foliage MDL strides in
  fable_mdl_format (type-21 grass/trees skip with MdlParseError); (e) terrain heightfield mesh.

## Ground-truth references
`ghidra_out/world_level_format.txt`, `newgame_handoff.txt`, `gdb_instantiation_re.txt`,
`physics_collision_system.txt`, `hero_appearance_morph.txt`; `Fable2AssetBrowser/source/src/Level/
{LevelLoader.h,LevelLoader.cpp}` + `MDL/ModelParser`; `docs/NATIVE_PORT_PLAN.md` (install policy);
`Fable2Native/src/native_scene.cpp` (F2SCENE) + `native_cook.cpp` (cook orchestration).
