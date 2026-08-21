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
- ✅ DEPTH + LIGHTING DONE (2026-08-10, D3D12, screenshot-verified) — (a)+(c) shipped together because
  depth occlusion is INVISIBLE when every surface is flat grey (the "flat silhouette" was BOTH no-depth
  AND no-shading). Now chapter2slums renders as real 3D (Fairfax castle/towers, townhouses, bridge/wall,
  market structures) with correct occlusion + directional shading. Impl:
  - Depth (`native_frontend_app.cpp`): `kDepthFormat=D32_FLOAT`; DSV heap + `create_depth_target()` (D32
    tex, DEPTH_WRITE, clear 1.0) at swapchain-init + `resize()`; World branch rebinds `OMSetRenderTargets(
    rtv,&dsv)` + `ClearDepthStencilView(1.0)` (frontend states stay depthless). Renderer pipeline:
    DepthEnable/WriteMask ALL/LESS_EQUAL, DSVFormat D32, DepthClipEnable TRUE (projection already emits
    standard [0,1] depth).
  - Lighting (`native_world_renderer.cpp`): normals+sun_direction were already in the scene data, just
    dropped. Added normal to the renderer Vertex/input-layout (POSITION 0/NORMAL 12/COLOR 24/TEXCOORD 40),
    world-rotate the normal, pass normalized sun in the cbuffer (CBV vis VERTEX→ALL), PS lambert
    `0.35+0.65*saturate(dot(n,-sun))`. D3D debug layer (FABLE2NATIVE_D3D_DEBUG=1) raised zero messages.
- ✅ ALBEDO TEXTURES DONE (2026-08-10, D3D12, screenshot-verified) — (b) partially: the buildings whose
  albedo lives in `globals_textures.bnk` (bs_* shared, self-contained comp-1 LhTex) now render TEXTURED
  (brick/timber/stone). The decode path already existed (`f2native_cook_lh_tex` = productised AssetBrowser
  LhTexCodec, `.tex`→DXT1 DDS; runtime `decode_dds_rgba8` loads DXT1/DXT5). What was missing = wiring the
  world cooker to extract+cook each material's albedo `.tex` and emit the loose-DDS path. Impl:
  - `cook_levels.py` `_cook_textures()`: for each distinct albedo token, resolve it across an ordered list
    of `--textures-bnk` containers (repeatable arg), `f2tool extract` the `.tex`, run `--tex-cook`
    (f2native_cook_lh_tex) → DDS in `<scene>.textures/`, emit `albedo=<abs-dds>` (runtime `resolve_texture`
    loads absolute paths directly → ZERO renderer change). Uncooked albedo/normal/spec tokens are dropped
    so the material shows its flat base colour instead of sampling white. Verified: 22/27 chapter2slums
    albedos cook (with 3 source bnks) and the townhouses + Fairfax castle + gatehouse walls + rocky cliff
    base all render textured.
  - ⚠ Space-name gotcha (fixed): some .tex entries are stored WITH SPACES ("bs_gatehouse_stone top.tex").
    The cooker now resolves textures with the RAW model path (spaces preserved) — texture_token's
    space->underscore was breaking those lookups — and sanitizes only the output DDS filename.
  - The source bnks (ALL comp-1 self-contained — 1024mip0 is NOT comp-7 as first assumed): shared bs_*
    in `Globals/globals_textures.bnk`; fairfax `fc_stone*`, `cliffg_*`, `bs_haunted_*` in
    `Globals/1024mip0_textures.bnk`; foliage/ground in the level's own `textures.bnk`. Pass each with a
    repeated `--textures-bnk`; searched in order.
  - Repro: `cook_levels.py … --textures-bnk <globals_textures.bnk> --textures-bnk <1024mip0_textures.bnk>
    --textures-bnk <level textures.bnk>` (default `--tex-cook` = build/RelWithDebInfo/f2native_cook_lh_tex.exe).
- ✅ ALL 43 ALBEDOS COOK (2026-08-10) — the last missing textures (`esa_facade_window_*`, `fc_rooftiles`,
  `fc_window_arched`) live in the level's per-region SHARED banks `worlds\albion\shared\shared_6281.bnk`
  + `shared_2445.bnk` (nested in `data/levels.bnk`, mounted via the level's `level.vfsconfig`) — RE'd in
  `ghidra_out/texture_resolution_re.txt`. Extract both from levels.bnk and pass as `--textures-bnk` sources
  (resolution order: shared_6281 → shared_2445 → level textures.bnk → 1024mip0 → globals_textures). With
  types 2,21 the cook now resolves 43/43 distinct albedos. General fix (TODO): parse level.vfsconfig to
  collect the shared-bank paths instead of hard-coding.
- ✅ TERRAIN GROUND (2026-08-10) — the level heightfield renders as ground. cook_levels.py
  `_build_terrain()` + `--terrain-ghf`/`--terrain-stride` port the validated `ghidra_out/terrain_mesh_re.txt`:
  `.ghf` = gzip of a 577×577 cell grid (origin vec3 + wCells/hCells + 14-byte cells {f32 height, f32 water,
  u32 matGUID, u8, u8}); vertex per cell at game(x*0.5, y*0.5, height), central-diff normals, UV*0.125,
  2 tris/quad; emitted game-space (flows through the same {x,z,y} swap as props) as one earth-tone material
  + mesh + identity instance. The `.ghf` is a per-region SHARED file in levels.bnk (`worlds\albion\bwsslums\
  heightfields\ch2_heightfield_slums_id_3a6902ed.ghf`, referenced from the level's `.list`). tile=0.5 wu/cell
  is a FIXED constant (proven, not the header float). Townhouses sit on the ground w/ height relief; Fairfax
  castle floats separately (it's at negative game-Y, outside this chunk — expected). Materials now carry a
  per-material base colour (was hardcoded 0.72 grey). NEXT: the render `.ehf` splat/atlas texture (Phase T2).
- ✅ HEMISPHERE-AMBIENT SHADING (2026-08-10) — world PS ladder step 1 of `ghidra_out/world_shading_model_re.txt`:
  replaced flat `0.35+0.65*ndl` with hemisphere ambient (sky above / ground below by world-up N.y) + N·L sun.
  Shadowed faces read sky-tinted instead of near-black. NEXT (ladder 2-3): cook the normal (2-ch BC5) + spec
  `.tex` channels, bind t1/t2, derivative-TBN normal mapping + Blinn-Phong (drop-in HLSL in the spec §7).
- ✅ FOLIAGE RENDERS (2026-08-10) — grass + trees now decode (was: type-21 + tree LODs skipped with
  MdlParseError). Root-caused + fixed per `ghidra_out/foliage_system_re.txt`: taught the MDL parser
  (`Fable2AssetBrowser/source/addons/fable_mdl_format.py`, ⚠ that tree is gitignored — local only) the
  foliage buffer layout — 3 strides (48 = full-f32 pos/normal/uv grass; 36 = half tree; 20 = bw, unported),
  the IsTree/NewTree material realign, an is_foliage gate (path OR tree-tag OR fallback-on-desync for
  `\Global\` grass). `cook_levels.py` passes `file_path=model_path` so the gate fires. chapter2slums with
  `--types 2,21` now cooks 84 meshes / 5496 instances / 147 blocks (was 43/14/5), zero skips; green oak
  canopies + grass scatter render. REMAINING: (c) sample the normal map (renderer PS ignores t1); foliage
  LOD = model-swap by distance (draws near model now); wind = GDB weather-theme shader sway (not decoded). (e) terrain
  heightfield. (f) VULKAN world-renderer parity — behind D3D12 (old origin-orbit camera, no scene-AABB fit,
  no depth attachment in its render pass, no lighting, no textures).

- ✅ NPC TOWNSPEOPLE (2026-08-10, D3D12, screenshot-verified) — the slums now populate with bind-pose
  child villagers at the level's creature-spawn markers. Data-driven from `ghidra_out/npc_spawn_re.txt`,
  no guessing. Impl:
  - New C++ tool `Fable2Native/tools/npc_markerdump.cpp` (added to the AssetBrowser CMake as target
    `npc_markerdump`, header-only dep `Skybox/GdbReaderInternal.h`): parses `<level>.save` (XML
    name→GUID) + `<level>.gdb`, and for each creature-spawn marker follows the SimpleTransformComponent
    (`0x619F96CF`) → Position (`0xBD7C27D4`) / Rotation (`0x21EBC83B`) vec3 chain that
    `GdbParser::LookupPlacement` does NOT traverse. Emits `markers.json` (name, game pos, yaw).
    VALIDATED: **124/124** markers on chapter2slums (109 MarkerCreatureGeneratorSpawnPoint + 15
    "Creature Generator Spawn Point"), miss=0, positions match the reference markerdump exactly.
    Unlike the RE-session's throwaway markerdump.cpp (baked `save_map.h`), this parses the `.save`
    XML at runtime — reusable for any level.
  - `cook_levels.py` gained `cook_npcs()` (via `read_npc_markers()` + a block in `cook_level()`),
    mirroring `--hero`: for a matched child villager part-set (`CH_mchild_{head,torso,legs}_01`, all
    validated through the 28-byte skinned path — NO StringBlock) it glues header++body per part
    (globals_model_headers.bnk + globals_models.bnk), parses via fable_mdl_format, emits each geom as
    an F2SCENE mesh (same `{x,z,y}` swap), and drops one instance per (marker × part-geom) at the
    marker render transform. Part diffuse `.tex` cook alongside via the existing `_cook_textures()`.
    New flags: `--npcs`, `--npc-limit N`, `--level-save`, `--level-gdb`, `--npc-body-bnk`
    (defaults to `--hero-body-bnk`), `--npc-female` (child-female set), `--npc-markerdump`.
  - VERIFIED: `--npcs --npc-limit 20` cooks **20 villagers × 3 parts = 100 instances** + 5 diffuse DDS
    (mchild_face/torso/legs) with hit=124 miss=0. A single-villager cook renders as a correct standing
    child — head+torso+legs stack with ZERO offset (they share the rig bind pose, as the spec predicted);
    at full-scene zoom the villagers are small (child ≈1.5 wu tall vs the ~130-wu-wide town spread), but
    present and on-ground at the real marker positions.
  - CAVEATS (from `npc_spawn_re.txt`, all expected): adult head parts (e.g. CH_mnormal_strong_head_01)
    use the StringBlock layout fable_mdl_format doesn't port → the cook uses the child set (no
    StringBlock) and skip-and-continues any failing part. Runtime appearance tint/clothing
    (VillagerComponent) + which archetype each marker actually spawns (the *_Generator payload) are
    runtime AI, out of scope for a static cook (a first pass places the same child set at every marker).
  - Cook command (full level + 20 NPCs):
    `python Fable2Native/tools/cook_levels.py <chapter2slums.engine_level> --cook out.f2scene
    --header-bnk Globals/globals_model_headers.bnk --body-bnk <chapter2slums_models.bnk> --types 2,21
    --terrain-ghf <slums.ghf> --terrain-stride 2 --water-file <slums.water>
    --hero --hero-body-bnk Globals/globals_models.bnk
    --npcs --npc-limit 20 --npc-body-bnk Globals/globals_models.bnk
    --level-save <chapter2slums.save> --level-gdb <chapter2slums.gdb>
    --textures-bnk <shared_6281> --textures-bnk <shared_2445> --textures-bnk <level textures.bnk>
    --textures-bnk Globals/1024mip0_textures.bnk --textures-bnk Globals/globals_textures.bnk`.
    (`npc_markerdump.exe` auto-located in `Fable2AssetBrowser/source/build/`.)

## ▶▶ SESSION SNAPSHOT 2026-08-10 (late) — vista cooked; full recook renders; 2 open items

**Shipped this session (committed):**
- **Sea/backdrop `.ehf` vista cook** — `cook_levels.py _build_ehf()` + `--vista-ehf` flag (spec
  `ghidra_out/ehf_vista_re.txt`, validated on 3 real .ehf). Parses the 63-byte BE header (origin
  f0/f1, grid u0/u1, tile f2) + the pre-850A0 body float (flat surface height) via the exact
  EhfChunkParser skip-tex walk, emits a flat plane through the same {x,z,y} swap as terrain/water.
  chapter2slums `sea_vista_id_6ce5fa48.ehf` → 33×65 grid, origin (0,-128), tile 2.0, **height
  35.304** → 2145 verts filling game **X[0,64] Y[-128,0]**.
- **Definitive full recook + render** (`scratchpad/recook_full.sh`, screenshot
  `scratchpad/shots/native_full_vista.png`): 661 meshes / 6847 instances = props(1323) + terrain +
  water + **vista** + hero + 20 NPCs + 64 lights + 515/516 textures. Renders: castle, bridge, dense
  town, lit terrain, sky, the vista plane bridging the near-corner void.

**◑ OPEN ITEM 1 — castle-approach VOID: RESOLVED as far as the shipped data grounds (2026-08-11).**
INVESTIGATED the leads (data-driven): (b) only ONE vista tile exists (`sea_vista_id_6ce5fa48`, no others).
(a) MEASURED water-body extents: `slums.water` = sea/canals at h=36.6 over X[0,122] Y[0,225] + two h=48.6
ponds at X[243,259]; `sea_vista.water` = ONE body at the SAME sea level h=36.6 over X[0,66] Y[-128,2].
**No shipped water/prop geometry covers the void proper (X[66,288] Y[-128,0]) — that region is
backdrop/skybox, so a sea plane across it would be GUESSING the extent.** FIX (grounded, shipped data
only): `--water-file` is now REPEATABLE; cooking `sea_vista.water` alongside `slums.water` adds the real
seaward strip that partially bridges the near corner (screenshot-verified: translucent sea extends off the
island's SW edge toward the castle). The remaining gap toward the castle (X~66-288) is the environment
backdrop/skybox horizon, NOT missing cookable geometry. (c)/(d) moot — the vista/castle use identity
placement and the numbers above are from the real cooked scene.
Add `--water-file <sea_vista.water>` to the recook. Older analysis below (kept):

**WATER SHADING FOLLOW-UP (2026-08-12):** the cooker now preserves each `.water` body's normal-map path.
`globals_texture_headers.bnk` supplies the PF40/256×256 metadata needed to decode the header-backed
`waternormalmap01.tex` body as BC5. Both native backends sample the cooked normal map for the retail dual-scroll
normal/Fresnel path. The cooker now emits one material per authored `.water` body and preserves all 37 BE params;
the F2SCENE loader and both native backends bind those values per material, with the resolved 0.42 WaterTheme
opacity carried separately. Both native water PSOs now use the retail ONE/SRC_ALPHA composite and
`refr_k=(1-distf)*refl_str*(1-frefl)`; remaining approximation is the retail scene-depth edge factor.

**OPEN ITEM 1 (original measurements) — the castle-approach VOID is only partly filled.**
MEASURED world bounds (render space, `native_full_vista.f2scene`):
  - terrain: X[0,288] Z[0,288]   (from `slums.ghf`; the main render `.ehf` has the SAME extent: origin
    (0,0), 577×577, tile 0.5 → X[0,288] Y[0,288] — so the .ehf does NOT extend past the .ghf)
  - water:   X[0,258] Z[0,225]   (the town canals; `slums.water`)
  - vista:   X[0,64]  Z[-128,0]  (the small sea_vista corner)
  - castle + props: X[0,295] Z[**-92.7**,221.6]  ← the castle sits at render Z≈-92 (game Y≈-92)
  ⇒ **The void = game X[64,288] Y[-128,0]** — the whole southern band the castle sits over, which the
  small X[0,64] sea_vista corner does NOT cover. NOT guessing: the numbers above are from the cooked
  scene. NEXT-SESSION LEADS (data-driven, don't guess): (a) is the negative-Y region meant to be SEA?
  — we cooked `slums.water` (the town water, Y≥0) but NOT `sea_vista.water` (streaming.bnk #115, same
  stem, staged at `scratchpad/sea_vista.water`); cook it and/or extend a sea plane across game Y<0.
  (b) `f2tool list` for OTHER vista/backdrop `.ehf` under `worlds\albion\bwsslums\vistas\chapter2slums`
  — there may be more than one tile. (c) does the vista carry a world PLACEMENT transform (level/.save)
  rather than the identity we assumed? (d) does `bs_market_fairfaxcastle` include its own island/plinth?

**✅ OPEN ITEM 2 cause (1) FIXED (2026-08-11, D3D12, screenshot-verified) — terrain now has a real
ground albedo.** Cause (1) below (terrain = blown-out WHITE → buildings read "dark by contrast") is
resolved: the terrain cook now cooks the level's DOMINANT ground texture from the `.ehf` and tiles it.
- **How (data-driven, no guess):** new `f2tool ehf <file.ehf>` (source
  `Fable2AssetBrowser/source/tools/F2Tool.cpp` + its two parser sources added to the f2tool CMake
  target — REBUILD f2tool via cmake in the VS dev shell) dumps, via the authoritative
  `EhfChunkParser`, the per-LOD `strs0` base-diffuse names + the splat-coverage histogram.
  `cook_levels.py _terrain_ground_texture()` sums splat coverage per LOD (splat index = lod×17; 255 =
  unpainted-base sentinel → LOD 0 fallback) and picks the most-painted layer. For chapter2slums that is
  **LOD 0 `art\environment\_groundtextures\cobbles_curvy_dirt.tex` at 91% coverage** (cobbles+base).
  The texture cooks through the normal albedo pass (it lives in `1024mip0_textures.bnk`), tiled at its
  LOD `base_scale` (0.25), terrain `base_colour` set white.
- **New flag:** `--terrain-ehf <level main .ehf>` (the `ch2_heightfield_slums_*.ehf` in `streaming.bnk`,
  NOT the sea `sea_vista` .ehf). Render: cobble/dirt ground under the town, buildings sit naturally on it.
- **✅ RUNG 2 DONE (2026-08-11, D3D12, screenshot-verified) — full per-cell splat COMPOSITE.** The
  terrain now bakes the game's real ground painting (grass slopes / rocky peak / dirt basin / cobble /
  tan footpath), not one tiled texture. Default when `--terrain-ehf` is given (`--no-terrain-splat`
  falls back to Rung 1).
  - New standalone baker `Fable2Native/tools/terrain_splat_bake.cpp` (built by the AssetBrowser CMake as
    `terrain_splat_bake`, deps EhfChunkParser + TextureAtlasDecoder + zlib): a VERBATIM port of the
    AssetBrowser `LevelLoader.cpp` `BakeEhfTerrainCompositeWithBnk` splat path (~11040-11285) —
    `ParseEhfBody` → per-LOD DDS via `--lod i=<dds>` → `sample_mat` (LOD tiled by world pos at its
    base_scale) + `sample_mask` (splat) + per-chunk-layer blend → writes an uncompressed RGBA8 DDS +
    prints `BOUNDS minx minz spanx spanz`. CLI `terrain_splat_bake <ehf> <out.dds> --res N --lod i=<dds>...`.
  - `cook_levels.py _terrain_splat_composite()`: `f2tool ehf` → cook all 14 LOD diffuses → run the baker
    → attach the baked DDS as the terrain albedo (via `albedo_abs=` which bypasses the bnk cook) with
    **whole-terrain normalized UVs** ((gx-minx)/spanx) from the printed BOUNDS. Flags `--terrain-splat-res`
    (default 2048), `--no-terrain-splat`, `--splat-bake`.
  - ⚠ **GOTCHA (cost this session ~1h):** the baked albedo is an UNCOMPRESSED RGBA8 DDS; the runtime
    only gained that decode path on 2026-08-10 18:51 (commit cb32ab6). The **Release** `f2native_frontend.exe`
    was stale (17:02) → terrain rendered WHITE (silent load-fail → 1×1 white default; the renderer ignores
    `load_dds_rgba8`'s return). RelWithDebInfo (20:54) worked; Release has since been rebuilt. If terrain is
    white, REBUILD the frontend exe.
  - The EMBEDDED baked-albedo path (`DecodeEhfTerrainAlbedoFromBytes`) is a DEAD END for chapter2slums —
    all 3 decode paths fail (probe tool `Fable2Native/tools/terrain_bake.cpp`; the prior "large_ring"
    scratch was the same dead end). May work for other levels; the splat composite is the general answer.
  - **✅ CAUSE (2) — per-prop baked lighting (2026-08-11, D3D12, verified):** the `.lmp` LightmapFile is
    now RE'd + applied. It is a per-prop-instance baked-lighting probe table (gzip "LightmapFile"; tail =
    n×56-byte records = [8-byte PropInstance.hash][48-byte payload = 12 BE floats = order-1 SH per RGB,
    channel-major, DC-first]). Full spec + guest-loader addresses: `ghidra_out/lmp_lightmap_probes_re.txt`.
    `cook_levels.py --level-lmp <level.lmp>` keys each type-2 prop by its `PropInstance.hash` and emits the
    full 12-coeff probe on the F2SCENE `instance` line (`sh <12>`); the D3D12 world renderer evaluates it
    per-vertex (COLOR1) in place of the hemisphere floor (no-probe geometry unchanged). chapter2slums: 897
    probes, 41/~55 type-2 props matched; verified applied (38k px changed).
    **✅ EXACT EVAL RECOVERED (no guessing) — the full order-1 SH is now applied.** Disassembled the shipped
    Xenos shader `VSHADER_STANDARDMATERIAL_..._AMB2` (const `g_PRTConstants`, PRT ambient): the eval is
    `amb.c = C0 + (N.x*C1 + N.y*C2 + N.z*C3)` per channel, against the OBJECT-space normal in game axes, with
    **NO scale** (coeffs pre-folded at bake) — which is why the exe-scan found no SH constants. Full report +
    the reusable `shader_bank_extract` tool: `ghidra_out/prop_ambient_shader_re.txt`. The renderer un-swaps
    our stored `{x,z,y}` mesh normal to game axes and evaluates the exact formula. Effect is subtle (the baked
    probes for this level sit close to our synthetic hemisphere for wall orientations) but now byte-faithful
    to the game. Vulkan now uses the same probe evaluation, complete-scene camera/culling policy, Phase-0
    procedural sky gradient, and optional MSAA-safe reversed-Z depth resolve for the water shoreline;
    the current AMD driver supports `MAX`, while devices without the resolve extensions—or with
    optional resolve-image allocation failure—retain the shoreline-free fallback without aborting.

**OPEN ITEM 2 — the scene reads DARK — ROOT-CAUSED this session (see `ghidra_out/dark_props_diagnosis.txt`
+ its SESSION VERIFICATION footer).** The props are NOT actually black: a raw-albedo PS diagnostic
(`return albedo.Sample(uv)`, no lighting) showed the buildings FULLY TEXTURED (brick/stone/timber) — the
SRV bind is correct — and showed the **TERRAIN as solid WHITE** (its material has NO albedo → default 1×1
white). Ruled out by measurement: black DDS (bs_stone_wall mean (98,88,71)), dark base_colour (all 0.72),
zero normals (0/661 meshes), missing-texture (defaults WHITE not black), low ambient (a +0.35 fill didn't
brighten → base≈0 at those pixels). **REAL causes, ranked:** (1) **terrain has no albedo** → blown-out
white plane makes the correctly-textured mid-tone buildings read "dark by contrast" — FIX = cook the
terrain ground/splat albedo (`--terrain-ghf` emits geometry only); #1 visual win. (2) ambient floor 0.18 +
near-horizontal sun under-lights vertical walls — proper fix = bake the level `.lmp` LightmapFile (skipped
today) or lift ambient. **A latent renderer bug WAS found+fixed in passing:** material-texture cap 256→4096
(the draw bound SRVs beyond the 128 created for 661 materials — committed). ⚠ The background agent's
"just raise the cap" conclusion was DISPROVEN by the A/B measurements above — don't chase the cap for the
dark look.

**Terrain-albedo cook (add to the recook):** stage the level `.ehf`
(`streaming.bnk` → `worlds\albion\bwsslums\heightfields\chapter2slums\ch2_heightfield_slums_id_3a6902ed.ehf`)
and pass `--terrain-ehf <that .ehf>` alongside `--terrain-ghf`, with `1024mip0_textures.bnk` in the
`--textures-bnk` list. A fast isolated check: cook with `--types 99` (0 props) + `--terrain-stride 4`.
Render+screenshot: `scratchpad/world_shot.ps1 -Scene <f2scene> -Tag <t>` (launches
`f2native_frontend.exe --start-world --scene <f2scene> --game-dir <assets/game>`, PrintWindow capture).

**Recook command of record:** `scratchpad/recook_full.sh` (all inputs staged in `scratchpad/lvl/` +
`scratchpad/*.ehf/.water/.ghf` + game `Globals/`; tex-cook =
`Fable2Native/build/Release/f2native_cook_lh_tex.exe` — REBUILD it if `cook_lh_tex.cpp` changed, the
exe was stale this session). Screenshot: `scratchpad/shot_world.ps1 -Scene <f2scene> -Tag <t>`.

## Ground-truth references
`ghidra_out/world_level_format.txt`, `newgame_handoff.txt`, `gdb_instantiation_re.txt`,
`physics_collision_system.txt`, `hero_appearance_morph.txt`; `Fable2AssetBrowser/source/src/Level/
{LevelLoader.h,LevelLoader.cpp}` + `MDL/ModelParser`; `docs/NATIVE_PORT_PLAN.md` (install policy);
`Fable2Native/src/native_scene.cpp` (F2SCENE) + `native_cook.cpp` (cook orchestration).
